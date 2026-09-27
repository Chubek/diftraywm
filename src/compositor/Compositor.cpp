#include "compositor/Compositor.hpp"
#include "compositor/WaylandRuntime.h"

#include "nterm/NTerm.hpp"
#include "notelet/Notelet.hpp"
#include "plugin/PluginManager.hpp"
#include "theme/ThemeEngine.hpp"
#include "lua/LuaEngine.hpp"
#include "views/Cell.hpp"
#include "views/GCursorView.hpp"
#include "views/NCursorView.hpp"
#include "views/TCursorView.hpp"
#include "views/View.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
#include <libtsm.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

Compositor::Compositor() : keyboard_handler_(this) {}

namespace {
bool theme_color(const std::string &value, float out[4]) {
  if (value.size() != 7 && value.size() != 9) return false;
  if (value[0] != '#') return false;
  for (int i = 0; i < 4; ++i) {
    if (i == 3 && value.size() == 7) { out[i] = 1.0f; break; }
    unsigned int channel = 0;
    auto part = value.data() + 1 + i * 2;
    auto result = std::from_chars(part, part + 2, channel, 16);
    if (result.ec != std::errc() || result.ptr != part + 2) return false;
    out[i] = static_cast<float>(channel) / 255.0f;
  }
  return true;
}

bool theme_pixels(const std::string &value, int &out) {
  if (value.size() < 3 || value.substr(value.size() - 2) != "px") return false;
  auto result = std::from_chars(value.data(), value.data() + value.size() - 2, out);
  return result.ec == std::errc() && result.ptr == value.data() + value.size() - 2 && out > 0;
}
}

Compositor::~Compositor() {
  stop();
  for (auto &cell : cells_) {
    detach_cell_surface(cell.get());
  }
  diftray_wayland_runtime_destroy(wayland_runtime_);
  wayland_runtime_ = nullptr;
  delete lua_engine_;
  delete plugin_manager_;
  delete theme_engine_;
}

bool Compositor::init() {
  if (active_view_) {
    return true;
  }
  std::filesystem::path config_path = "diftray.conf";
  if (const char *configured = std::getenv("DIFTRAYWM_CONFIG")) {
    config_path = configured;
  } else if (const char *config_home = std::getenv("XDG_CONFIG_HOME")) {
    const auto user_config = std::filesystem::path(config_home) / "diftraywm/diftray.conf";
    if (std::filesystem::exists(user_config)) {
      config_path = user_config;
    }
  }
  if (!load_compositor_config(config_path.string(), config_, status_line_)) {
    return false;
  }
  if (!config_.theme.empty() && std::filesystem::path(config_.theme).is_relative()) {
    config_.theme = (config_path.parent_path() / config_.theme).lexically_normal().string();
  }
  if (!config_.word_pool.empty()) {
    setenv("DIFTRAYWM_WORD_POOL", config_.word_pool.c_str(), 0);
  }

  ncursor_views_.push_back(std::make_unique<NCursorView>());
  ncursor_views_.back()->set_id("primary");
  active_ncursor_ = ncursor_views_.back().get();
  active_ncursor_->set_workspace(current_workspace_);
  workspace_ncursor_[current_workspace_] = active_ncursor_;
  workspace_view_[current_workspace_] = active_ncursor_;
  cells_.push_back(std::make_unique<Cell>(config_.shell));
  active_ncursor_->insert_cell(cells_.back().get(), false);
  active_ncursor_->set_cell_select_mode(false);
  active_ncursor_->set_output_box({0, config_.status_bar_height, output_width_,
                                    output_height_ - config_.status_bar_height});

  theme_engine_ = new ThemeEngine();
  if (!config_.theme.empty()) {
    const auto result = load_theme_file(config_.theme);
    if (result != "theme applied") {
      status_line_ = result;
      return false;
    }
  }
  plugin_manager_ = new PluginManager();
  lua_engine_ = new LuaEngine();
  nterm_renderer_.init(config_.font, config_.font_size);
  command_context_.compositor = this;
  rebuild_command_context();
  active_view_ = active_ncursor_;

  if (lua_engine_) {
    lua_engine_->init();
    lua_engine_->scan_extensions();
  }
  if (plugin_manager_) {
    plugin_manager_->discover();
  }
  notelet_catalog_ = std::make_unique<NoteletCatalog>();
  const char *notelet_path = std::getenv("DIFTRAY_NOTELETS_PATH");
  std::string discovery_path;
  if (notelet_path) {
    discovery_path = notelet_path;
  } else {
    if (const char *home = std::getenv("HOME"); home && *home)
      discovery_path = std::string(home) + "/.local/share/diftraywm/notelets:";
    discovery_path += DIFTRAY_NOTELET_DEFAULT_PATH;
  }
  if (!notelet_catalog_->discover(discovery_path, status_line_)) return false;

  if (std::getenv("DIFTRAYWM_LOGIC_ONLY")) {
    status_line_ = "logic-only mode";
    return true;
  }

  display_.reset(wl_display_create());
  if (!display_) {
    status_line_ = "failed to create Wayland display";
    return false;
  }
  const char *socket_name = std::getenv("DIFTRAYWM_WAYLAND_SOCKET");
  const char *registered_socket =
      socket_name && *socket_name
          ? (wl_display_add_socket(display_.get(), socket_name) == 0 ? socket_name : nullptr)
          : wl_display_add_socket_auto(display_.get());
  if (!registered_socket) {
    status_line_ = "failed to add Wayland socket";
    display_.reset();
    return false;
  }
  wayland_socket_ = registered_socket;

  diftray_wayland_style style{
      config_.border_size,
      config_.command_bar_height,
      config_.status_bar_height,
      {config_.border_color[0], config_.border_color[1], config_.border_color[2],
       config_.border_color[3]},
      {config_.background_color[0], config_.background_color[1],
       config_.background_color[2], config_.background_color[3]},
      {config_.command_bar_color[0], config_.command_bar_color[1],
       config_.command_bar_color[2], config_.command_bar_color[3]},
      {1.0f, 0.72f, 0.18f, 1.0f}};
  wayland_runtime_ = diftray_wayland_runtime_create(display_.get(), &style);
  if (!wayland_runtime_) {
    status_line_ = "failed to initialize wlroots runtime";
    display_.reset();
    return false;
  }
  // Keep the parent compositor's WAYLAND_DISPLAY in place while wlroots
  // autocreates its backend.  In a nested session, wlroots uses this
  // variable to connect to the parent compositor; replacing it with our
  // newly-created socket here makes it connect back to itself.  Children
  // launched by DiftrayWM are given our socket below, after backend startup.
  diftray_wayland_runtime_set_key_handler(wayland_runtime_,
                                          &Compositor::terminal_key_received, this);
  diftray_wayland_runtime_set_toplevel_handler(
      wayland_runtime_, &Compositor::handle_new_toplevel,
      &Compositor::handle_toplevel_destroy, &Compositor::handle_toplevel_request,
      this);
  diftray_wayland_runtime_set_focus_handler(wayland_runtime_,
                                            &Compositor::handle_toplevel_focus);
  diftray_wayland_runtime_set_output_handler(
      wayland_runtime_, &Compositor::handle_output_geometry, this);
  for (auto &cell : cells_) {
    attach_cell_surface(cell.get());
  }
  setenv("WAYLAND_DISPLAY", wayland_socket_.c_str(), 1);
  status_line_ = "ncursor ready";
  update_chrome();
  return true;
}

