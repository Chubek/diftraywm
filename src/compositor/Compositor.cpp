#include "compositor/Compositor.hpp"
#include "compositor/ControlServer.hpp"
#include "compositor/WaylandRuntime.h"
#include "ctl/ControlSocketPath.hpp"

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

#include <termlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
#include <libtsm.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <csignal>
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

// Modifier bits that participate in binding matches; Caps Lock and Num Lock
// are masked out, mirroring the configuration program's binding matching.
constexpr uint32_t kBindingMods =
    WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
    WLR_MODIFIER_MOD3 | WLR_MODIFIER_LOGO | WLR_MODIFIER_MOD5;

uint32_t prefix_modifier_bit(const std::string &name) {
  if (name == "Meta" || name == "Super") return WLR_MODIFIER_LOGO;
  if (name == "Ctrl") return WLR_MODIFIER_CTRL;
  if (name == "Shift") return WLR_MODIFIER_SHIFT;
  if (name == "Alt") return WLR_MODIFIER_ALT;
  if (name == "Mod3") return WLR_MODIFIER_MOD3;
  if (name == "Mod5") return WLR_MODIFIER_MOD5;
  return 0;
}

// Bridges the keymap subsystem's own modifier bits to and from libinput's, so
// one chord in the INI means the same thing whether the compositor matches it
// against a key event or the evdev backend matches it against an input_event.
uint32_t wlr_mods(uint32_t keymap_mods) {
  uint32_t mods = 0;
  if (keymap_mods & kModShift) mods |= WLR_MODIFIER_SHIFT;
  if (keymap_mods & kModCtrl) mods |= WLR_MODIFIER_CTRL;
  if (keymap_mods & kModAlt) mods |= WLR_MODIFIER_ALT;
  if (keymap_mods & kModLogo) mods |= WLR_MODIFIER_LOGO;
  // The keymap spells Meta as AltGr, which libinput reports as Mod3.
  if (keymap_mods & kModMeta) mods |= WLR_MODIFIER_MOD3;
  return mods;
}

uint32_t keymap_mods(uint32_t wlr_modifier) {
  uint32_t mods = kModNone;
  if (wlr_modifier & WLR_MODIFIER_SHIFT) mods |= kModShift;
  if (wlr_modifier & WLR_MODIFIER_CTRL) mods |= kModCtrl;
  if (wlr_modifier & WLR_MODIFIER_ALT) mods |= kModAlt;
  if (wlr_modifier & WLR_MODIFIER_LOGO) mods |= kModLogo;
  if (wlr_modifier & WLR_MODIFIER_MOD3) mods |= kModMeta;
  return mods;
}

// Parses a Meta-prefix chord spec such as "Ctrl+Q": one or more modifiers
// followed by a single key name. Returns false when the spec is malformed.
bool parse_meta_chord(const std::string &spec, uint32_t &mods, uint32_t &keysym) {
  mods = 0;
  keysym = 0;
  std::size_t start = 0;
  std::vector<std::string> parts;
  for (std::size_t i = 0; i <= spec.size(); ++i) {
    if (i == spec.size() || spec[i] == '+') {
      parts.push_back(spec.substr(start, i - start));
      start = i + 1;
    }
  }
  if (parts.size() < 2) {
    return false;
  }
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    const uint32_t bit = prefix_modifier_bit(parts[i]);
    if (bit == 0 || (mods & bit) != 0) {
      return false;
    }
    mods |= bit;
  }
  const uint32_t sym =
      xkb_keysym_from_name(parts.back().c_str(), XKB_KEYSYM_NO_FLAGS);
  if (sym == XKB_KEY_NoSymbol) {
    return false;
  }
  keysym = sym;
  return true;
}

bool chord_keysym_matches(uint32_t pressed, uint32_t spec) {
  if (pressed == spec) {
    return true;
  }
  // A single-letter chord key matches either shift state (q vs Q).
  if (spec >= 'a' && spec <= 'z') {
    return pressed == spec - ('a' - 'A');
  }
  if (spec >= 'A' && spec <= 'Z') {
    return pressed == spec + ('a' - 'A');
  }
  return false;
}
}

