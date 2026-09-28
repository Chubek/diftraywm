#pragma once

#include "command/CommandBar.hpp"
#include "compositor/Output.hpp"
#include <map>
#include "command/CommandContext.hpp"
#include "config/Config.hpp"
#include "input/KeyboardHandler.hpp"
#include "help/HelpPager.hpp"
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
  friend struct CompositorTestAccess;
public:
  Compositor();
  ~Compositor();

  bool init();
  void notify_extensions(const std::string &event);
  void flush_extension_commands();
  int run();
  void stop();
  void set_active_view(View *view);
  std::string spawn_cell(bool above);
  std::string kill_selected_cell();
  std::string move_selected_cell(int delta);
  std::string move_cell(const std::string &id, const std::string &destination);
  std::string move_cursor(const std::string &id, const std::string &kind, const std::string &destination);
  std::string list_views() const;

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
  std::string refresh_notelet();
  std::string open_help_page(const std::string &topic);
  std::string find_help(const std::string &pattern);
  std::string set_help_bookmark(const std::string &name);
  std::string open_help_bookmark(const std::string &name);
  bool help_pager_active() const { return help_pager_active_; }
  const std::string &help_page_name() const { return help_pager_.page_name(); }
  std::string spawn_ncursor();
  std::string evaluate_config(const std::string &expression, bool run);
  std::string config_variables() const;
  bool handle_key(uint32_t keysym, uint32_t modifiers, uint32_t state,
                  uint32_t unicode, uint32_t keycode = UINT32_MAX);
  std::string cycle_tab(int delta);
  std::string switch_workspace(int number);
  int current_workspace() const { return current_workspace_; }
  void relayout();
  void synchronize_outputs(const std::vector<OutputGeometry> &outputs);
  std::string list_outputs() const;
  MonitorConfig monitor_config(const std::string &name) const;
  std::string configure_output(const std::string &name, const std::string &setting,
                               const std::string &value, const std::string &extra = {});
  std::string focus_output(const std::string &name);
  std::string move_to_output(const std::string &name);
  const std::string &current_output() const { return active_output_; }


  wl_display *display() const { return display_.get(); }
  CommandBar &command_bar() { return command_bar_; }
  const CommandBar &command_bar() const { return command_bar_; }
  const std::string &status_line() const { return status_line_; }

private:
  void layout_current_output();
  bool install_signals();
  static int shutdown_signal(int signal, void *userdata);
  static int child_signal(int signal, void *userdata);
  wl_event_source *signal_sources_[3]{};
  std::vector<int> launched_pids_;
  Cell *pending_kill_ = nullptr;
  Cell *active_cell() const;
  NCursorView *ncursor_view() const;
  GCursorView *active_gcursor() const;
  GCursorView *find_gcursor(const std::string &id) const;
  GCursorView *find_gcursor(wlr_xdg_toplevel *toplevel) const;
  void attach_cell_surface(Cell *cell);
  void detach_cell_surface(Cell *cell);
  void render_cell(Cell *cell, bool selected);
  void paint_notelet(Cell *cell);
  void request_notelet(Cell *cell, const std::string &key, const std::string &event);
  static int notelets_ready(void *userdata);
  wl_event_source *notelet_timer_ = nullptr;
  void paint_help_pager();
  bool handle_help_pager_key(uint32_t keysym, uint32_t unicode);
  bool feed_help_search_key(uint32_t keysym, uint32_t unicode);
  bool help_key_matches(const std::string &binding, uint32_t keysym,
                        uint32_t unicode) const;
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
  HelpPager help_pager_;
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
  std::map<std::string, Output> outputs_{{"default", Output{OutputGeometry{"default"}}}};
  std::string active_output_ = "default";
  std::string rendering_output_;
  bool laying_out_ = false;
  int output_x_ = 0, output_y_ = 0;
  int output_width_ = 1280;
  int output_height_ = 720;
  bool command_bar_open_ = false;
  bool launcher_mode_ = false;
  bool help_pager_active_ = false;
  bool help_search_open_ = false;
  std::string help_search_input_;
  GCursorView *quick_restore_[4] = {nullptr, nullptr, nullptr, nullptr};
  unsigned long next_ncursor_id_ = 1;
  int current_workspace_ = 1;
  int tcursor_workspace_ = 0;

};