int Compositor::run() {
  if (!wayland_runtime_ || !display_) {
    std::cerr << "DiftrayWM requires a Wayland compositor backend\n";
    return 1;
  }
  if (!diftray_wayland_runtime_start(wayland_runtime_)) {
    std::cerr << "failed to start wlroots backend\n";
    return 1;
  }
  std::cerr << "DiftrayWM compositor on WAYLAND_DISPLAY=" << wayland_socket_ << '\n';
  for (auto &cell : cells_) {
    if (cell && cell->nterm() && !cell->nterm()->running()) {
      cell->nterm()->start();
      watch_cell_pty(cell.get());
    }
  }
  relayout();
  running_ = true;
  wl_display_run(display_.get());
  running_ = false;
  stop();
  return 0;
}

void Compositor::stop() {
  running_ = false;
  for (auto &entry : pty_sources_) {
    if (entry.second) {
      wl_event_source_remove(entry.second);
    }
  }
  pty_sources_.clear();
  pty_cells_.clear();
  if (display_) {
    wl_display_terminate(display_.get());
  }
  for (auto &cell : cells_) {
    if (cell && cell->nterm()) {
      cell->nterm()->stop();
    }
  }
}

void Compositor::attach_cell_surface(Cell *cell) {
  if (!cell || !wayland_runtime_ || cell->surface()) {
    return;
  }
  cell->set_surface(diftray_cell_surface_create(wayland_runtime_));
}

void Compositor::detach_cell_surface(Cell *cell) {
  if (!cell || !cell->surface()) {
    return;
  }
  unwatch_cell_pty(cell);
  diftray_cell_surface_destroy(cell->surface());
  cell->set_surface(nullptr);
}

void Compositor::watch_cell_pty(Cell *cell) {
  if (!cell || !cell->nterm() || !display_) {
    return;
  }
  const int fd = cell->nterm()->master_fd();
  if (fd < 0 || pty_sources_.count(fd)) {
    return;
  }
  wl_event_source *source = wl_event_loop_add_fd(
      wl_display_get_event_loop(display_.get()), fd, WL_EVENT_READABLE,
      &Compositor::terminal_fd_ready, this);
  if (source) {
    pty_sources_[fd] = source;
    pty_cells_[fd] = cell;
  }
}

void Compositor::unwatch_cell_pty(Cell *cell) {
  if (!cell || !cell->nterm()) {
    return;
  }
  const int fd = cell->nterm()->master_fd();
  const auto it = pty_sources_.find(fd);
  if (it != pty_sources_.end()) {
    wl_event_source_remove(it->second);
    pty_sources_.erase(it);
  }
  pty_cells_.erase(fd);
}

void Compositor::render_cell(Cell *cell, bool selected) {
  if (!cell || !cell->surface() || !cell->nterm()) {
    return;
  }
  const wlr_box box = cell->box();
  const int border = config_.border_size;
  const int width = std::max(1, box.width - border * 2);
  const int height = std::max(1, box.height - border * 2);
  auto *glyphs = nterm_renderer_.glyphs();
  const int cell_w = glyphs ? glyphs->cell_width() : 8;
  const int cell_h = glyphs ? glyphs->cell_height() : 16;
  const std::size_t cols = static_cast<std::size_t>(std::max(1, width / std::max(1, cell_w)));
  const std::size_t rows = static_cast<std::size_t>(std::max(1, height / std::max(1, cell_h)));
  if (cell->nterm()->columns() != cols || cell->nterm()->rows() != rows) {
    cell->nterm()->resize(cols, rows);
    if (notelet_cells_.count(cell)) paint_notelet(cell);
  }
  std::vector<uint32_t> pixels;
  nterm_renderer_.render(cell->nterm(), pixels, width, height, selected);
  diftray_cell_surface_place(cell->surface(), box.x, box.y, box.width, box.height);
  diftray_cell_surface_update(cell->surface(), pixels.data(), width, height);
  diftray_cell_surface_set_highlight(cell->surface(), selected &&
                                        ncursor_view() &&
                                        ncursor_view()->cell_select_mode());
}

void Compositor::paint_notelet(Cell *cell) {
  auto it = notelet_cells_.find(cell);
  if (it == notelet_cells_.end() || !cell->nterm()) return;
  cell->nterm()->display("\x1b[2J\x1b[H");
  cell->nterm()->display(it->second->frame());
}

