#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

struct tsm_screen;
struct tsm_vte;
struct vterm_s;
using vterm_t = vterm_s;
struct wl_event_source;

class NTerm {
public:
  explicit NTerm(std::string shell_override = {});
  ~NTerm();

  bool start();
  void stop();
  bool running() const;
  const std::string &last_error() const;
  void on_readable();
  void resize(std::size_t columns, std::size_t rows);
  void feed_input(std::string_view bytes);
  void append_output(std::string_view bytes);
  void set_shell_path(std::string shell_path);
  int master_fd() const;
  pid_t child_pid() const;
  const std::string &shell_path() const;
  std::size_t columns() const;
  std::size_t rows() const;
  const std::deque<std::string> &scrollback() const;
  const std::string &input_buffer() const;

private:
  int master_fd_ = -1;
  pid_t child_pid_ = -1;
  std::string shell_path_;
  tsm_screen *screen_ = nullptr;
  tsm_vte *vte_ = nullptr;
  vterm_t *vterm_ = nullptr;
  wl_event_source *readable_source_ = nullptr;
  std::size_t columns_ = 80;
  std::size_t rows_ = 24;
  std::deque<std::string> scrollback_;
  std::string input_buffer_;
  std::string last_error_;
  bool running_ = false;
};
