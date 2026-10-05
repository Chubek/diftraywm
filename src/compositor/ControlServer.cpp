#include "compositor/ControlServer.hpp"

#include <wayland-server-core.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
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
  const std::string requested_path = path;
  stop();
  if (!display) {
    error_ = "control server requires a display";
    return false;
  }
  display_ = display;
  dispatcher_ = std::move(dispatcher);
  path_ = requested_path;
  if (path_.empty() || path_.size() >= sizeof(sockaddr_un::sun_path)) {
    error_ = "control socket path is empty or too long";
    return false;
  }
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (listen_fd_ < 0) {
    error_ = std::string("cannot create control socket: ") + std::strerror(errno);
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path_.c_str(), path_.size());
  struct stat existing{};
  if (::lstat(path_.c_str(), &existing) == 0) {
    // Never remove another session's live endpoint or an unrelated file.
    if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) {
      error_ = "control socket path is occupied: " + path_;
      stop();
      return false;
    }
    const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (probe < 0) {
      error_ = "cannot check existing control socket";
      stop();
      return false;
    }
    const int connected = ::connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    const int probe_error = errno;
    ::close(probe);
    if (connected == 0 || (probe_error != ECONNREFUSED && probe_error != ENOENT)) {
      error_ = "control socket already in use: " + path_;
      stop();
      return false;
    }
    struct stat current{};
    if (::lstat(path_.c_str(), &current) == 0 &&
        current.st_dev == existing.st_dev && current.st_ino == existing.st_ino) {
      if (::unlink(path_.c_str()) != 0) {
        error_ = "cannot remove stale control socket: " + path_;
        stop();
        return false;
      }
    }
  } else if (errno != ENOENT) {
    error_ = "cannot inspect control socket: " + path_;
    stop();
    return false;
  }
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
  owns_socket_ = true;
  struct stat node{};
  if (::lstat(path_.c_str(), &node) == 0) {
    socket_device_ = node.st_dev;
    socket_inode_ = node.st_ino;
  }
  if (::listen(listen_fd_, 8) != 0) {
    error_ = "cannot listen on control socket: " + std::string(std::strerror(errno));
    stop();
    return false;
  }
  listen_source_ = wl_event_loop_add_fd(wl_display_get_event_loop(display_), listen_fd_,
                                        WL_EVENT_READABLE, &ControlServer::on_accept, this);
  if (!listen_source_) {
    error_ = "cannot watch the control socket";
    stop();
    return false;
  }
  timeout_source_ = wl_event_loop_add_timer(wl_display_get_event_loop(display_),
                                           &ControlServer::on_timeout, this);
  if (!timeout_source_) {
    error_ = "cannot watch control connection deadlines";
    stop();
    return false;
  }
  wl_event_source_timer_update(timeout_source_, 1000);
  error_.clear();
  return true;
}

void ControlServer::stop() {
  for (auto &entry : clients_) {
    if (entry.second.source) wl_event_source_remove(entry.second.source);
    if (entry.first >= 0) ::close(entry.first);
  }
  clients_.clear();
  if (timeout_source_) {
    wl_event_source_remove(timeout_source_);
    timeout_source_ = nullptr;
  }
  if (listen_source_) {
    wl_event_source_remove(listen_source_);
    listen_source_ = nullptr;
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (!path_.empty()) {
    struct stat node{};
    if (owns_socket_ && ::lstat(path_.c_str(), &node) == 0 &&
        node.st_dev == socket_device_ && node.st_ino == socket_inode_) {
      ::unlink(path_.c_str());
    }
    path_.clear();
  }
  owns_socket_ = false;
  dispatcher_ = {};
  display_ = nullptr;
}

int ControlServer::on_accept(int, uint32_t, void *data) {
  auto *self = static_cast<ControlServer *>(data);
  // Leave queued connections for the next callback after a bounded batch.
  for (unsigned accepted = 0; accepted < 64; ++accepted) {
    const int fd = ::accept4(self->listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
      if (errno == EINTR) continue;
      // EAGAIN simply means the last queued connection is already accepted.
      return 0;
    }
    if (self->clients_.size() >= 32) {
      // Refuse rather than grow without bound; a well-behaved client retries.
      const std::string busy = "-\ndiftraywm: too many control connections\n";
      ::send(fd, busy.data(), busy.size(), MSG_NOSIGNAL);
      ::close(fd);
      continue;
    }
    Client client;
    client.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
    client.source = wl_event_loop_add_fd(wl_display_get_event_loop(self->display_), fd,
                                         WL_EVENT_READABLE, &ControlServer::on_client, self);
    if (!client.source) {
      ::close(fd);
      continue;
    }
    self->clients_.emplace(fd, std::move(client));
  }
  return 0;
}

int ControlServer::on_client(int fd, uint32_t mask, void *data) {
  auto *self = static_cast<ControlServer *>(data);
  const auto entry = self->clients_.find(fd);
  if (entry == self->clients_.end()) {
    return 0;
  }
  if (mask & WL_EVENT_ERROR) {
    self->close_client(fd);
    return 0;
  }
  if (entry->second.replying) {
    self->flush_reply(fd);
    return 0;
  }
  char buffer[4096];
  bool peer_gone = false;
  for (;;) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      entry->second.buffer.append(buffer, static_cast<std::size_t>(count));
      if (entry->second.buffer.size() > kMaxRequestBytes ||
          entry->second.buffer.find('\n') != std::string::npos) break;
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
    self->queue_reply(fd, "-\ndiftraywm: control request too large\n");
    return 0;
  }
  const std::size_t newline = entry->second.buffer.find('\n');
  if (newline == std::string::npos) {
    if (peer_gone) {
      self->queue_reply(fd, "-\ndiftraywm: incomplete control request\n");
    }
    return 0;  // Wait for the rest of the request.
  }
  std::string line = entry->second.buffer.substr(0, newline);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  self->handle_line(fd, line);
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
  queue_reply(fd, std::string(1, accepted ? '+' : '-') + "\n" + body + "\n");
}

void ControlServer::queue_reply(int fd, std::string payload) {
  const auto entry = clients_.find(fd);
  if (entry == clients_.end()) return;
  auto &client = entry->second;
  client.reply = std::move(payload);
  client.buffer.clear();
  client.replying = true;
  client.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
  wl_event_source_fd_update(client.source, WL_EVENT_WRITABLE);
  flush_reply(fd);
}

void ControlServer::flush_reply(int fd) {
  const auto entry = clients_.find(fd);
  if (entry == clients_.end()) return;
  auto &client = entry->second;
  // Bound work per callback so a large status response cannot monopolise frames.
  std::size_t budget = kMaxRequestBytes;
  while (client.sent < client.reply.size() && budget > 0) {
    const std::size_t count = std::min(budget, client.reply.size() - client.sent);
    const ssize_t sent = ::send(fd, client.reply.data() + client.sent, count, MSG_NOSIGNAL);
    if (sent > 0) {
      client.sent += static_cast<std::size_t>(sent);
      budget -= static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    close_client(fd);
    return;
  }
  if (client.sent == client.reply.size()) close_client(fd);
}

int ControlServer::on_timeout(void *data) {
  auto *self = static_cast<ControlServer *>(data);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = self->clients_.begin(); it != self->clients_.end();) {
    const int fd = it->first;
    const bool expired = it->second.deadline <= now;
    ++it;
    if (expired) self->close_client(fd);
  }
  wl_event_source_timer_update(self->timeout_source_, 1000);
  return 0;
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