void Compositor::render_all_cells() {
  auto *ncursor = ncursor_view();
  Cell *focused = active_cell();
  const bool tcursor_active =
      tcursor_view_ && active_view_ == tcursor_view_.get() &&
      tcursor_workspace_ == current_workspace_;
  for (auto &cell : cells_) {
    if (!cell) {
      continue;
    }
    auto *owner = owner_ncursor(cell.get());
    const bool on_workspace = owner && owner->workspace() == current_workspace_;
    const bool on_active_tab =
        config_.ncursor_mode != "tab" || owner == ncursor;
    bool visible = on_workspace && on_active_tab;
    if (tcursor_active) {
      visible = on_workspace && cell->state() == CellState::TCURSOR;
    }
    if (cell->surface()) {
      diftray_cell_surface_set_visible(cell->surface(), visible && !active_gcursor());
    }
    if (visible) {
      render_cell(cell.get(), cell.get() == focused);
    }
  }
  (void)ncursor;
}

void Compositor::apply_view_visibility() {
  if (!wayland_runtime_) {
    return;
  }
  auto *gcursor = active_gcursor();
  const bool show_gcursor =
      gcursor != nullptr && !gcursor->docked() &&
      gcursor->workspace() == current_workspace_;
  diftray_wayland_runtime_set_gcursor_visible(wayland_runtime_, show_gcursor);
  diftray_wayland_runtime_set_ncursor_visible(wayland_runtime_, !show_gcursor);
  for (auto &cursor : gcursors_) {
    if (!cursor || !cursor->toplevel()) {
      continue;
    }
    const bool show = show_gcursor &&
        (config_.gcursor_mode == "stack"
             ? (!cursor->docked() && cursor->workspace() == current_workspace_)
             : cursor.get() == gcursor);
    diftray_wayland_runtime_set_gcursor_visible_surface(
        wayland_runtime_, cursor->toplevel(), show);
  }
  if (show_gcursor && config_.gcursor_mode == "stack") {
    auto windows = live_gcursors_on_workspace(current_workspace_);
    for (size_t i = 0; i < windows.size(); ++i) {
      const int x = static_cast<int>(i * output_width_ / windows.size());
      const int right = static_cast<int>((i + 1) * output_width_ / windows.size());
      diftray_wayland_runtime_layout_gcursor(wayland_runtime_, windows[i]->toplevel(),
                                             x, config_.status_bar_height,
                                             right - x,
                                             std::max(1, output_height_ - config_.status_bar_height));
    }
  } else if (show_gcursor) {
    diftray_wayland_runtime_layout_gcursor(wayland_runtime_, gcursor->toplevel(),
                                           0, config_.status_bar_height, output_width_,
                                           std::max(1, output_height_ - config_.status_bar_height));
  }
  if (show_gcursor) {
    diftray_wayland_runtime_focus_gcursor(wayland_runtime_, gcursor->toplevel());
  } else {
    diftray_wayland_runtime_clear_keyboard_focus(wayland_runtime_);
  }
}

void Compositor::update_chrome() {
  if (!wayland_runtime_) {
    return;
  }
  std::string chrome = "DiftrayWM  ws:" + std::to_string(current_workspace_);
  if (auto *gcursor = active_gcursor()) {
    const auto tabs = live_gcursors_on_workspace(current_workspace_);
    if (tabs.size() > 1) {
      std::size_t index = 0;
      for (; index < tabs.size(); ++index) {
        if (tabs[index] == gcursor) {
          break;
        }
      }
      chrome += "  tab:" + std::to_string(index + 1) + "/" +
                std::to_string(tabs.size());
    }
    chrome += "  gcursor:" + gcursor->word_id();
  } else {
    const auto tabs = ncursors_on_workspace(current_workspace_);
    if (tabs.size() > 1) {
      std::size_t index = 0;
      for (; index < tabs.size(); ++index) {
        if (tabs[index] == ncursor_view()) {
          break;
        }
      }
      chrome += "  tab:" + std::to_string(index + 1) + "/" +
                std::to_string(tabs.size());
    }
    if (tcursor_view_ && active_view_ == tcursor_view_.get() &&
        tcursor_workspace_ == current_workspace_) {
      chrome += "  tcursor";
    } else {
      chrome += "  ncursor";
      if (auto *cell = active_cell()) {
        chrome += "  cell:" + cell->id().substr(0, 6);
        if (auto it = notelet_cells_.find(cell); it != notelet_cells_.end())
          chrome += "  notelet:" + it->second->id();
      }
    }
  }
  if (!status_line_.empty()) {
    chrome += "  |  " + status_line_;
  }
  diftray_wayland_runtime_set_status_line(wayland_runtime_, chrome.c_str());
  if (command_bar_open_) {
    std::string prompt = launcher_mode_ ? "launch> " : (command_bar_.scope == CommandScope::NCURSOR_GLOBAL ? "global: " : ":");
    prompt += command_bar_.input_buffer;
    diftray_wayland_runtime_set_command_bar(wayland_runtime_, true, prompt.c_str());
  } else {
    diftray_wayland_runtime_set_command_bar(wayland_runtime_, false, "");
  }
}

void Compositor::relayout() {
  auto *ncursor = ncursor_view();
  if (!ncursor) {
    return;
  }
  int usable_height = output_height_ - config_.status_bar_height;
  if (command_bar_open_) {
    usable_height -= config_.command_bar_height;
  }
  const auto workspace_views = ncursors_on_workspace(current_workspace_);
  if (tcursor_view_ && active_view_ == tcursor_view_.get() &&
      tcursor_workspace_ == current_workspace_ && active_cell()) {
    active_cell()->set_box({0, config_.status_bar_height, output_width_, std::max(1, usable_height)});
  } else if (config_.ncursor_mode == "tab" && workspace_views.size() > 1) {
    ncursor->set_output_box({0, config_.status_bar_height, output_width_, std::max(1, usable_height)});
    ncursor->layout();
  } else if (workspace_views.size() > 1 && config_.ncursor_mode == "stack") {
    const int band =
        std::max(1, usable_height / static_cast<int>(workspace_views.size()));
    int y = config_.status_bar_height;
    for (auto *view : workspace_views) {
      view->set_output_box({0, y, output_width_, band});
      view->layout();
      y += band;
    }
  } else {
    ncursor->set_output_box({0, config_.status_bar_height, output_width_, std::max(1, usable_height)});
    ncursor->layout();
  }
  apply_view_visibility();
  render_all_cells();
  update_chrome();
}

