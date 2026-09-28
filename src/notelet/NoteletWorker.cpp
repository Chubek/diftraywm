#include "notelet/Notelet.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr size_t max_reply = 1024 * 1024;
void put(std::string &bytes, const std::string &value) {
  uint32_t length = value.size();
  bytes.append(reinterpret_cast<const char *>(&length), sizeof(length));
  bytes += value;
}
bool take(const std::string &bytes, size_t &offset, std::string &value) {
  uint32_t length;
  if (offset > bytes.size() || bytes.size() - offset < sizeof(length)) return false;
  std::memcpy(&length, bytes.data() + offset, sizeof(length));
  offset += sizeof(length);
  if (length > bytes.size() - offset) return false;
  value.assign(bytes, offset, length);
  offset += length;
  return true;
}
}

void Notelet::stop_worker() {
  if (read_fd_ >= 0) { close(read_fd_); read_fd_ = -1; }
  if (worker_ > 0) {
    // The worker is a separate process group: timed-out scripts cannot leave
    // synchronous standard-library subprocesses holding the result pipe open.
    kill(-worker_, SIGKILL);
    kill(worker_, SIGKILL);
    while (waitpid(worker_, nullptr, 0) < 0 && errno == EINTR) {}
    worker_ = -1;
  }
}

bool Notelet::start_worker(std::string &error) {
  if (worker_ > 0 || events_.empty()) return true;
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) != 0) { error = "notelet pipe failed"; return false; }
  const auto event = events_.front();
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]); close(pipefd[1]);
    error = "notelet worker failed"; return false;
  }
  if (pid == 0) {
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, nullptr);
    setpgid(0, 0);
    close(pipefd[0]);
    // Retain only the reply pipe. Never inherit compositor sockets or PTYs.
    if (pipefd[1] != 3) { dup2(pipefd[1], 3); close(pipefd[1]); }
    fcntl(3, F_SETFD, FD_CLOEXEC);
    close_range(4, ~0U, 0);
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
      for (int fd = 0; fd < 3; ++fd) dup2(nullfd, fd);
      if (nullfd > 3) close(nullfd);
    }
    struct rlimit cpu{1, 1};
    setrlimit(RLIMIT_CPU, &cpu);
    std::string failure, bytes;
    const bool ok = render(event.first, failure, event.second);
    put(bytes, ok ? "" : failure);
    put(bytes, frame_);
    if (ok) for (const auto &[key, value] : state_) { put(bytes, key); put(bytes, value); }
    size_t sent = 0;
    while (sent < bytes.size()) {
      ssize_t count = write(3, bytes.data() + sent, bytes.size() - sent);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) _exit(1);
      sent += count;
    }
    _exit(0);
  }
  setpgid(pid, pid);
  close(pipefd[1]);
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  worker_ = pid;
  read_fd_ = pipefd[0];
  worker_bytes_.clear();
  deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  events_.pop_front();
  return true;
}

bool Notelet::request_render(std::string key, std::string &error, std::string event) {
  error.clear();
  if (events_.size() >= 64) { error = "notelet input queue full"; return false; }
  events_.emplace_back(std::move(key), std::move(event));
  if (start_worker(error)) return true;
  events_.clear();
  return false;
}

bool Notelet::poll(std::string &error) {
  error.clear();
  if (worker_ <= 0) return false;
  bool eof = false;
  char buffer[8192];
  for (;;) {
    ssize_t count = read(read_fd_, buffer, sizeof(buffer));
    if (count > 0) {
      worker_bytes_.append(buffer, static_cast<size_t>(count));
      if (worker_bytes_.size() > max_reply) { error = "notelet reply too large"; break; }
    } else if (count == 0) { eof = true; break; }
    else if (errno == EINTR) continue;
    else if (errno == EAGAIN) break;
    else { error = "notelet worker read failed"; break; }
  }
  if (!eof && error.empty() && std::chrono::steady_clock::now() < deadline_) return false;
  if (!eof && error.empty()) error = "notelet execution timed out";
  stop_worker();
  if (error.empty()) {
    size_t offset = 0;
    std::string failure, frame;
    std::map<std::string, std::string> state;
    bool valid = take(worker_bytes_, offset, failure) && take(worker_bytes_, offset, frame);
    while (valid && offset < worker_bytes_.size()) {
      std::string key, value;
      valid = take(worker_bytes_, offset, key) && take(worker_bytes_, offset, value);
      if (valid) state.emplace(std::move(key), std::move(value));
    }
    if (!valid) error = "notelet worker returned an incomplete frame";
    else if (!failure.empty()) error = failure;
    else { frame_ = std::move(frame); state_ = std::move(state); }
  }
  if (!error.empty()) events_.clear();
  else start_worker(error);
  return true;
}
