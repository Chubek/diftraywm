#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

struct tsm_screen;
struct tsm_vte;
struct _vterm_s;

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
  // Render compositor-owned terminal content without writing to a PTY.
  void display(std::string_view bytes);
  bool handle_key(uint32_t keysym, uint32_t ascii, unsigned int mods,
                  uint32_t unicode);
  void set_shell_path(std::string shell_path);
  int master_fd() const;
  pid_t child_pid() const;
  const std::string &shell_path() const;
  std::size_t columns() const;
  std::size_t rows() const;
  tsm_screen *screen() const;
  bool consume_dirty();
  bool owns_pid(pid_t pid) const;

private:
  static void vte_write(tsm_vte *vte, const char *u8, size_t len, void *data);
  bool spawn_shell();
  void feed_parsers(std::string_view bytes);

  void *ptytty_handle_ = nullptr;
  int master_fd_ = -1;
  pid_t child_pid_ = -1;
  std::string shell_path_;
  tsm_screen *screen_ = nullptr;
  tsm_vte *vte_ = nullptr;
  struct _vterm_s *vterm_ = nullptr;
  std::size_t columns_ = 80;
  std::size_t rows_ = 24;
  std::string last_error_;
  bool running_ = false;
  bool dirty_ = true;
};
