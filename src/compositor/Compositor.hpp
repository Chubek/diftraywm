#pragma once

#include "command/CommandBar.hpp"
#include "command/CommandContext.hpp"
#include "config/Config.hpp"

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

struct wl_display;
struct wl_event_source;
struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_output_layout;
struct wlr_scene;
struct wlr_seat;
struct wlr_output;
struct diftray_wayland_runtime;

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

struct WlDisplayDeleter {
  void operator()(wl_display *ptr) const noexcept;
};
struct WlrBackendDeleter {
  void operator()(wlr_backend *ptr) const noexcept;
};
struct WlrRendererDeleter {
  void operator()(wlr_renderer *ptr) const noexcept;
};
struct WlrAllocatorDeleter {
  void operator()(wlr_allocator *ptr) const noexcept;
};
struct WlrOutputLayoutDeleter {
  void operator()(wlr_output_layout *ptr) const noexcept;
};
struct WlrSceneDeleter {
  void operator()(wlr_scene *ptr) const noexcept;
};
struct WlrSeatDeleter {
  void operator()(wlr_seat *ptr) const noexcept;
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
  std::string set_shell_override(const std::string &shell, CommandScope scope);
  std::string apply_theme_css(const std::string &css);

  wl_display *display() const { return display_.get(); }
  wlr_output_layout *output_layout() const { return output_layout_.get(); }
  CommandBar &command_bar() { return command_bar_; }
  const CommandBar &command_bar() const { return command_bar_; }

private:
  Cell *active_cell() const;
  NCursorView *ncursor_view() const;
  GCursorView *active_gcursor() const;
  void refresh_terminal_display();
  static int terminal_fd_ready(int fd, uint32_t mask, void *data);
  static void terminal_key_received(void *userdata, uint32_t keysym,
                                    uint32_t modifiers, uint32_t state);
  void handle_terminal_key(uint32_t keysym, uint32_t modifiers,
                           uint32_t state);
  void rebuild_command_context();
  std::string erase_cell(Cell *cell);

  std::unique_ptr<wl_display, WlDisplayDeleter> display_;
  std::unique_ptr<wlr_backend, WlrBackendDeleter> backend_;
  std::unique_ptr<wlr_renderer, WlrRendererDeleter> renderer_;
  std::unique_ptr<wlr_allocator, WlrAllocatorDeleter> allocator_;
  std::unique_ptr<wlr_output_layout, WlrOutputLayoutDeleter> output_layout_;
  std::unique_ptr<wlr_scene, WlrSceneDeleter> scene_;
  std::unique_ptr<wlr_seat, WlrSeatDeleter> seat_;
  std::unique_ptr<NCursorView> ncursor_view_;
  std::vector<std::unique_ptr<Cell>> cells_;
  std::vector<std::unique_ptr<GCursorView>> gcursors_;
  std::unique_ptr<TCursorView> tcursor_view_;
  CommandBar command_bar_;
  CommandContext command_context_;
  std::string status_line_;
  ThemeEngine *theme_engine_ = nullptr;
  PluginManager *plugin_manager_ = nullptr;
  LuaEngine *lua_engine_ = nullptr;
  std::vector<Output *> outputs_;
  std::vector<View *> views_;
  std::vector<Input *> inputs_;
  View *active_view_ = nullptr;
  bool running_ = false;
  wl_event_source *terminal_source_ = nullptr;
  std::string wayland_socket_;
  diftray_wayland_runtime *wayland_runtime_ = nullptr;
  CompositorConfig config_;
};