int Compositor::terminal_fd_ready(int fd, uint32_t mask, void *data) {
  auto *compositor = static_cast<Compositor *>(data);
  if (!compositor || !(mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
    return 0;
  }
  auto it = compositor->pty_cells_.find(fd);
  if (it == compositor->pty_cells_.end() || !it->second || !it->second->nterm()) {
    return 0;
  }
  it->second->nterm()->on_readable();
  compositor->render_cell(it->second, it->second == compositor->active_cell());
  return 0;
}

bool Compositor::terminal_key_received(void *userdata, uint32_t keysym,
                                       uint32_t modifiers, uint32_t state,
                                       uint32_t unicode, uint32_t, uint32_t) {
  auto *compositor = static_cast<Compositor *>(userdata);
  return compositor && compositor->handle_key(keysym, modifiers, state, unicode);
}

void Compositor::handle_new_toplevel(void *userdata, wlr_xdg_toplevel *toplevel,
                                     int client_pid) {
  auto *compositor = static_cast<Compositor *>(userdata);
  if (compositor) {
    compositor->on_new_toplevel(toplevel, client_pid);
  }
}

void Compositor::handle_toplevel_destroy(void *userdata,
                                         wlr_xdg_toplevel *toplevel) {
  auto *compositor = static_cast<Compositor *>(userdata);
  if (compositor) {
    compositor->on_toplevel_destroy(toplevel);
  }
}

void Compositor::handle_toplevel_focus(void *userdata,
                                       wlr_xdg_toplevel *toplevel) {
  auto *compositor = static_cast<Compositor *>(userdata);
  if (!compositor) return;
  auto *cursor = compositor->find_gcursor(toplevel);
  if (cursor && !cursor->docked() && cursor->workspace() == compositor->current_workspace_) {
    compositor->set_active_view(cursor);
    compositor->update_chrome();
  }
}

void Compositor::handle_toplevel_request(void *userdata,
                                         wlr_xdg_toplevel *toplevel,
                                         const char *request) {
  auto *compositor = static_cast<Compositor *>(userdata);
  if (!compositor || !request) {
    return;
  }
  auto *cursor = compositor->find_gcursor(toplevel);
  if (!cursor) {
    return;
  }
  if (std::strcmp(request, "minimize") == 0) {
    compositor->dock_cursor(cursor->word_id());
  }
}

void Compositor::handle_output_geometry(void *userdata, int width, int height) {
  auto *compositor = static_cast<Compositor *>(userdata);
  if (compositor) {
    compositor->on_output_geometry(width, height);
  }
}

void Compositor::on_output_geometry(int width, int height) {
  output_width_ = std::max(1, width);
  output_height_ = std::max(1, height);
  relayout();
}

void Compositor::on_new_toplevel(wlr_xdg_toplevel *toplevel, int client_pid) {
  auto cursor = std::make_unique<GCursorView>(this);
  cursor->set_toplevel(toplevel);
  Cell *owner = nullptr;
  for (auto &cell : cells_) {
    if (cell && cell->nterm() && cell->nterm()->owns_pid(client_pid)) {
      owner = cell.get();
      break;
    }
  }
  cursor->set_owner_cell(owner);
  cursor->set_owner_ncursor(active_ncursor_);
  cursor->set_workspace(current_workspace_);
  GCursorView *raw = cursor.get();
  gcursors_.push_back(std::move(cursor));
  if (wayland_runtime_) {
    diftray_wayland_runtime_attach_gcursor(wayland_runtime_, toplevel);
  }
  set_active_view(raw);
  status_line_ = "gcursor " + raw->word_id();
  relayout();
}

void Compositor::on_toplevel_destroy(wlr_xdg_toplevel *toplevel) {
  auto *cursor = find_gcursor(toplevel);
  if (!cursor) {
    return;
  }
  if (active_view_ == cursor) {
    active_view_ = ncursor_view();
  }
  for (auto &slot : quick_restore_) {
    if (slot == cursor) {
      slot = nullptr;
    }
  }
  gcursors_.erase(std::remove_if(gcursors_.begin(), gcursors_.end(),
                                 [cursor](const auto &owned) {
                                   return owned.get() == cursor;
                                 }),
                  gcursors_.end());
  relayout();
}

unsigned int Compositor::tsm_mods(uint32_t modifiers) const {
  unsigned int mods = 0;
  if (modifiers & WLR_MODIFIER_SHIFT) {
    mods |= TSM_SHIFT_MASK;
  }
  if (modifiers & WLR_MODIFIER_CAPS) {
    mods |= TSM_LOCK_MASK;
  }
  if (modifiers & WLR_MODIFIER_CTRL) {
    mods |= TSM_CONTROL_MASK;
  }
  if (modifiers & WLR_MODIFIER_ALT) {
    mods |= TSM_ALT_MASK;
  }
  if (modifiers & WLR_MODIFIER_LOGO) {
    mods |= TSM_LOGO_MASK;
  }
  return mods;
}

void Compositor::open_command_bar(CommandScope scope, const std::string &prefix) {
  command_bar_open_ = true;
  launcher_mode_ = prefix == "launch";
  command_bar_.visible = true;
  command_bar_.scope = scope;
  command_bar_.input_buffer.clear();
  relayout();
}

void Compositor::close_command_bar() {
  command_bar_open_ = false;
  launcher_mode_ = false;
  command_bar_.visible = false;
  command_bar_.input_buffer.clear();
  relayout();
}

bool Compositor::feed_command_bar_key(uint32_t keysym, uint32_t unicode) {
  if (keysym == XKB_KEY_Escape) {
    close_command_bar();
    return true;
  }
  if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
    std::string command = command_bar_.input_buffer;
    if (launcher_mode_ && command.find("launch ") != 0) {
      command = "launch " + command;
    }
    command_bar_.dispatch(command);
    status_line_ = command_bar_.status_line();
    close_command_bar();
    return true;
  }
  if (keysym == XKB_KEY_BackSpace) {
    command_bar_.handle_key('\b');
    update_chrome();
    return true;
  }
  if (unicode >= 32 && unicode < 127) {
    command_bar_.handle_key(static_cast<unsigned int>(unicode));
    update_chrome();
    return true;
  }
  return true;
}

