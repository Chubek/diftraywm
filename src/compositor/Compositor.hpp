#pragma once

#include "command/CommandBar.hpp"
#include "command/CommandContext.hpp"
#include "config/Config.hpp"
#include "input/KeyboardHandler.hpp"
#include "nterm/NTermRenderer.hpp"

#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct wl_display;
struct wl_event_source;
struct wlr_xdg_toplevel;
struct diftray_wayland_runtime;
struct diftray_cell_surface;

class View;
class Cell;
class NCursorView;
class GCursorView;
class TCursorView;
struct Output;
struct Input;
class ThemeEngine;
class PluginManager;
class LuaEngine;
class NoteletCatalog;
class Notelet;

struct WlDisplayDeleter {
  void operator()(wl_display *ptr) const noexcept;
};

class Compositor {
public:
  Compositor();
  ~Compositor();

  bool init();
  int run();
  void stop();
  void set_active_view(View *view);
  std::string spawn_cell(bool above);
  std::string kill_selected_cell();
  std::string move_selected_cell(int delta);
  std::string toggle_cell_select_mode();
  std::string focus_active_cell();
  std::string promote_active_cell_to_tcursor();
  std::string restore_tcursor();
  std::string dock_cursor(const std::string &id);
  std::string restore_cursor(const std::string &id);
  std::string list_cursor_ids() const;
  std::string list_docked_cursors() const;
  std::string assign_cursor_slot(const std::string &id, int slot);
  std::string restore_quick_slot(int slot);
  std::string set_shell_override(const std::string &shell, CommandScope scope);
  std::string apply_theme_css(const std::string &css);
  std::string load_theme_file(const std::string &path);
  std::string launch_program(const std::string &command);
  std::string list_notelets() const;
  std::string open_notelet(const std::string &id);
  std::string close_notelet();
  std::string spawn_ncursor();
  bool handle_key(uint32_t keysym, uint32_t modifiers, uint32_t state,
                  uint32_t unicode);
  std::string cycle_tab(int delta);
  std::string switch_workspace(int number);
  int current_workspace() const { return current_workspace_; }
  void relayout();

  wl_display *display() const { return display_.get(); }
  CommandBar &command_bar() { return command_bar_; }
  const CommandBar &command_bar() const { return command_bar_; }
  const std::string &status_line() const { return status_line_; }

private:
  Cell *active_cell() const;
  NCursorView *ncursor_view() const;
  GCursorView *active_gcursor() const;
  GCursorView *find_gcursor(const std::string &id) const;
  GCursorView *find_gcursor(wlr_xdg_toplevel *toplevel) const;
  void attach_cell_surface(Cell *cell);
  void detach_cell_surface(Cell *cell);
  void render_cell(Cell *cell, bool selected);
  void paint_notelet(Cell *cell);
  void render_all_cells();
  void apply_view_visibility();
  void update_chrome();
  void watch_cell_pty(Cell *cell);
  void unwatch_cell_pty(Cell *cell);
  static int terminal_fd_ready(int fd, uint32_t mask, void *data);
  static bool terminal_key_received(void *userdata, uint32_t keysym,
                                    uint32_t modifiers, uint32_t state,
                                    uint32_t unicode, uint32_t time_msec,
                                    uint32_t keycode);
  static void handle_new_toplevel(void *userdata, wlr_xdg_toplevel *toplevel,
                                  int client_pid);
  static void handle_toplevel_destroy(void *userdata,
                                      wlr_xdg_toplevel *toplevel);
  static void handle_toplevel_focus(void *userdata,
                                    wlr_xdg_toplevel *toplevel);
  static void handle_toplevel_request(void *userdata,
                                      wlr_xdg_toplevel *toplevel,
                                      const char *request);
  static void handle_output_geometry(void *userdata, int width, int height);
  void on_new_toplevel(wlr_xdg_toplevel *toplevel, int client_pid);
  void on_toplevel_destroy(wlr_xdg_toplevel *toplevel);
  void on_output_geometry(int width, int height);
  void rebuild_command_context();
  std::string erase_cell(Cell *cell);
  unsigned int tsm_mods(uint32_t modifiers) const;
  void open_command_bar(CommandScope scope, const std::string &prefix);
  void close_command_bar();
  bool feed_command_bar_key(uint32_t keysym, uint32_t unicode);

  static constexpr int kMinWorkspace = 1;
  static constexpr int kMaxWorkspace = 10;

  NCursorView *owner_ncursor(const Cell *cell) const;
  std::vector<NCursorView *> ncursors_on_workspace(int workspace) const;
  std::vector<GCursorView *> live_gcursors_on_workspace(int workspace) const;
  bool view_exists(const View *view) const;
  void remember_workspace_focus();
  void restore_workspace_focus();

  std::unique_ptr<wl_display, WlDisplayDeleter> display_;
  std::vector<std::unique_ptr<NCursorView>> ncursor_views_;
  std::vector<std::unique_ptr<Cell>> cells_;
  std::vector<std::unique_ptr<GCursorView>> gcursors_;
  std::unique_ptr<TCursorView> tcursor_view_;
  CommandBar command_bar_;
  CommandContext command_context_;
  KeyboardHandler keyboard_handler_;
  NTermRenderer nterm_renderer_;
  std::string status_line_;
  ThemeEngine *theme_engine_ = nullptr;
  PluginManager *plugin_manager_ = nullptr;
  LuaEngine *lua_engine_ = nullptr;
  std::unique_ptr<NoteletCatalog> notelet_catalog_;
  std::unordered_map<Cell *, std::unique_ptr<Notelet>> notelet_cells_;
  View *active_view_ = nullptr;
  NCursorView *active_ncursor_ = nullptr;
  bool running_ = false;
  std::unordered_map<int, Cell *> pty_cells_;
  std::unordered_map<int, wl_event_source *> pty_sources_;
  std::string wayland_socket_;
  diftray_wayland_runtime *wayland_runtime_ = nullptr;
  CompositorConfig config_;
  int output_width_ = 1280;
  int output_height_ = 720;
  bool command_bar_open_ = false;
  bool launcher_mode_ = false;
  GCursorView *quick_restore_[4] = {nullptr, nullptr, nullptr, nullptr};
  int current_workspace_ = 1;
  int tcursor_workspace_ = 0;
  NCursorView *workspace_ncursor_[kMaxWorkspace + 1] = {};
  View *workspace_view_[kMaxWorkspace + 1] = {};
};
