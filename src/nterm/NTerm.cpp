#include "nterm/NTerm.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include <libptytty.h>
#include <libtsm.h>
extern "C" {
#include <vterm.h>
}

ptytty *pty_handle(void *ptr) { return static_cast<ptytty *>(ptr); }

void NTerm::vte_write(tsm_vte *, const char *u8, size_t len, void *data) {
  auto *self = static_cast<NTerm *>(data);
  if (self) {
    self->feed_input(std::string_view(u8, len));
  }
}

NTerm::NTerm(std::string shell_override) : shell_path_(std::move(shell_override)) {
  if (shell_path_.empty()) {
    const char *environment_shell = std::getenv("DIFTRAYWM_SHELL");
    shell_path_ = environment_shell && *environment_shell ? environment_shell : "/bin/sh";
  }
  tsm_screen_new(&screen_, nullptr, nullptr);
  if (screen_) {
    tsm_screen_resize(screen_, static_cast<unsigned>(columns_),
                      static_cast<unsigned>(rows_));
    tsm_screen_set_max_sb(screen_, 2000);
    tsm_vte_new(&vte_, screen_, &NTerm::vte_write, this, nullptr, nullptr);
  }
  vterm_ = vterm_create(static_cast<uint16_t>(columns_),
                        static_cast<uint16_t>(rows_),
                        VTERM_FLAG_NOPTY | VTERM_FLAG_NOCURSES | VTERM_FLAG_XTERM_256);
}

NTerm::~NTerm() {
  stop();
  if (vterm_) {
    vterm_destroy(vterm_);
    vterm_ = nullptr;
  }
  if (vte_) {
    tsm_vte_unref(vte_);
    vte_ = nullptr;
  }
  if (screen_) {
    tsm_screen_unref(screen_);
    screen_ = nullptr;
  }
}

bool NTerm::spawn_shell() {
  last_error_.clear();
  if (::access(shell_path_.c_str(), X_OK) != 0) {
    last_error_ = "shell is not executable: " + shell_path_;
    return false;
  }
  ptytty::init();
  ptytty_handle_ = ptytty::create();
  if (!ptytty_handle_ || !pty_handle(ptytty_handle_)->get()) {
    last_error_ = "libptytty failed to allocate a PTY";
    delete pty_handle(ptytty_handle_);
    ptytty_handle_ = nullptr;
    return false;
  }
  master_fd_ = pty_handle(ptytty_handle_)->pty;
  const int slave = pty_handle(ptytty_handle_)->tty;
  struct winsize size {};
  size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(columns_, 65535));
  size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(rows_, 65535));
  if (master_fd_ >= 0) {
    ::ioctl(master_fd_, TIOCSWINSZ, &size);
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    last_error_ = std::string("fork failed: ") + std::strerror(errno);
    delete pty_handle(ptytty_handle_);
    ptytty_handle_ = nullptr;
    master_fd_ = -1;
    return false;
  }
  if (pid == 0) {
    pty_handle(ptytty_handle_)->make_controlling_tty();
    if (slave >= 0) {
      ::dup2(slave, STDIN_FILENO);
      ::dup2(slave, STDOUT_FILENO);
      ::dup2(slave, STDERR_FILENO);
    }
    if (master_fd_ >= 0 && master_fd_ > STDERR_FILENO) {
      ::close(master_fd_);
    }
    if (slave > STDERR_FILENO) {
      ::close(slave);
    }
    const char *name = std::strrchr(shell_path_.c_str(), '/');
    name = name ? name + 1 : shell_path_.c_str();
    ::execl(shell_path_.c_str(), name, "-i", static_cast<char *>(nullptr));
    _exit(127);
  }
  child_pid_ = pid;
  pty_handle(ptytty_handle_)->close_tty();
  const int flags = ::fcntl(master_fd_, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
  }
  running_ = true;
  dirty_ = true;
  return true;
}

bool NTerm::start() {
  if (running_) {
    return true;
  }
  return spawn_shell();
}

void NTerm::stop() {
  if (master_fd_ >= 0) {
    master_fd_ = -1;
  }
  if (ptytty_handle_) {
    delete pty_handle(ptytty_handle_);
    ptytty_handle_ = nullptr;
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

void NTerm::feed_parsers(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }
  if (vte_) {
    tsm_vte_input(vte_, bytes.data(), bytes.size());
  }
  if (vterm_) {
    std::vector<char> copy(bytes.begin(), bytes.end());
    vterm_render(vterm_, copy.data(), static_cast<int>(copy.size()));
  }
  dirty_ = true;
}

void NTerm::display(std::string_view bytes) { feed_parsers(bytes); }

void NTerm::on_readable() {
  if (master_fd_ < 0) {
    return;
  }
  char buffer[4096];
  for (;;) {
    const ssize_t count = ::read(master_fd_, buffer, sizeof(buffer));
    if (count > 0) {
      feed_parsers(std::string_view(buffer, static_cast<std::size_t>(count)));
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    running_ = false;
    break;
  }
}

void NTerm::resize(std::size_t columns, std::size_t rows) {
  columns_ = std::max<std::size_t>(1, columns);
  rows_ = std::max<std::size_t>(1, rows);
  if (screen_) {
    tsm_screen_resize(screen_, static_cast<unsigned>(columns_),
                      static_cast<unsigned>(rows_));
  }
  if (vterm_) {
    vterm_resize_full(vterm_, static_cast<uint16_t>(columns_),
                      static_cast<uint16_t>(rows_), 0, 0, 0, 0);
  }
  if (master_fd_ >= 0) {
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(columns_);
    size.ws_row = static_cast<unsigned short>(rows_);
    ::ioctl(master_fd_, TIOCSWINSZ, &size);
  }
  dirty_ = true;
}

void NTerm::feed_input(std::string_view bytes) {
  if (bytes.empty() || master_fd_ < 0) {
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

bool NTerm::handle_key(uint32_t keysym, uint32_t ascii, unsigned int mods,
                       uint32_t unicode) {
  if (!vte_) {
    return false;
  }
  return tsm_vte_handle_keyboard(vte_, keysym, ascii, mods, unicode);
}

void NTerm::set_shell_path(std::string shell_path) {
  if (shell_path.empty()) {
    return;
  }
  if (running_) {
    stop();
  }
  shell_path_ = std::move(shell_path);
}

int NTerm::master_fd() const { return master_fd_; }
pid_t NTerm::child_pid() const { return child_pid_; }
const std::string &NTerm::shell_path() const { return shell_path_; }
std::size_t NTerm::columns() const { return columns_; }
std::size_t NTerm::rows() const { return rows_; }
tsm_screen *NTerm::screen() const { return screen_; }

bool NTerm::consume_dirty() {
  const bool dirty = dirty_;
  dirty_ = false;
  return dirty;
}

bool NTerm::owns_pid(pid_t pid) const {
  if (pid <= 0 || child_pid_ <= 0) {
    return false;
  }
  pid_t current = pid;
  for (int depth = 0; depth < 32 && current > 1; ++depth) {
    if (current == child_pid_) {
      return true;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(current));
    FILE *file = std::fopen(path, "r");
    if (!file) {
      return false;
    }
    int id = 0;
    char comm[256];
    char state = 0;
    int ppid = 0;
    const int matched = std::fscanf(file, "%d %255s %c %d", &id, comm, &state, &ppid);
    std::fclose(file);
    if (matched != 4 || ppid <= 0) {
      return false;
    }
    current = ppid;
  }
  return false;
}