bool Compositor::handle_key(uint32_t keysym, uint32_t modifiers, uint32_t state,
                            uint32_t unicode) {
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    return command_bar_open_ || active_gcursor() == nullptr;
  }
  const bool meta = (modifiers & WLR_MODIFIER_LOGO) != 0;
  if (meta && keysym == XKB_KEY_Escape) {
    stop();
    return true;
  }
  if (command_bar_open_ && !(meta && keysym == XKB_KEY_colon)) {
    return feed_command_bar_key(keysym, unicode);
  }
  if (meta && (keysym == XKB_KEY_colon || keysym == XKB_KEY_semicolon)) {
    open_command_bar(CommandScope::NCURSOR_GLOBAL, "");
    return true;
  }
  if (!meta && keysym == XKB_KEY_colon && !active_gcursor()) {
    open_command_bar(CommandScope::CELL, "");
    return true;
  }
  if (meta && keysym == XKB_KEY_d) {
    open_command_bar(CommandScope::NCURSOR_GLOBAL, "launch");
    return true;
  }
  if (meta && keysym == XKB_KEY_Tab) {
    toggle_cell_select_mode();
    relayout();
    return true;
  }
  if (meta && keysym == XKB_KEY_Up) {
    move_selected_cell(-1);
    relayout();
    return true;
  }
  if (meta && keysym == XKB_KEY_Down) {
    move_selected_cell(1);
    relayout();
    return true;
  }
  if (meta && (keysym == XKB_KEY_k || keysym == XKB_KEY_K)) {
    status_line_ = kill_selected_cell();
    relayout();
    return true;
  }
  if (meta && keysym == XKB_KEY_n) {
    status_line_ = spawn_ncursor();
    relayout();
    return true;
  }
  if (meta && keysym == XKB_KEY_Return) {
    if (tcursor_view_ && tcursor_workspace_ == current_workspace_) {
      restore_tcursor();
    } else {
      if (tcursor_view_) {
        tcursor_view_.reset();
        tcursor_workspace_ = 0;
      }
      promote_active_cell_to_tcursor();
    }
    relayout();
    return true;
  }
  if (meta && (keysym == XKB_KEY_Left || keysym == XKB_KEY_KP_Left)) {
    status_line_ = cycle_tab(-1);
    return true;
  }
  if (meta && (keysym == XKB_KEY_Right || keysym == XKB_KEY_KP_Right)) {
    status_line_ = cycle_tab(1);
    return true;
  }
  if (meta) {
    int workspace = 0;
    if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
      workspace = static_cast<int>(keysym - XKB_KEY_1 + 1);
    } else if (keysym == XKB_KEY_0) {
      workspace = 10;
    } else if (keysym >= XKB_KEY_KP_1 && keysym <= XKB_KEY_KP_9) {
      workspace = static_cast<int>(keysym - XKB_KEY_KP_1 + 1);
    } else if (keysym == XKB_KEY_KP_0) {
      workspace = 10;
    }
    if (workspace != 0) {
      status_line_ = switch_workspace(workspace);
      return true;
    }
  }
  if (meta && keysym >= XKB_KEY_F1 && keysym <= XKB_KEY_F4) {
    restore_quick_slot(static_cast<int>(keysym - XKB_KEY_F1));
    relayout();
    return true;
  }
  if (ncursor_view() && ncursor_view()->cell_select_mode()) {
    if (keysym == XKB_KEY_Return) {
      focus_active_cell();
      ncursor_view()->set_cell_select_mode(false);
      relayout();
      return true;
    }
    if (keysym == XKB_KEY_Up) {
      ncursor_view()->select_previous_cell();
      relayout();
      return true;
    }
    if (keysym == XKB_KEY_Down) {
      ncursor_view()->select_next_cell();
      relayout();
      return true;
    }
  }
  if (active_gcursor()) {
    return false;
  }
  if (auto *cell = active_cell(); cell && cell->nterm()) {
    if (auto it = notelet_cells_.find(cell); it != notelet_cells_.end()) {
      std::string key;
      if (keysym == XKB_KEY_Return) key = "Enter";
      else if (keysym == XKB_KEY_BackSpace) key = "Backspace";
      else if (keysym == XKB_KEY_Escape) key = "Escape";
      else if (keysym == XKB_KEY_Up) key = "Up";
      else if (keysym == XKB_KEY_Down) key = "Down";
      else if (keysym == XKB_KEY_Left) key = "Left";
      else if (keysym == XKB_KEY_Right) key = "Right";
      else if (unicode > 0 && unicode < 128) key.assign(1, static_cast<char>(unicode));
      if (!key.empty()) {
        std::string error;
        if (!it->second->render(key, error)) status_line_ = error;
        else paint_notelet(cell);
        render_cell(cell, true);
        update_chrome();
      }
      return true;
    }
    cell->nterm()->handle_key(keysym, keysym < 128 ? keysym : 0, tsm_mods(modifiers),
                              unicode);
    render_cell(cell, true);
    return true;
  }
  return true;
}

void Compositor::set_active_view(View *view) {
  active_view_ = view;
  if (view && view->type() == ViewType::NCURSOR) {
    active_ncursor_ = static_cast<NCursorView *>(view);
  }
  rebuild_command_context();
}

NCursorView *Compositor::ncursor_view() const { return active_ncursor_; }

Cell *Compositor::active_cell() const {
  return active_ncursor_ ? active_ncursor_->active_cell() : nullptr;
}

GCursorView *Compositor::active_gcursor() const {
  if (auto *cursor = dynamic_cast<GCursorView *>(active_view_)) {
    return cursor->docked() ? nullptr : cursor;
  }
  return nullptr;
}

GCursorView *Compositor::find_gcursor(const std::string &id) const {
  for (const auto &cursor : gcursors_) {
    if (cursor && cursor->word_id() == id) {
      return cursor.get();
    }
  }
  return nullptr;
}

