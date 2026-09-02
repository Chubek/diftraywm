#include "nterm/NTerm.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

NTerm::NTerm(std::string shell_override) : shell_path_(std::move(shell_override)) {
  if (shell_path_.empty()) {
    const char *environment_shell = std::getenv("DIFTRAYWM_SHELL");
    shell_path_ = environment_shell && *environment_shell ? environment_shell : "/bin/sh";
  }
}

NTerm::~NTerm() { stop(); }

bool NTerm::start() {
  if (running_) {
    return true;
  }
  last_error_.clear();
  if (::access(shell_path_.c_str(), X_OK) != 0) {
    last_error_ = "shell is not executable: " + shell_path_;
    return false;
  }

  struct winsize size {};
  size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(columns_, 65535));
  size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(rows_, 65535));
  const pid_t pid = ::forkpty(&master_fd_, nullptr, nullptr, &size);
  if (pid < 0) {
    master_fd_ = -1;
    last_error_ = std::string("forkpty failed: ") + std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    const char *name = std::strrchr(shell_path_.c_str(), '/');
    name = name ? name + 1 : shell_path_.c_str();
    ::execl(shell_path_.c_str(), name, "-i", static_cast<char *>(nullptr));
    _exit(127);
  }

  child_pid_ = pid;
  const int flags = ::fcntl(master_fd_, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
  }
  running_ = true;
  return true;
}

void NTerm::stop() {
  if (master_fd_ >= 0) {
    ::close(master_fd_);
    master_fd_ = -1;
  }
  if (child_pid_ > 0) {
    int status = 0;
    if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
      ::kill(child_pid_, SIGHUP);
      ::waitpid(child_pid_, &status, 0);
    }
    child_pid_ = -1;
  }
  running_ = false;
}

bool NTerm::running() const { return running_; }
const std::string &NTerm::last_error() const { return last_error_; }

void NTerm::on_readable() {
  if (master_fd_ < 0) {
    if (!input_buffer_.empty()) {
      scrollback_.push_back(input_buffer_);
      input_buffer_.clear();
    }
    return;
  }
  char buffer[4096];
  for (;;) {
    const ssize_t count = ::read(master_fd_, buffer, sizeof(buffer));
    if (count > 0) {
      append_output(std::string_view(buffer, static_cast<std::size_t>(count)));
      continue;
    }
    if (count < 0 && (errno == EINTR)) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    if (count == 0 || (count < 0 && (errno != EAGAIN && errno != EWOULDBLOCK))) {
      running_ = false;
      break;
    }
  }
}

void NTerm::resize(std::size_t columns, std::size_t rows) {
  columns_ = std::max<std::size_t>(1, columns);
  rows_ = std::max<std::size_t>(1, rows);
}

void NTerm::feed_input(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }
  input_buffer_.append(bytes);
  if (master_fd_ < 0) {
    return;
  }
  const char *data = bytes.data();
  std::size_t remaining = bytes.size();
  while (remaining > 0) {
    const ssize_t count = ::write(master_fd_, data, remaining);
    if (count > 0) {
      data += count;
      remaining -= static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}

void NTerm::append_output(std::string_view bytes) { scrollback_.emplace_back(bytes); }

void NTerm::set_shell_path(std::string shell_path) {
  if (!shell_path.empty()) {
    if (running_) {
      stop();
    }
    shell_path_ = std::move(shell_path);
  }
}
int NTerm::master_fd() const { return master_fd_; }
pid_t NTerm::child_pid() const { return child_pid_; }
const std::string &NTerm::shell_path() const { return shell_path_; }
std::size_t NTerm::columns() const { return columns_; }
std::size_t NTerm::rows() const { return rows_; }
const std::deque<std::string> &NTerm::scrollback() const { return scrollback_; }
const std::string &NTerm::input_buffer() const { return input_buffer_; }