Compositor::~Compositor() {
  stop();
  remove_control_socket();
  if (notelet_timer_) wl_event_source_remove(notelet_timer_);
  for (auto *source : signal_sources_) if (source) wl_event_source_remove(source);
  notelet_cells_.clear();
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
  std::filesystem::path config_path;
  if (const char *configured = std::getenv("DIFTRAYWM_CONFIG")) {
    config_path = configured;
    if (!load_compositor_config(config_path.string(), config_, status_line_)) return false;
  } else {
    std::vector<std::filesystem::path> directories;
    if (const char *home = std::getenv("XDG_CONFIG_HOME"); home && *home)
      directories.emplace_back(std::filesystem::path(home) / "diftraywm");
    else if (const char *home = std::getenv("HOME"); home && *home)
      directories.emplace_back(std::filesystem::path(home) / ".config/diftraywm");
    directories.emplace_back(".");
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) directories.emplace_back(executable.parent_path().parent_path() / "share/diftraywm");
    for (const auto &directory : directories) {
      for (const auto *name : {"diftray.yaml", "diftray.yml", "diftray.toml", "diftray.conf"}) {
        const auto candidate = directory / name;
        if (std::filesystem::exists(candidate, error)) { config_path = candidate; break; }
      }
      if (!config_path.empty()) break;
    }
    if (!config_path.empty() && !load_compositor_config(config_path.string(), config_, status_line_)) return false;
  }
  if (!config_path.empty()) {
    config_path_ = config_path.string();
  }
  if (!config_.theme.empty() && std::filesystem::path(config_.theme).is_relative()) {
    config_.theme = (config_path.parent_path() / config_.theme).lexically_normal().string();
  }
  if (!config_.help_path.empty() && std::filesystem::path(config_.help_path).is_relative()) {
    config_.help_path = (config_path.parent_path() / config_.help_path).lexically_normal().string();
  }
  help_pager_.set_search_path(config_.help_path.empty()
      ? (config_path.parent_path() / "help").string() + ":" + DIFTRAY_HELP_DEFAULT_PATH
      : config_.help_path);
  // A bad keymap is reported but not fatal: the built-in keys stay in force, so
  // a typo in the INI cannot lock the user out of their own session.
  if (!config_.keymap.empty() && !load_keymap(status_line_)) {
    keymap_error_ = status_line_;
  }
  if (!config_.word_pool.empty()) {
    setenv("DIFTRAYWM_WORD_POOL", config_.word_pool.c_str(), 0);
  }
  // The launcher starts locked exactly when the configuration asks for it.
  launcher_locked_ = config_.launcher_locked;

  ncursor_views_.push_back(std::make_unique<NCursorView>());
  ncursor_views_.back()->set_id("primary");
  active_ncursor_ = ncursor_views_.back().get();
  active_ncursor_->set_workspace(current_workspace_);
  outputs_[active_output_].ncursors[current_workspace_] = active_ncursor_;
  outputs_[active_output_].views[current_workspace_] = active_ncursor_;
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
  plugin_manager_ = new PluginManager(&command_bar_);
  lua_engine_ = new LuaEngine(&command_bar_);
  if (!nterm_renderer_.init(config_.font, config_.font_size)) {
    status_line_ = "failed to initialize text renderer"; return false;
  }
  command_context_.compositor = this;
  rebuild_command_context();
  active_view_ = active_ncursor_;

  if (lua_engine_) {
    lua_engine_->init();
    lua_engine_->scan_extensions();
    if (!lua_engine_->error().empty()) std::cerr << lua_engine_->error() << '\n';
  }
  if (plugin_manager_) {
    if (!plugin_manager_->discover()) std::cerr << "plugin discovery: " << plugin_manager_->error() << '\n';
  }
  notelet_catalog_ = std::make_unique<NoteletCatalog>();
  const char *notelet_path = std::getenv("DIFTRAY_NOTELETS_PATH");
  std::string discovery_path;
  if (notelet_path) {
    discovery_path = notelet_path;
  } else {
    if (const char *data = std::getenv("XDG_DATA_HOME"); data && *data)
      discovery_path = std::string(data) + "/diftraywm/notelets:";
    else if (const char *home = std::getenv("HOME"); home && *home)
      discovery_path = std::string(home) + "/.local/share/diftraywm/notelets:";
    discovery_path += (config_path.parent_path() / "notelets").string() + ":";
    discovery_path += DIFTRAY_NOTELET_DEFAULT_PATH;
  }
  if (!notelet_catalog_->discover(discovery_path, status_line_)) return false;

  if (std::getenv("DIFTRAYWM_LOGIC_ONLY")) {
    lua_engine_->drain_commands();
    status_line_ = "logic-only mode";
    return true;
  }

  display_.reset(wl_display_create());
  if (!display_) {
    status_line_ = "failed to create Wayland display";
    return false;
  }
  if (!install_signals()) { status_line_ = "failed to install signal handlers"; return false; }
  notelet_timer_ = wl_event_loop_add_timer(wl_display_get_event_loop(display_.get()),
                                          &Compositor::notelets_ready, this);
  if (!notelet_timer_) { status_line_ = "failed to create Notelet timer"; return false; }
  wl_event_source_timer_update(notelet_timer_, 10);
  const char *socket_name = std::getenv("DIFTRAYWM_WAYLAND_SOCKET");
  const char *registered_socket =
      socket_name && *socket_name
          ? (wl_display_add_socket(display_.get(), socket_name) == 0 ? socket_name : nullptr)
          : wl_display_add_socket_auto(display_.get());
  if (!registered_socket) {
    status_line_ = "failed to add Wayland socket";
    return false;
  }
  wayland_socket_ = registered_socket;

  const diftray_wayland_style style = wayland_style();
  wayland_runtime_ = diftray_wayland_runtime_create(display_.get(), &style);
  if (!wayland_runtime_) {
    status_line_ = "failed to initialize wlroots runtime";
    return false;
  }
  // Keep the parent compositor's WAYLAND_DISPLAY in place while wlroots
  // autocreates its backend.  In a nested session, wlroots uses this
  // variable to connect to the parent compositor; replacing it with our
  // newly-created socket here makes it connect back to itself.  Children
  // launched by DiftrayWM are given our socket below, after backend startup.
  diftray_wayland_runtime_set_output_config_handler(wayland_runtime_, [](void *data, const char *name, diftray_output_config *out) {
    const auto monitor = static_cast<Compositor *>(data)->monitor_config(name);
    *out = {monitor.rotation, monitor.scale, monitor.positioned, monitor.x, monitor.y};
  }, this);
  diftray_wayland_runtime_set_frame_handler(wayland_runtime_, [](void *data) {
    static_cast<Compositor *>(data)->notify_extensions("frame");
  }, this);
  diftray_wayland_runtime_set_text_renderer(wayland_runtime_,
      [](void *userdata, const char *text, uint32_t *pixels, int width, int height,
         const float bg[4], const float fg[4]) {
        auto *self = static_cast<Compositor *>(userdata);
        self->nterm_renderer_.glyphs()->draw_text(text, pixels, width, height, bg, fg);
      }, this);
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
  // The control socket is a convenience, not a prerequisite: a session without
  // it still runs, it just cannot be driven by diftrayctl.
  install_control_socket();
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
    if (cell && cell->nterm() && !notelet_cells_.count(cell.get()) && !cell->nterm()->running()) {
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
  // Connections are served on the same event loop that is about to stop, so
  // drop them before the display tears down the loop's sources.
  if (control_server_) control_server_->stop();
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
    if (notelet_cells_.count(cell)) request_notelet(cell, "", "resize");
  }
  std::vector<uint32_t> pixels;
  nterm_renderer_.render(cell->nterm(), pixels, width, height, selected);
  diftray_cell_surface_place(cell->surface(), box.x, box.y, box.width, box.height);
  diftray_cell_surface_update(cell->surface(), pixels.data(), width, height);
  diftray_cell_surface_set_highlight(cell->surface(), selected &&
                                        ncursor_view() &&
                                        ncursor_view()->cell_select_mode());
}

void Compositor::request_notelet(Cell *cell, const std::string &key, const std::string &event) {
  auto it = notelet_cells_.find(cell);
  if (it == notelet_cells_.end() || !cell->nterm()) return;
  auto *owner = owner_ncursor(cell);
  const auto output = outputs_.find(owner ? owner->output_name() : active_output_);
  const auto geometry = output != outputs_.end() ? output->second.geometry : OutputGeometry{};
  it->second->set_context({{"rotation", std::to_string(geometry.rotation)},
      {"scale", std::to_string(geometry.scale)},
      {"output_width", std::to_string(geometry.width)}, {"output_height", std::to_string(geometry.height)},
      {"columns", std::to_string(cell->nterm()->columns())},
      {"rows", std::to_string(cell->nterm()->rows())}, {"cell", cell->id()},
      {"workspace", std::to_string(owner ? owner->workspace() : current_workspace_)},
      {"output", owner ? owner->output_name() : active_output_},
      {"outputs", list_outputs()}, {"cursors", list_cursor_ids()}});
  std::string error;
  if (display_) {
    if (!it->second->request_render(key, error, event)) status_line_ = error;
    if (notelet_timer_) wl_event_source_timer_update(notelet_timer_, 10);
  } else {
    if (!it->second->render(key, error, event)) status_line_ = error;
    else paint_notelet(cell);
  }
}

