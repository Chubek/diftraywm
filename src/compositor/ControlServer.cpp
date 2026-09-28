#include "compositor/ControlServer.hpp"

#include <wayland-server-core.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
// A client that connects and stalls must not wedge the compositor, so reads
// and writes give up after this long. diftrayctl always sends its command
// immediately, so the timeout only ever trips on a broken peer.
constexpr int kIoTimeoutMs = 5000;
constexpr std::size_t kMaxRequestBytes = 64 * 1024;
}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(wl_display *display, const std::string &path,
                          Dispatcher dispatcher) {
  display_ = display;
  dispatcher_ = std::move(dispatcher);
  path_ = path;
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
    error_ = "control socket path is empty or too long";
    return false;
  }
  // A previous crash can leave the node behind; the live socket is the only
  // thing that matters, and binding an unlinked path is the usual recovery.
  ::unlink(path_.c_str());
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    error_ = std::string("cannot create control socket: ") + std::strerror(errno);
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path_.c_str(), path_.size());
  // A session-private control socket is a security boundary: it can restart the
  // compositor and load arbitrary code, so it stays owner-only.
  const mode_t previous = ::umask(0077);
  const bool bound = ::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address),
                            sizeof(address)) == 0;
  ::umask(previous);
  if (!bound) {
    error_ = "cannot bind control socket " + path_ + ": " + std::strerror(errno);
    stop();
    return false;
  }
  if (::listen(listen_fd_, 8) != 0) {
    error_ = "cannot listen on control socket: " + std::string(std::strerror(errno));
    stop();
    return false;
  }
  const int flags = ::fcntl(listen_fd_, F_GETFL, 0);
  if (flags >= 0) ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
  listen_source_ = wl_event_loop_add_fd(wl_display_get_event_loop(display_), listen_fd_,
                                        WL_EVENT_READABLE, &ControlServer::on_accept, this);
  if (!listen_source_) {
    error_ = "cannot watch the control socket";
    stop();
    return false;
  }
  error_.clear();
  return true;
}

void ControlServer::stop() {
  for (auto &entry : clients_) {
    if (entry.second.source) wl_event_source_remove(entry.second.source);
    if (entry.first >= 0) ::close(entry.first);
  }
  clients_.clear();
  if (listen_source_) {
    wl_event_source_remove(listen_source_);
    listen_source_ = nullptr;
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (!path_.empty()) {
    ::unlink(path_.c_str());
    path_.clear();
  }
  display_ = nullptr;
}

int ControlServer::on_accept(int, uint32_t, void *data) {
  auto *self = static_cast<ControlServer *>(data);
  for (;;) {
    const int fd = ::accept4(self->listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      // EAGAIN simply means the last queued connection is already accepted.
      return 0;
    }
    if (self->clients_.size() >= 32) {
      // Refuse rather than grow without bound; a well-behaved client retries.
      const std::string busy = "diftraywm: too many control connections\n";
      ::write(fd, busy.data(), busy.size());
      ::close(fd);
      continue;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    pollfd timeout{fd, 0, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    Client client;
    client.source = wl_event_loop_add_fd(wl_display_get_event_loop(self->display_), fd,
                                         WL_EVENT_READABLE, &ControlServer::on_client, self);
    if (!client.source) {
      ::close(fd);
      continue;
    }
    self->clients_.emplace(fd, std::move(client));
  }
}

int ControlServer::on_client(int fd, uint32_t, void *data) {
  auto *self = static_cast<ControlServer *>(data);
  const auto entry = self->clients_.find(fd);
  if (entry == self->clients_.end()) {
    return 0;
  }
  char buffer[4096];
  bool peer_gone = false;
  for (;;) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      entry->second.buffer.append(buffer, static_cast<std::size_t>(count));
      if (entry->second.buffer.find('\n') != std::string::npos) break;
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    // EOF or a hard error. diftrayctl half-closes after its request, so this
    // is the normal end of a command: the buffer still has to be examined for
    // a complete line before the connection is dropped.
    peer_gone = true;
    break;
  }
  if (entry->second.buffer.size() > kMaxRequestBytes) {
    write_all(fd, "diftraywm: control request too large\n");
    self->close_client(fd);
    return 0;
  }
  const std::size_t newline = entry->second.buffer.find('\n');
  if (newline == std::string::npos) {
    if (peer_gone) {
      write_all(fd, "diftraywm: incomplete control request\n");
      self->close_client(fd);
    }
    return 0;  // Wait for the rest of the request.
  }
  std::string line = entry->second.buffer.substr(0, newline);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  self->handle_line(fd, line);
  self->close_client(fd);
  return 0;
}

void ControlServer::handle_line(int fd, const std::string &line) {
  // The first line is a one-character verdict so diftrayctl can set a reliable
  // exit status; the rest is the human-readable status line, which may span
  // several lines (output list, mux list, session status).
  bool accepted = false;
  std::string body = "control dispatcher unavailable";
  if (dispatcher_) {
    auto result = dispatcher_(line);
    accepted = result.first;
    body = std::move(result.second);
  }
  if (body.empty()) {
    body = "ok";
  }
  // The reply is written before the socket closes, so commands that unwind the
  // event loop still report their outcome to diftrayctl.
  write_all(fd, std::string(1, accepted ? '+' : '-') + "\n" + body + "\n");
}

void ControlServer::write_all(int fd, const std::string &payload) {
  std::size_t sent = 0;
  while (sent < payload.size()) {
    const ssize_t count = ::write(fd, payload.data() + sent, payload.size() - sent);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd waiter{fd, POLLOUT, 0};
      if (::poll(&waiter, 1, kIoTimeoutMs) > 0) continue;
    }
    return;
  }
}

void ControlServer::close_client(int fd) {
  const auto entry = clients_.find(fd);
  if (entry == clients_.end()) {
    return;
  }
  if (entry->second.source) {
    wl_event_source_remove(entry->second.source);
  }
  ::close(fd);
  clients_.erase(entry);
}