GCursorView *Compositor::find_gcursor(wlr_xdg_toplevel *toplevel) const {
  for (const auto &cursor : gcursors_) {
    if (cursor && cursor->toplevel() == toplevel) {
      return cursor.get();
    }
  }
  return nullptr;
}

void Compositor::rebuild_command_context() {
  command_context_.compositor = this;
  command_context_.active_view = active_view_;
  command_context_.ncursor_view = ncursor_view();
  command_context_.active_cell = active_cell();
  command_context_.active_gcursor = active_gcursor();
  command_context_.theme_engine = theme_engine_;
  command_context_.plugin_manager = plugin_manager_;
  command_context_.lua_engine = lua_engine_;
  command_context_.status_line = &status_line_;
  command_bar_.set_context(&command_context_);
}

std::string Compositor::spawn_cell(bool above) {
  if (!ncursor_view()) {
    return "spawn failed: no ncursor view";
  }
  cells_.push_back(std::make_unique<Cell>(config_.shell));
  auto *cell = cells_.back().get();
  if (!ncursor_view()->insert_cell(cell, above)) {
    cells_.pop_back();
    return "spawn failed: could not insert cell";
  }
  attach_cell_surface(cell);
  if (cell->nterm()) {
    cell->nterm()->start();
    watch_cell_pty(cell);
  }
  relayout();
  rebuild_command_context();
  return std::string("spawned cell ") + cell->id();
}

std::string Compositor::erase_cell(Cell *cell) {
  if (!cell) {
    return "kill failed: no active cell";
  }
  std::size_t workspace_cells = 0;
  for (auto *view : ncursors_on_workspace(current_workspace_)) {
    for (const auto &stack : view->cell_stacks()) {
      workspace_cells += stack.cells.size();
    }
  }
  if (workspace_cells <= 1) {
    return "kill failed: cannot remove the last cell";
  }
  detach_cell_surface(cell);
  notelet_cells_.erase(cell);
  if (auto *owner = owner_ncursor(cell)) {
    owner->remove_cell(cell);
  } else if (ncursor_view()) {
    ncursor_view()->remove_cell(cell);
  }
  if (tcursor_view_) {
    tcursor_view_.reset();
    tcursor_workspace_ = 0;
    active_view_ = ncursor_view();
  }
  cells_.erase(std::remove_if(cells_.begin(), cells_.end(),
                              [cell](const std::unique_ptr<Cell> &owned) {
                                return owned.get() == cell;
                              }),
               cells_.end());
  rebuild_command_context();
  relayout();
  return "removed cell";
}

std::string Compositor::kill_selected_cell() { return erase_cell(active_cell()); }

std::string Compositor::move_selected_cell(int delta) {
  if (!ncursor_view()) {
    return "move failed: no ncursor view";
  }
  if (ncursor_view()->move_selected_cell(delta)) {
    relayout();
    rebuild_command_context();
    return "moved selected cell";
  }
  return "move failed";
}

std::string Compositor::toggle_cell_select_mode() {
  if (!ncursor_view()) {
    return "cell select unavailable";
  }
  ncursor_view()->set_cell_select_mode(!ncursor_view()->cell_select_mode());
  relayout();
  return ncursor_view()->cell_select_mode() ? "cell select enabled"
                                            : "cell select disabled";
}

std::string Compositor::focus_active_cell() {
  if (auto *cell = active_cell()) {
    set_active_view(ncursor_view());
    relayout();
    return "focused cell " + cell->id();
  }
  return "focus failed: no active cell";
}

std::string Compositor::promote_active_cell_to_tcursor() {
  if (tcursor_view_) {
    return "tcursor already active";
  }
  auto *cell = active_cell();
  if (!cell) {
    return "promote failed: no active cell";
  }
  tcursor_view_ = std::make_unique<TCursorView>(cell);
  active_view_ = tcursor_view_.get();
  tcursor_workspace_ = current_workspace_;
  rebuild_command_context();
  relayout();
  return "promoted cell " + cell->id();
}

std::string Compositor::restore_tcursor() {
  if (!tcursor_view_) {
    return "restore failed: no tcursor";
  }
  tcursor_view_.reset();
  tcursor_workspace_ = 0;
  active_view_ = ncursor_view();
  rebuild_command_context();
  relayout();
  return "restored tcursor";
}

std::string Compositor::dock_cursor(const std::string &id) {
  auto *cursor = find_gcursor(id);
  if (!cursor) {
    return "dock failed: cursor not found";
  }
  if (auto *cell = cursor->owner_cell() ? cursor->owner_cell() : active_cell()) {
    cell->cursor_area().dock(cursor);
  } else if (ncursor_view()) {
    ncursor_view()->cursor_area().dock(cursor);
  } else {
    cursor->dock();
  }
  if (wayland_runtime_) {
    diftray_wayland_runtime_set_gcursor_visible_surface(wayland_runtime_,
                                                        cursor->toplevel(), false);
  }
  if (active_view_ == cursor) {
    active_view_ = ncursor_view();
  }
  rebuild_command_context();
  relayout();
  return "docked cursor " + cursor->word_id();
}

std::string Compositor::restore_cursor(const std::string &id) {
  auto *cursor = find_gcursor(id);
  if (!cursor) {
    return "restore failed: cursor not found";
  }
  if (auto *cell = cursor->owner_cell()) {
    cell->cursor_area().restore(cursor);
  } else if (ncursor_view()) {
    ncursor_view()->cursor_area().restore(cursor);
  } else {
    cursor->restore();
  }
  if (wayland_runtime_) {
    diftray_wayland_runtime_set_gcursor_visible_surface(wayland_runtime_,
                                                        cursor->toplevel(), true);
    diftray_wayland_runtime_focus_gcursor(wayland_runtime_, cursor->toplevel());
  }
  set_active_view(cursor);
  relayout();
  return "restored cursor " + cursor->word_id();
}