int Compositor::notelets_ready(void *userdata) {
  auto *self = static_cast<Compositor *>(userdata);
  bool busy = false, changed = false;
  for (auto &[cell, app] : self->notelet_cells_) {
    std::string error;
    if (app->poll(error)) {
      if (!error.empty()) self->status_line_ = app->id() + ": " + error;
      else {
        self->paint_notelet(cell);
        self->render_cell(cell, cell == self->active_cell());
      }
      changed = true;
    }
    busy |= app->busy();
  }
  if (changed) self->update_chrome();
  self->flush_extension_commands();
  if (busy || (self->lua_engine_ && !self->lua_engine_->extensions().empty()))
    wl_event_source_timer_update(self->notelet_timer_, 10);
  return 0;
}

std::string Compositor::refresh_notelet() {
  Cell *cell = active_cell();
  if (!cell || !notelet_cells_.count(cell)) return "no active notelet";
  request_notelet(cell, "", "refresh");
  render_cell(cell, true);
  return "notelet refresh requested";
}

void Compositor::paint_notelet(Cell *cell) {
  auto it = notelet_cells_.find(cell);
  if (it == notelet_cells_.end() || !cell->nterm()) return;
  cell->nterm()->display("\x1b[2J\x1b[H");
  cell->nterm()->display(it->second->frame());
}

void Compositor::paint_help_pager() {
  if (!help_pager_active_) return;
  auto *cell = active_cell();
  if (!cell || !cell->nterm()) return;
  cell->nterm()->display(help_pager_.render(cell->nterm()->columns(), cell->nterm()->rows()));
  render_cell(cell, true);
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
    if (!owner || owner->output_name() != active_output_) continue;
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
  if (active_output_ == rendering_output_) paint_help_pager();
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
  diftray_wayland_runtime_set_ncursor_visible(wayland_runtime_, true);
  for (auto &cursor : gcursors_) {
    if (!cursor || !cursor->toplevel() || cursor->output_name() != active_output_) {
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
                                             output_x_ + x, output_y_ + config_.status_bar_height,
                                             right - x,
                                             std::max(1, output_height_ - config_.status_bar_height));
    }
  } else if (show_gcursor) {
    diftray_wayland_runtime_layout_gcursor(wayland_runtime_, gcursor->toplevel(),
                                           output_x_, output_y_ + config_.status_bar_height, output_width_,
                                           std::max(1, output_height_ - config_.status_bar_height));
  }
  if (active_output_ != rendering_output_) return;
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
  if (meta_prefix_pending_) {
    chrome += "  |  prefix " + meta_prefix_spec();
  }
  diftray_wayland_runtime_set_status_line(wayland_runtime_, chrome.c_str());
  // The launcher lives in the top taskbar, not the bottom command bar, so the
  // two never overlap and the launcher keeps a stable place on screen.
  if (launcher_mode_) {
    diftray_wayland_runtime_set_command_bar(wayland_runtime_, false, "");
  } else if (command_bar_open_ || help_search_open_) {
    std::string prompt;
    if (help_search_open_) {
      prompt = "/" + help_search_input_;
    } else {
      prompt = command_bar_.scope == CommandScope::NCURSOR_GLOBAL ? "global: " : ":";
      prompt += command_bar_.input_buffer;
    }
    diftray_wayland_runtime_set_command_bar(wayland_runtime_, true, prompt.c_str());
  } else {
    diftray_wayland_runtime_set_command_bar(wayland_runtime_, false, "");
  }
  refresh_launcher_bar();
}

void Compositor::layout_current_output() {
  auto *ncursor = ncursor_view();
  if (!ncursor) {
    return;
  }
  // The command bar and help search draw as a Wayland overlay on top of the
  // cells. Their height is deliberately NOT subtracted here: shrinking the
  // cells would resize their PTYs, scrolling terminal content (including the
  // shell prompt and any half-typed line) into scrollback every time `:` is
  // pressed and leaving blank rows behind when the bar closes.
  const int usable_height = output_height_ - config_.status_bar_height;
  const auto workspace_views = ncursors_on_workspace(current_workspace_);
  if (tcursor_view_ && active_view_ == tcursor_view_.get() &&
      tcursor_workspace_ == current_workspace_ && active_cell()) {
    active_cell()->set_box({output_x_, output_y_ + config_.status_bar_height, output_width_, std::max(1, usable_height)});
  } else if (config_.ncursor_mode == "tab" && workspace_views.size() > 1) {
    ncursor->set_output_box({output_x_, output_y_ + config_.status_bar_height, output_width_, std::max(1, usable_height)});
    ncursor->layout();
  } else if (workspace_views.size() > 1 && config_.ncursor_mode == "stack") {
    const int band =
        std::max(1, usable_height / static_cast<int>(workspace_views.size()));
    int y = output_y_ + config_.status_bar_height;
    for (auto *view : workspace_views) {
      view->set_output_box({output_x_, y, output_width_, band});
      view->layout();
      y += band;
    }
  } else {
    ncursor->set_output_box({output_x_, output_y_ + config_.status_bar_height, output_width_, std::max(1, usable_height)});
    ncursor->layout();
  }
  apply_view_visibility();
  render_all_cells();
}