std::string Compositor::list_cursor_ids() const {
  if (gcursors_.empty()) {
    return "no cursors";
  }
  std::ostringstream out;
  for (const auto &cursor : gcursors_) {
    if (!cursor) {
      continue;
    }
    if (out.tellp() > 0) {
      out << ' ';
    }
    out << cursor->word_id();
    if (cursor->docked()) {
      out << "*";
    }
  }
  return out.str();
}

std::string Compositor::list_docked_cursors() const {
  std::ostringstream out;
  for (const auto &cursor : gcursors_) {
    if (cursor && cursor->docked()) {
      if (out.tellp() > 0) {
        out << ' ';
      }
      out << cursor->word_id();
    }
  }
  const auto result = out.str();
  return result.empty() ? "no docked cursors" : result;
}

std::string Compositor::assign_cursor_slot(const std::string &id, int slot) {
  auto *cursor = find_gcursor(id);
  if (!cursor) {
    return "assign failed: cursor not found";
  }
  if (slot < 0 || slot > 3) {
    return "assign failed: slot must be F1-F4";
  }
  for (auto &assigned : quick_restore_) {
    if (assigned == cursor) {
      assigned = nullptr;
    }
  }
  quick_restore_[slot] = cursor;
  cursor->set_quick_restore_slot(slot);
  if (ncursor_view()) {
    ncursor_view()->cursor_area().assign_slot(cursor, slot);
  }
  return "assigned " + cursor->word_id() + " to F" + std::to_string(slot + 1);
}

std::string Compositor::restore_quick_slot(int slot) {
  if (slot < 0 || slot > 3 || !quick_restore_[slot]) {
    return "no cursor assigned";
  }
  return restore_cursor(quick_restore_[slot]->word_id());
}

std::string Compositor::set_shell_override(const std::string &shell, CommandScope scope) {
  if (scope == CommandScope::CELL) {
    if (auto *cell = active_cell()) {
      cell->set_shell_override(shell);
      return "set cell shell to " + shell;
    }
    return "set shell failed: no active cell";
  }
  for (auto &cell : cells_) {
    if (cell) {
      cell->set_shell_override(shell);
    }
  }
  return "set global shell to " + shell;
}

std::string Compositor::apply_theme_css(const std::string &css) {
  if (!theme_engine_) {
    return "theme engine unavailable";
  }
  ThemeProperties parsed;
  if (!theme_engine_->parse_css(css, parsed)) {
    return theme_engine_->last_error();
  }
  CompositorConfig next = config_;
  for (const auto &[key, value] : parsed.tokens) {
    if (key == "border-color" && !theme_color(value, next.border_color)) return "invalid border-color";
    if (key == "background-color" && !theme_color(value, next.background_color)) return "invalid background-color";
    if (key == "command-bar-color" && !theme_color(value, next.command_bar_color)) return "invalid command-bar-color";
    if (key == "border-size" && !theme_pixels(value, next.border_size)) return "invalid border-size";
    if (key == "command-bar-height" && !theme_pixels(value, next.command_bar_height)) return "invalid command-bar-height";
    if (key == "status-bar-height" && !theme_pixels(value, next.status_bar_height)) return "invalid status-bar-height";
  }
  config_ = std::move(next);
  theme_engine_->swap_active(std::move(parsed));
  if (wayland_runtime_) {
    diftray_wayland_style style{
        config_.border_size, config_.command_bar_height, config_.status_bar_height,
        {config_.border_color[0], config_.border_color[1], config_.border_color[2], config_.border_color[3]},
        {config_.background_color[0], config_.background_color[1], config_.background_color[2], config_.background_color[3]},
        {config_.command_bar_color[0], config_.command_bar_color[1], config_.command_bar_color[2], config_.command_bar_color[3]},
        {1.0f, 0.72f, 0.18f, 1.0f}};
    diftray_wayland_runtime_set_style(wayland_runtime_, &style);
    relayout();
  }
  return "theme applied";
}

std::string Compositor::load_theme_file(const std::string &path) {
  std::ifstream input(path);
  if (!input) return "cannot open theme: " + path;
  std::ostringstream contents;
  contents << input.rdbuf();
  return apply_theme_css(contents.str());
}

std::string Compositor::launch_program(const std::string &command) {
  if (command.empty()) {
    return "launch requires a command";
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    return "launch failed: fork";
  }
  if (pid == 0) {
    if (display_) {
      setenv("WAYLAND_DISPLAY", wayland_socket_.c_str(), 1);
    }
    ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  return "launched " + command;
}

std::string Compositor::list_notelets() const {
  if (!notelet_catalog_ || notelet_catalog_->names().empty()) return "no notelets found";
  std::string result = "notelets:";
  for (const auto &name : notelet_catalog_->names()) result += " " + name;
  return result;
}

std::string Compositor::open_notelet(const std::string &id) {
  if (!notelet_catalog_ || !ncursor_view()) return "notelets unavailable";
  std::string error;
  auto notelet = notelet_catalog_->open(id, error);
  if (!notelet) return error;
  if (!notelet->render("", error)) return "notelet failed: " + error;
  auto cell = std::make_unique<Cell>(config_.shell);
  Cell *raw = cell.get();
  if (!ncursor_view()->insert_cell(raw, false)) return "notelet cell creation failed";
  cells_.push_back(std::move(cell));
  notelet_cells_.emplace(raw, std::move(notelet));
  attach_cell_surface(raw);
  paint_notelet(raw);
  rebuild_command_context();
  relayout();
  return "opened notelet " + id;
}

std::string Compositor::close_notelet() {
  Cell *cell = active_cell();
  if (!cell || !notelet_cells_.count(cell)) return "no active notelet";
  return erase_cell(cell);
}

std::string Compositor::spawn_ncursor() {
  auto view = std::make_unique<NCursorView>();
  view->set_id("ncursor-" + std::to_string(ncursor_views_.size() + 1));
  view->set_workspace(current_workspace_);
  cells_.push_back(std::make_unique<Cell>(config_.shell));
  view->insert_cell(cells_.back().get(), false);
  attach_cell_surface(cells_.back().get());
  if (cells_.back()->nterm()) {
    cells_.back()->nterm()->start();
    watch_cell_pty(cells_.back().get());
  }
  active_ncursor_ = view.get();
  active_view_ = view.get();
  ncursor_views_.push_back(std::move(view));
  workspace_ncursor_[current_workspace_] = active_ncursor_;
  workspace_view_[current_workspace_] = active_ncursor_;
  rebuild_command_context();
  relayout();
  return "opened ncursor " + active_ncursor_->id();
}

NCursorView *Compositor::owner_ncursor(const Cell *cell) const {
  if (!cell) {
    return nullptr;
  }
  for (const auto &view : ncursor_views_) {
    if (!view) {
      continue;
    }
    for (const auto &stack : view->cell_stacks()) {
      if (std::find(stack.cells.begin(), stack.cells.end(), cell) !=
          stack.cells.end()) {
        return view.get();
      }
    }
  }
  return nullptr;
}

std::vector<NCursorView *> Compositor::ncursors_on_workspace(int workspace) const {
  std::vector<NCursorView *> views;
  for (const auto &view : ncursor_views_) {
    if (view && view->workspace() == workspace) {
      views.push_back(view.get());
    }
  }
  return views;
}

std::vector<GCursorView *> Compositor::live_gcursors_on_workspace(int workspace) const {
  std::vector<GCursorView *> views;
  for (const auto &cursor : gcursors_) {
    if (cursor && !cursor->docked() && cursor->workspace() == workspace) {
      views.push_back(cursor.get());
    }
  }
  return views;
}

bool Compositor::view_exists(const View *view) const {
  if (!view) {
    return false;
  }
  for (const auto &ncursor : ncursor_views_) {
    if (ncursor.get() == view) {
      return true;
    }
  }
  for (const auto &cursor : gcursors_) {
    if (cursor.get() == view) {
      return true;
    }
  }
  return tcursor_view_.get() == view;
}

void Compositor::remember_workspace_focus() {
  if (current_workspace_ < kMinWorkspace || current_workspace_ > kMaxWorkspace) {
    return;
  }
  workspace_ncursor_[current_workspace_] = active_ncursor_;
  workspace_view_[current_workspace_] = active_view_;
}

void Compositor::restore_workspace_focus() {
  NCursorView *ncursor = nullptr;
  if (current_workspace_ >= kMinWorkspace && current_workspace_ <= kMaxWorkspace) {
    ncursor = workspace_ncursor_[current_workspace_];
  }
  bool ncursor_ok = false;
  for (const auto &view : ncursor_views_) {
    if (ncursor && view.get() == ncursor &&
        ncursor->workspace() == current_workspace_) {
      ncursor_ok = true;
      break;
    }
  }
  if (!ncursor_ok) {
    ncursor = nullptr;
    for (const auto &view : ncursor_views_) {
      if (view && view->workspace() == current_workspace_) {
        ncursor = view.get();
        break;
      }
    }
  }
  active_ncursor_ = ncursor;
  View *view = nullptr;
  if (current_workspace_ >= kMinWorkspace && current_workspace_ <= kMaxWorkspace) {
    view = workspace_view_[current_workspace_];
  }
  if (!view_exists(view)) {
    view = ncursor;
  } else if (view->type() == ViewType::NCURSOR) {
    if (static_cast<NCursorView *>(view)->workspace() != current_workspace_) {
      view = ncursor;
    }
  } else if (view->type() == ViewType::GCURSOR) {
    auto *cursor = static_cast<GCursorView *>(view);
    if (cursor->docked() || cursor->workspace() != current_workspace_) {
      view = ncursor;
    }
  } else if (view->type() == ViewType::TCURSOR &&
             tcursor_workspace_ != current_workspace_) {
    view = ncursor;
  }
  active_view_ = view ? view : ncursor;
}

std::string Compositor::cycle_tab(int delta) {
  if (delta == 0) {
    return "tab unchanged";
  }
  const int step = delta > 0 ? 1 : -1;
  if (auto *gcursor = active_gcursor()) {
    const auto tabs = live_gcursors_on_workspace(current_workspace_);
    if (tabs.size() <= 1) {
      return "only one tab";
    }
    auto it = std::find(tabs.begin(), tabs.end(), gcursor);
    std::size_t index = it == tabs.end()
                            ? 0
                            : static_cast<std::size_t>(std::distance(tabs.begin(), it));
    index = (index + tabs.size() + static_cast<std::size_t>(step == 1 ? 1 : tabs.size() - 1)) %
            tabs.size();
    set_active_view(tabs[index]);
    workspace_view_[current_workspace_] = tabs[index];
    relayout();
    return "tab " + tabs[index]->word_id();
  }
  const auto tabs = ncursors_on_workspace(current_workspace_);
  if (tabs.size() <= 1) {
    return "only one tab";
  }
  auto it = std::find(tabs.begin(), tabs.end(), active_ncursor_);
  std::size_t index = it == tabs.end()
                          ? 0
                          : static_cast<std::size_t>(std::distance(tabs.begin(), it));
  index = (index + tabs.size() + static_cast<std::size_t>(step == 1 ? 1 : tabs.size() - 1)) %
          tabs.size();
  set_active_view(tabs[index]);
  workspace_ncursor_[current_workspace_] = tabs[index];
  workspace_view_[current_workspace_] = tabs[index];
  relayout();
  return "tab " + tabs[index]->id();
}

std::string Compositor::switch_workspace(int number) {
  if (number < kMinWorkspace || number > kMaxWorkspace) {
    return "workspace must be 1-10";
  }
  if (number == current_workspace_) {
    return "workspace " + std::to_string(number);
  }
  remember_workspace_focus();
  current_workspace_ = number;
  bool exists = false;
  for (const auto &view : ncursor_views_) {
    if (view && view->workspace() == number) {
      exists = true;
      break;
    }
  }
  if (!exists) {
    spawn_ncursor();
    return "workspace " + std::to_string(number);
  }
  restore_workspace_focus();
  rebuild_command_context();
  relayout();
  return "workspace " + std::to_string(number);
}

void WlDisplayDeleter::operator()(wl_display *ptr) const noexcept {
  if (ptr) {
    wl_display_destroy(ptr);
  }
}