int Compositor::terminal_fd_ready(int fd, uint32_t mask, void *data) {
  auto *compositor = static_cast<Compositor *>(data);
  if (!compositor || !(mask & (WL_EVENT_READABLE | WL_EVENT_WRITABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
    return 0;
  }
  auto it = compositor->pty_cells_.find(fd);
  if (it == compositor->pty_cells_.end() || !it->second || !it->second->nterm()) {
    return 0;
  }
  auto *cell = it->second;
  if (mask & WL_EVENT_WRITABLE) cell->nterm()->flush_input();
  if (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR)) cell->nterm()->on_readable();
  if (!cell->nterm()->running()) compositor->unwatch_cell_pty(cell);
  else wl_event_source_fd_update(compositor->pty_sources_.at(fd), WL_EVENT_READABLE |
      (cell->nterm()->input_pending() ? WL_EVENT_WRITABLE : 0));
  compositor->render_cell(cell, cell == compositor->active_cell());
  if (compositor->help_pager_active_ && cell == compositor->active_cell()) {
    compositor->paint_help_pager();
  }
  return 0;
}

bool Compositor::terminal_key_received(void *userdata, uint32_t keysym,
                                       uint32_t modifiers, uint32_t state,
                                       uint32_t unicode, uint32_t, uint32_t keycode) {
  auto *compositor = static_cast<Compositor *>(userdata);
  if (compositor) compositor->notify_extensions("input");
  return compositor && compositor->handle_key(keysym, modifiers, state, unicode, keycode);
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
  std::vector<OutputGeometry> geometry;
  diftray_output_geometry item{};
  for (size_t i = 0; diftray_wayland_runtime_output_at(wayland_runtime_, i, &item); ++i)
    geometry.push_back({item.name, item.x, item.y, item.width, item.height, item.rotation, item.scale});
  synchronize_outputs(geometry);
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
  auto *parent = owner ? owner_ncursor(owner) : active_ncursor_;
  cursor->set_owner_ncursor(parent);
  cursor->set_workspace(parent ? parent->workspace() : current_workspace_);
  cursor->set_output_name(parent ? parent->output_name() : active_output_);
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
  for (auto &cell : cells_) cell->cursor_area().forget(cursor);
  for (auto &view : ncursor_views_) view->cursor_area().forget(cursor);
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

// The compositor's Meta prefix. The keymap INI is the authority, because the
// user asked for every key to live there; the config program's `prefix`
// variable and the built-in Ctrl+Q remain as fallbacks for a session with no
// INI, so removing the file does not lock anyone out.
// The seat's keymap and state, for Remap(). Both are null before a seat
// exists, which is the normal state in the headless tests.
struct xkb_keymap *Compositor::seat_keymap() const {
  return wayland_runtime_ ? diftray_wayland_runtime_seat_keymap(wayland_runtime_) : nullptr;
}

struct xkb_state *Compositor::seat_state() const {
  return wayland_runtime_ ? diftray_wayland_runtime_seat_state(wayland_runtime_) : nullptr;
}

std::string Compositor::meta_prefix_spec() const {
  KeyChord prefix;
  if (keymap_.meta_prefix_chord(prefix)) {
    return prefix.str();
  }
  if (config_.program) {
    const auto &variables = config_.program->variables();
    if (const auto it = variables.find("prefix"); it != variables.end()) {
      if (const auto *text = std::get_if<std::string>(&it->second)) {
        uint32_t mods = 0, sym = 0;
        if (parse_meta_chord(*text, mods, sym)) {
          return *text;
        }
      }
    }
  }
  return "Ctrl+Q";
}

bool Compositor::load_keymap(std::string &error) {
  error.clear();
  keymap_ = Keymap{};
  keymap_error_.clear();
  if (config_.keymap.empty()) {
    keymap_profile_.clear();
    return true;
  }
  // Relative paths resolve against the config file's directory, the same rule
  // the theme and help paths already use, so a config directory stays
  // self-contained.
  std::filesystem::path path(config_.keymap);
  if (path.is_relative() && !config_path_.empty()) {
    path = std::filesystem::path(config_path_).parent_path() / path;
  }
  path = path.lexically_normal();
  // Qualified: the member function shadows the free function of the same name.
  if (!::load_keymap(path.string(), keymap_, error)) {
    keymap_error_ = error;
    keymap_ = Keymap{};
    keymap_profile_.clear();
    return false;
  }
  keymap_profile_ = keymap_.default_profile;
  return true;
}

void Compositor::keymap_select_profile(const std::string &name) {
  std::string error;
  if (keymap_.profile_named(name, error)) {
    keymap_profile_ = name;
  } else {
    status_line_ = error;
  }
}

void Compositor::keymap_reset_profile() {
  keymap_profile_ = keymap_.default_profile;
  status_line_ = "keymap profile: " + keymap_profile_;
}

std::string Compositor::keymap_info() const {
  if (!keymap_error_.empty()) {
    return "keymap: " + keymap_error_ + " (built-in keys in force)";
  }
  if (keymap_.empty()) {
    return "keymap: none loaded (" +
           (config_.keymap.empty() ? std::string("no keymap configured")
                                   : config_.keymap) +
           ")";
  }
  std::string out = keymap_.summary();
  out += "active profile: " + (keymap_profile_.empty() ? "(none)" : keymap_profile_) + "\n";
  out += keymap_.describe_bindings();
  return out;
}

bool Compositor::apply_keymap(uint32_t keysym, uint32_t modifiers, uint32_t keycode) {
  if (keymap_.empty()) {
    return false;
  }
  // The evdev backend and the compositor agree on chord identity: the kernel
  // key code is the XKB key code minus the eight keys libxkbcommon reserves,
  // and the modifier bits are the same set libinput reports.
  KeyChord chord;
  chord.code = keycode >= 8 ? keycode - 8 : 0;
  chord.mods = keymap_mods(modifiers);
  if (chord.code == 0) {
    return false;
  }
  // The [init] prefix switches profile and is swallowed, so it never reaches
  // the terminal. Its release is swallowed too, or the character would appear
  // when the user let go.
  if (keymap_.prefix.code == chord.code && keymap_.prefix.mods == chord.mods) {
    if (keymap_.prefix_action.kind == Action::Kind::trigger) {
      keymap_select_profile(keymap_.prefix_action.profile);
    }
    return true;
  }
  const Action *action = keymap_.lookup(keymap_profile_, chord);
  if (!action) {
    return false;
  }
  switch (action->kind) {
    case Action::Kind::ignore:
      return true;
    case Action::Kind::trigger:
      keymap_select_profile(action->profile);
      return true;
    case Action::Kind::diftray: {
      // Anything a user can type after `:` is available here, which is what
      // AGENTS.md section 12.4 asks of every compositor action.
      const bool ok = command_bar_.dispatch(action->text);
      status_line_ = ok ? "keymap: " + action->text
                        : "keymap: " + action->text + " rejected: " +
                              command_bar_.status_line();
      return true;
    }
    case Action::Kind::exec: {
      // The same path `launch` uses, so a keymap-launched program inherits the
      // session's WAYLAND_DISPLAY and is tracked in launched_pids_.
      status_line_ = launch_program(action->text);
      return true;
    }
    case Action::Kind::typeout: {
      status_line_ = type_out_text(action->text);
      return true;
    }
    case Action::Kind::remap: {
      // A remap redirects the chord: the target is looked up with the same
      // rules as any other key, so a profile can rewrite <C-q> into a Meta
      // binding. The translation needs a keysym, which only the seat's state can
      // supply, so without one the key is swallowed rather than guessed at. The
      // guard makes a remap cycle terminate instead of recursing.
      if (keymap_remap_guard_ || !seat_keymap() || !seat_state()) {
        return true;
      }
      const xkb_keycode_t target_code = action->chord.code + 8;
      const xkb_keysym_t target_sym = xkb_state_key_get_one_sym(seat_state(), target_code);
      if (target_sym == XKB_KEY_NoSymbol) {
        return true;
      }
      keymap_remap_guard_ = true;
      struct Guard {
        bool *flag;
        ~Guard() { *flag = false; }
      } guard{&keymap_remap_guard_};
      // xkb_state_key_get_utf8 writes into a caller-supplied buffer, so ask for
      // the target character the same way the input path does: a remap that
      // reaches a printable key must still type it.
      char utf8[7] = {};
      xkb_state_key_get_utf8(seat_state(), target_code, utf8, sizeof(utf8));
      handle_key(target_sym, wlr_mods(action->chord.mods),
                 WL_KEYBOARD_KEY_STATE_PRESSED,
                 utf8[0] ? static_cast<uint32_t>(static_cast<unsigned char>(utf8[0])) : 0,
                 target_code);
      return true;
    }
    case Action::Kind::none:
      break;
  }
  return false;
}

bool Compositor::match_meta_prefix(uint32_t keysym, uint32_t modifiers,
                                  uint32_t keycode) const {
  // The keymap's [meta] prefix is the authority when it sets one, and it spells
  // chords as a key code plus modifier bits -- so compare those directly.
  // Routing the INI's "C-q" through the legacy "Ctrl+Q" string parser below
  // would never match, because that parser splits on '+' and needs a separate
  // key name.
  KeyChord prefix;
  if (keymap_.meta_prefix_chord(prefix)) {
    if (prefix.mods != keymap_mods(modifiers)) {
      return false;
    }
    if (keycode >= 8 && keycode - 8 == prefix.code) {
      return true;
    }
    // Without a usable key code -- headless tests, or a key event that arrived
    // before the seat had one -- ask the seat's keymap what the prefix key
    // produces and compare keysyms. With no seat there is nothing to compare
    // against, and refusing is the safe answer: a false positive would arm the
    // prefix on an unrelated key.
    if (seat_keymap() && seat_state()) {
      return xkb_state_key_get_one_sym(seat_state(), prefix.code + 8) == keysym;
    }
    return false;
  }
  uint32_t mods = 0, sym = 0;
  if (!parse_meta_chord(meta_prefix_spec(), mods, sym)) {
    return false;
  }
  if ((modifiers & kBindingMods) != mods) {
    return false;
  }
  return chord_keysym_matches(keysym, sym);
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

bool Compositor::help_key_matches(const std::string &binding, uint32_t keysym,
                                  uint32_t unicode) const {
  std::string normalized;
  normalized.reserve(binding.size());
  for (unsigned char ch : binding) normalized.push_back(static_cast<char>(std::tolower(ch)));
  if (normalized == "space") return keysym == XKB_KEY_space;
  if (normalized == "pagedown" || normalized == "page-down") return keysym == XKB_KEY_Page_Down;
  if (normalized == "pageup" || normalized == "page-up") return keysym == XKB_KEY_Page_Up;
  if (normalized == "up") return keysym == XKB_KEY_Up;
  if (normalized == "down") return keysym == XKB_KEY_Down;
  return binding.size() == 1 && unicode != 0 &&
         static_cast<unsigned char>(binding[0]) == static_cast<unsigned char>(unicode);
}

bool Compositor::feed_help_search_key(uint32_t keysym, uint32_t unicode) {
  if (keysym == XKB_KEY_Escape) {
    help_search_open_ = false;
    help_search_input_.clear();
  } else if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
    status_line_ = find_help(help_search_input_);
    help_search_open_ = false;
    help_search_input_.clear();
  } else if (keysym == XKB_KEY_BackSpace) {
    if (!help_search_input_.empty()) help_search_input_.pop_back();
  } else if (unicode >= 32 && unicode < 127) {
    help_search_input_.push_back(static_cast<char>(unicode));
  }
  relayout();
  return true;
}

bool Compositor::handle_help_pager_key(uint32_t keysym, uint32_t unicode) {
  if (keysym == XKB_KEY_Escape || help_key_matches(keymap_.help_key_close, keysym, unicode)) {
    help_pager_active_ = false;
    help_search_open_ = false;
    status_line_ = "help closed";
    relayout();
    return true;
  }
  if (help_key_matches(keymap_.help_key_search, keysym, unicode)) {
    help_search_open_ = true;
    help_search_input_.clear();
    relayout();
    return true;
  }
  std::string message;
  bool changed = false;
  const std::size_t rows = active_cell() && active_cell()->nterm()
                               ? active_cell()->nterm()->rows()
                               : 24;
  if (help_key_matches(keymap_.help_key_next, keysym, unicode)) {
    changed = help_pager_.next_match(message);
  } else if (help_key_matches(keymap_.help_key_previous, keysym, unicode)) {
    changed = help_pager_.previous_match(message);
  } else if (help_key_matches(keymap_.help_key_page_down, keysym, unicode)) {
    changed = help_pager_.scroll_pages(1, rows > 1 ? rows - 1 : 1, message);
  } else if (help_key_matches(keymap_.help_key_page_up, keysym, unicode)) {
    changed = help_pager_.scroll_pages(-1, rows > 1 ? rows - 1 : 1, message);
  } else if (help_key_matches(keymap_.help_key_line_down, keysym, unicode) || keysym == XKB_KEY_Down) {
    changed = help_pager_.scroll_lines(1, rows > 1 ? rows - 1 : 1, message);
  } else if (help_key_matches(keymap_.help_key_line_up, keysym, unicode) || keysym == XKB_KEY_Up) {
    changed = help_pager_.scroll_lines(-1, rows > 1 ? rows - 1 : 1, message);
  } else {
    return true;
  }
  if (!message.empty()) status_line_ = message;
  if (changed) paint_help_pager();
  update_chrome();
  return true;
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
                            uint32_t unicode, uint32_t keycode) {
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    return command_bar_open_ || active_gcursor() == nullptr;
  }
  if (pending_kill_) {
    Cell *target = pending_kill_;
    if (keysym == XKB_KEY_y || keysym == XKB_KEY_Y || keysym == XKB_KEY_Return) {
      pending_kill_ = nullptr;
      status_line_ = erase_cell(target);
    } else if (keysym == XKB_KEY_n || keysym == XKB_KEY_N || keysym == XKB_KEY_Escape) {
      pending_kill_ = nullptr;
      status_line_ = "cell removal cancelled";
    }
    update_chrome();
    return true;
  }
  const uint32_t real_mods = modifiers;
  const bool real_meta = (real_mods & WLR_MODIFIER_LOGO) != 0;
  if (meta_prefix_pending_ && keysym == XKB_KEY_Escape && !real_meta) {
    meta_prefix_pending_ = false;
    status_line_ = "prefix cancelled";
    update_chrome();
    return true;
  }
  if (real_meta && keysym == XKB_KEY_Escape) {
    stop();
    return true;
  }
  if (help_search_open_) {
    return feed_help_search_key(keysym, unicode);
  }
  if (command_bar_open_ && !(real_meta && keysym == XKB_KEY_colon)) {
    return feed_command_bar_key(keysym, unicode);
  }
  // An armed Meta prefix (Ctrl+Q by default) lends the Logo modifier to this
  // keypress only. The flag is consumed here no matter how the key is
  // handled below; arming afresh happens after the binding lookup.
  uint32_t mods = real_mods;
  bool meta = real_meta;
  if (meta_prefix_pending_) {
    mods |= WLR_MODIFIER_LOGO;
    meta = true;
    meta_prefix_pending_ = false;
  }
  if (config_.program && !(help_pager_active_ && !meta)) {
    // Physical bindings take precedence when both match the same event.
    for (bool physical : {true, false}) {
      for (const auto &binding : config_.program->bindings()) {
        if (std::holds_alternative<ConfigKeycode>(binding.key) != physical ||
            !binding.matches(keysym, keycode, mods)) continue;
        const auto saved_scope = command_bar_.scope;
        command_bar_.scope = binding.global ? CommandScope::NCURSOR_GLOBAL : CommandScope::CELL;
        for (const auto &command : binding.commands)
          if (!command_bar_.dispatch(command)) break;
        command_bar_.scope = saved_scope;
        status_line_ = command_bar_.status_line();
        update_chrome();
        return true;
      }
    }
  }
  // The prefix chord itself arms Meta for the next key unless a binding above
  // consumed it. Pressing the chord twice in a row stays armed. This is checked
  // before the keymap profiles so a profile cannot consume the chord the Meta
  // prefix needs, and after the config program's `bind()` entries so an explicit
  // binding still wins.
  if (match_meta_prefix(keysym, real_mods, keycode)) {
    meta_prefix_pending_ = true;
    status_line_ = "prefix " + meta_prefix_spec() + " (Meta) - next key";
    update_chrome();
    return true;
  }
  // Then the keymap INI, so a profile can redefine any built-in binding. It
  // runs last of the three because the two above are more specific: a binding
  // the user wrote down, and the chord that makes Meta reachable at all.
  if (apply_keymap(keysym, real_mods, keycode)) {
    update_chrome();
    return true;
  }
  if (meta && (keysym == XKB_KEY_colon || keysym == XKB_KEY_semicolon)) {
    open_command_bar(CommandScope::NCURSOR_GLOBAL, "");
    return true;
  }
  if (!meta && keysym == XKB_KEY_colon && !active_gcursor()) {
    open_command_bar(CommandScope::CELL, "");
    return true;
  }
  if (help_pager_active_ && !meta) {
    return handle_help_pager_key(keysym, unicode);
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
  // Multiplexer focus with an extra Ctrl takes precedence over the plain
  // Meta+arrow bindings below (tab cycling and cell reordering).
  const bool ctrl = (mods & WLR_MODIFIER_CTRL) != 0;
  if (meta && ctrl && (keysym == XKB_KEY_Up || keysym == XKB_KEY_KP_Up)) {
    status_line_ = mux_focus("up");
    return true;
  }
  if (meta && ctrl && (keysym == XKB_KEY_Down || keysym == XKB_KEY_KP_Down)) {
    status_line_ = mux_focus("down");
    return true;
  }
  if (meta && ctrl && (keysym == XKB_KEY_Left || keysym == XKB_KEY_KP_Left)) {
    status_line_ = mux_focus("left");
    return true;
  }
  if (meta && ctrl && (keysym == XKB_KEY_Right || keysym == XKB_KEY_KP_Right)) {
    status_line_ = mux_focus("right");
    return true;
  }
  if (meta && (keysym == XKB_KEY_s || keysym == XKB_KEY_S)) {
    status_line_ = mux_split(false);
    return true;
  }
  if (meta && (keysym == XKB_KEY_v || keysym == XKB_KEY_V)) {
    status_line_ = mux_split(true);
    return true;
  }
  if (meta && (keysym == XKB_KEY_o || keysym == XKB_KEY_O)) {
    status_line_ = mux_focus("next");
    return true;
  }
  if (meta && (keysym == XKB_KEY_p || keysym == XKB_KEY_P)) {
    status_line_ = mux_focus("prev");
    return true;
  }
  if (meta && (keysym == XKB_KEY_z || keysym == XKB_KEY_Z)) {
    status_line_ = mux_zoom();
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
    pending_kill_ = active_cell();
    status_line_ = pending_kill_ ? "Kill cell " + pending_kill_->id() + "? y / n" : "no active cell";
    relayout();
    return true;
  }
  if (meta && (keysym == XKB_KEY_bracketleft || keysym == XKB_KEY_bracketright)) {
    status_line_ = focus_output(keysym == XKB_KEY_bracketleft ? "prev" : "next");
    update_chrome();
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
      else if (keysym == XKB_KEY_Tab) key = "Tab";
      else if (keysym == XKB_KEY_Delete) key = "Delete";
      else if (keysym == XKB_KEY_Home) key = "Home";
      else if (keysym == XKB_KEY_End) key = "End";
      else if (keysym == XKB_KEY_Page_Up) key = "PageUp";
      else if (keysym == XKB_KEY_Page_Down) key = "PageDown";
      else if (unicode >= 32 && unicode <= 0x10ffff && !(unicode >= 0xd800 && unicode <= 0xdfff)) {
        if (unicode < 0x80) key += static_cast<char>(unicode);
        else if (unicode < 0x800) {
          key += static_cast<char>(0xc0 | (unicode >> 6));
          key += static_cast<char>(0x80 | (unicode & 63));
        } else if (unicode < 0x10000) {
          key += static_cast<char>(0xe0 | (unicode >> 12));
          key += static_cast<char>(0x80 | ((unicode >> 6) & 63));
          key += static_cast<char>(0x80 | (unicode & 63));
        } else {
          key += static_cast<char>(0xf0 | (unicode >> 18));
          key += static_cast<char>(0x80 | ((unicode >> 12) & 63));
          key += static_cast<char>(0x80 | ((unicode >> 6) & 63));
          key += static_cast<char>(0x80 | (unicode & 63));
        }
      }
      if (!key.empty()) {
        request_notelet(cell, key, "key");
        render_cell(cell, true);
        update_chrome();
      }
      return true;
    }
    cell->nterm()->handle_key(keysym, keysym < 128 ? keysym : 0, tsm_mods(real_mods),
                              unicode);
    if (auto source = pty_sources_.find(cell->nterm()->master_fd()); source != pty_sources_.end())
      wl_event_source_fd_update(source->second, WL_EVENT_READABLE |
          (cell->nterm()->input_pending() ? WL_EVENT_WRITABLE : 0));
    render_cell(cell, true);
    return true;
  }
  return true;
}

std::string Compositor::open_help_page(const std::string &topic) {
  std::string message;
  if (!help_pager_.open(topic, message)) return message;
  if (active_gcursor()) set_active_view(ncursor_view());
  help_pager_active_ = true;
  help_search_open_ = false;
  relayout();
  return message;
}

std::string Compositor::find_help(const std::string &pattern) {
  std::string message;
  if (!help_pager_.find(pattern, message)) return message;
  if (help_pager_active_) paint_help_pager();
  return message;
}

std::string Compositor::set_help_bookmark(const std::string &name) {
  std::string message;
  help_pager_.set_bookmark(name, message);
  return message;
}

std::string Compositor::open_help_bookmark(const std::string &name) {
  std::string message;
  if (!help_pager_.open_bookmark(name, message)) return message;
  help_pager_active_ = true;
  relayout();
  return message;
}

void Compositor::set_active_view(View *view) {
  if (view && view->output_name() != active_output_) focus_output(view->output_name());
  if (view && view->type() == ViewType::GCURSOR) {
    auto *cursor = static_cast<GCursorView *>(view);
    if (cursor->workspace() != current_workspace_) switch_workspace(cursor->workspace());
    if (cursor->owner_ncursor()) active_ncursor_ = cursor->owner_ncursor();
  }
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
  cells_.push_back(std::make_unique<Cell>(ncursor_view()->default_shell().empty() ? config_.shell : ncursor_view()->default_shell()));
  auto *cell = cells_.back().get();
  if (!ncursor_view()->insert_cell(cell, above)) {
    cells_.pop_back();
    return "spawn failed: could not insert cell";
  }
  attach_cell_surface(cell);
  if (display_ && cell->nterm()) {
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
  // Graphical clients outlive the terminal that launched them. Transfer their
  // dock ownership before destroying the cell to avoid dangling pointers.
  auto *parent = owner_ncursor(cell);
  for (auto &cursor : gcursors_) {
    if (cursor->owner_cell() != cell) continue;
    cell->cursor_area().forget(cursor.get());
    cursor->set_owner_cell(nullptr);
    cursor->set_owner_ncursor(parent);
    if (parent && cursor->docked()) parent->cursor_area().dock(cursor.get());
  }
  if (pending_kill_ == cell) pending_kill_ = nullptr;
  detach_cell_surface(cell);
  notelet_cells_.erase(cell);
  if (auto *owner = owner_ncursor(cell)) {
    owner->remove_cell(cell);
  } else if (ncursor_view()) {
    ncursor_view()->remove_cell(cell);
  }
  if (tcursor_view_ && tcursor_view_->cell() == cell) {
    tcursor_view_.reset();
    tcursor_workspace_ = 0;
    active_view_ = ncursor_view();
  }
  cells_.erase(std::remove_if(cells_.begin(), cells_.end(),
                              [cell](const std::unique_ptr<Cell> &owned) {
                                return owned.get() == cell;
                              }),
               cells_.end());
  if (parent && !parent->active_cell()) {
    NCursorView *replacement = nullptr;
    for (auto *view : ncursors_on_workspace(parent->workspace()))
      if (view != parent && view->active_cell()) { replacement = view; break; }
    if (replacement) {
      for (auto &cursor : gcursors_) {
        if (cursor->owner_ncursor() != parent) continue;
        parent->cursor_area().forget(cursor.get());
        cursor->set_owner_ncursor(replacement);
        if (cursor->docked()) replacement->cursor_area().dock(cursor.get());
      }
      for (auto &[name, output] : outputs_)
        for (int ws = kMinWorkspace; ws <= kMaxWorkspace; ++ws) {
          if (output.ncursors[ws] == parent) output.ncursors[ws] = replacement;
          if (output.views[ws] == parent) output.views[ws] = replacement;
        }
      if (active_ncursor_ == parent) active_ncursor_ = replacement;
      if (active_view_ == parent) active_view_ = replacement;
      ncursor_views_.erase(std::remove_if(ncursor_views_.begin(), ncursor_views_.end(),
          [parent](const auto &view) { return view.get() == parent; }), ncursor_views_.end());
    }
  }
  rebuild_command_context();
  relayout();
  return "removed cell";
}

std::string Compositor::kill_selected_cell() { return erase_cell(active_cell()); }

std::string Compositor::mux_split(bool vertical) {
  if (!ncursor_view()) {
    return "split failed: no ncursor view";
  }
  cells_.push_back(std::make_unique<Cell>(ncursor_view()->default_shell().empty() ? config_.shell : ncursor_view()->default_shell()));
  auto *cell = cells_.back().get();
  const bool inserted =
      vertical ? ncursor_view()->split_stack(cell) : ncursor_view()->insert_cell(cell, false);
  if (!inserted) {
    cells_.pop_back();
    return "split failed: could not insert cell";
  }
  attach_cell_surface(cell);
  if (display_ && cell->nterm()) {
    cell->nterm()->start();
    watch_cell_pty(cell);
  }
  relayout();
  rebuild_command_context();
  return std::string("split cell ") + cell->id() + (vertical ? " vertical" : " horizontal");
}

std::string Compositor::mux_focus(const std::string &direction) {
  auto *view = ncursor_view();
  if (!view) {
    return "focus failed: no ncursor view";
  }
  bool moved = false;
  if (direction == "next") {
    moved = view->focus_next_cell();
  } else if (direction == "prev" || direction == "previous") {
    moved = view->focus_prev_cell();
  } else if (direction == "up") {
    moved = view->focus_up_cell();
  } else if (direction == "down") {
    moved = view->focus_down_cell();
  } else if (direction == "left") {
    moved = view->focus_left_cell();
  } else if (direction == "right") {
    moved = view->focus_right_cell();
  } else {
    return "mux focus requires next, prev, up, down, left, or right";
  }
  if (!moved) {
    if (direction == "next" || direction == "prev" || direction == "previous") {
      return "focus failed: no panes";
    }
    const std::string where = direction == "up" ? "above"
                              : direction == "down" ? "below"
                              : direction == "left" ? "to the left"
                                                    : "to the right";
    return "no pane " + where;
  }
  relayout();
  rebuild_command_context();
  if (auto *cell = view->active_cell()) {
    return "focused cell " + cell->id();
  }
  return "focused cell";
}

std::string Compositor::mux_kill() { return erase_cell(active_cell()); }

std::string Compositor::mux_zoom() {
  if (tcursor_view_ && tcursor_workspace_ == current_workspace_) {
    return restore_tcursor();
  }
  if (tcursor_view_) {
    tcursor_view_.reset();
    tcursor_workspace_ = 0;
  }
  return promote_active_cell_to_tcursor();
}

std::string Compositor::mux_list() const {
  auto *view = ncursor_view();
  if (!view) {
    return "no ncursor view";
  }
  Cell *focused = active_cell();
  std::ostringstream out;
  std::size_t count = 0;
  for (const auto &stack : view->cell_stacks()) {
    for (auto *cell : stack.cells) {
      if (!cell || !cell->nterm()) {
        continue;
      }
      if (count > 0) {
        out << '\n';
      }
      out << (cell == focused ? "* " : "  ") << cell->id() << ' '
          << cell->nterm()->columns() << 'x' << cell->nterm()->rows();
      ++count;
    }
  }
  return count == 0 ? "no panes" : out.str();
}

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
  tcursor_view_->set_output_name(active_output_);
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
  if (auto *cell = cursor->owner_cell()) {
    cell->cursor_area().dock(cursor);
  } else if (cursor->owner_ncursor()) {
    cursor->owner_ncursor()->cursor_area().dock(cursor);
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
  } else if (cursor->owner_ncursor()) {
    cursor->owner_ncursor()->cursor_area().restore(cursor);
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
    if (cursor && cursor->docked() &&
        (command_bar_.scope == CommandScope::CELL ? cursor->owner_cell() == active_cell()
          : cursor->owner_ncursor() == active_ncursor_ && !cursor->owner_cell())) {
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
  if (quick_restore_[slot]) quick_restore_[slot]->set_quick_restore_slot(std::nullopt);
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
  if (shell.empty() || (shell != "libshell" && ::access(shell.c_str(), X_OK) != 0)) return "shell is not executable: " + shell;
  auto change = [&](Cell *cell, bool individual) {
    unwatch_cell_pty(cell);
    cell->set_shell_override(shell, individual);
    if (display_ && !notelet_cells_.count(cell)) {
      cell->nterm()->start();
      watch_cell_pty(cell);
    }
  };
  if (scope == CommandScope::CELL) {
    if (!active_cell()) return "set shell failed: no active cell";
    change(active_cell(), true);
    return "set cell shell to " + shell;
  }
  if (!active_ncursor_) return "set shell failed: no active ncursor";
  active_ncursor_->set_default_shell(shell);
  for (auto &cell : cells_)
    if (owner_ncursor(cell.get()) == active_ncursor_ && !cell->has_shell_override()) change(cell.get(), false);
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
    if (key == "launcher-bar-color" && !theme_color(value, next.launcher_bar_color)) return "invalid launcher-bar-color";
    if (key == "border-size" && !theme_pixels(value, next.border_size)) return "invalid border-size";
    if (key == "command-bar-height" && !theme_pixels(value, next.command_bar_height)) return "invalid command-bar-height";
    if (key == "status-bar-height" && !theme_pixels(value, next.status_bar_height)) return "invalid status-bar-height";
    if (key == "launcher-bar-height" && !theme_pixels(value, next.launcher_bar_height)) return "invalid launcher-bar-height";
  }
  config_ = std::move(next);
  theme_engine_->swap_active(std::move(parsed));
  if (wayland_runtime_) {
    const diftray_wayland_style style = wayland_style();
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

// Types text into the focused cell's terminal, which is what a keymap's
// Typeout() does. Refuses when no cell is focused rather than writing into
// nothing, and reports the outcome so the status line can say what happened.
std::string Compositor::type_out_text(const std::string &text) {
  Cell *target = active_cell();
  if (!target || !target->nterm() || !target->nterm()->running()) {
    return "keymap: no running terminal to type into";
  }
  target->nterm()->feed_input(text);
  target->nterm()->flush_input();
  return "keymap: typed " + std::to_string(text.size()) + " characters";
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
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, nullptr);
    if (display_) {
      setenv("WAYLAND_DISPLAY", wayland_socket_.c_str(), 1);
    }
    ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  launched_pids_.push_back(pid);
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
  auto cell = std::make_unique<Cell>(config_.shell);
  Cell *raw = cell.get();
  if (!ncursor_view()->insert_cell(raw, false)) return "notelet cell creation failed";
  cells_.push_back(std::move(cell));
  notelet_cells_.emplace(raw, std::move(notelet));
  request_notelet(raw, "", "open");
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
  view->set_id("ncursor-" + std::to_string(next_ncursor_id_++));
  view->set_workspace(current_workspace_);
  view->set_output_name(active_output_);
  cells_.push_back(std::make_unique<Cell>(config_.shell));
  view->insert_cell(cells_.back().get(), false);
  attach_cell_surface(cells_.back().get());
  if (display_ && cells_.back()->nterm()) {
    cells_.back()->nterm()->start();
    watch_cell_pty(cells_.back().get());
  }
  active_ncursor_ = view.get();
  active_view_ = view.get();
  ncursor_views_.push_back(std::move(view));
  outputs_[active_output_].ncursors[current_workspace_] = active_ncursor_;
  outputs_[active_output_].views[current_workspace_] = active_ncursor_;
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
    if (view && view->workspace() == workspace && view->output_name() == active_output_) {
      views.push_back(view.get());
    }
  }
  return views;
}

std::vector<GCursorView *> Compositor::live_gcursors_on_workspace(int workspace) const {
  std::vector<GCursorView *> views;
  for (const auto &cursor : gcursors_) {
    if (cursor && !cursor->docked() && cursor->workspace() == workspace && cursor->output_name() == active_output_) {
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
  outputs_[active_output_].ncursors[current_workspace_] = active_ncursor_;
  outputs_[active_output_].views[current_workspace_] = active_view_;
}

void Compositor::restore_workspace_focus() {
  NCursorView *ncursor = nullptr;
  if (current_workspace_ >= kMinWorkspace && current_workspace_ <= kMaxWorkspace) {
    ncursor = outputs_[active_output_].ncursors[current_workspace_];
  }
  bool ncursor_ok = false;
  for (const auto &view : ncursor_views_) {
    if (ncursor && view.get() == ncursor &&
        ncursor->workspace() == current_workspace_ && ncursor->output_name() == active_output_) {
      ncursor_ok = true;
      break;
    }
  }
  if (!ncursor_ok) {
    ncursor = nullptr;
    for (const auto &view : ncursor_views_) {
      if (view && view->workspace() == current_workspace_ && view->output_name() == active_output_) {
        ncursor = view.get();
        break;
      }
    }
  }
  active_ncursor_ = ncursor;
  View *view = nullptr;
  if (current_workspace_ >= kMinWorkspace && current_workspace_ <= kMaxWorkspace) {
    view = outputs_[active_output_].views[current_workspace_];
  }
  if (!view_exists(view) || view->output_name() != active_output_) {
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
    outputs_[active_output_].views[current_workspace_] = tabs[index];
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
  outputs_[active_output_].ncursors[current_workspace_] = tabs[index];
  outputs_[active_output_].views[current_workspace_] = tabs[index];
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
    if (view && view->workspace() == number && view->output_name() == active_output_) {
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

void Compositor::notify_extensions(const std::string &event) {
  const auto old_status = command_bar_.status_line();
  if (plugin_manager_) plugin_manager_->notify(event);
  if (lua_engine_) lua_engine_->notify(event);
  if (old_status != command_bar_.status_line()) {
    status_line_ = command_bar_.status_line();
    update_chrome();
  }
}

void Compositor::flush_extension_commands() {
  if (!lua_engine_) return;
  const auto before = command_bar_.status_line();
  lua_engine_->drain_commands();
  if (before != command_bar_.status_line()) {
    status_line_ = command_bar_.status_line();
    update_chrome();
  }
}


std::string Compositor::evaluate_config(const std::string &expression, bool run) {
  if (!config_.program) return "configuration program unavailable";
  ConfigValue value;
  std::string error;
  if (!config_.program->evaluate(expression, value, error)) return error;
  if (!run) return ConfigProgram::describe(value);
  std::vector<std::string> commands;
  if (!ConfigProgram::commands(value, commands, error)) return error;
  for (const auto &command : commands)
    if (!command_bar_.dispatch(command)) break;
  return command_bar_.status_line();
}

std::string Compositor::config_variables() const {
  if (!config_.program) return "configuration program unavailable";
  std::string result;
  for (const auto &[name, value] : config_.program->variables())
    result += name + " = " + ConfigProgram::describe(value) + "\n";
  return result.empty() ? "no configuration variables" : result;
}
