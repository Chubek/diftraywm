#include "compositor/Compositor.hpp"
#include "compositor/WaylandRuntime.h"

#include "nterm/NTerm.hpp"
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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <filesystem>
#include <poll.h>
#include <sstream>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

Compositor::Compositor() = default;
Compositor::~Compositor() {
  stop();
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
    status_line_ = "Wayland socket unavailable; using headless mode";
    display_.reset();
  } else {
    wayland_socket_ = registered_socket;
    diftray_wayland_style style{
        config_.border_size,
        config_.command_bar_height,
        {config_.border_color[0], config_.border_color[1], config_.border_color[2],
         config_.border_color[3]},
        {config_.background_color[0], config_.background_color[1],
         config_.background_color[2], config_.background_color[3]},
        {config_.command_bar_color[0], config_.command_bar_color[1],
         config_.command_bar_color[2], config_.command_bar_color[3]}};
    wayland_runtime_ = diftray_wayland_runtime_create(display_.get(), &style);
    if (!wayland_runtime_) {
      status_line_ = "failed to initialize wlroots runtime";
      display_.reset();
    } else {
      diftray_wayland_runtime_set_key_handler(
          wayland_runtime_, &Compositor::terminal_key_received, this);
    }
  }
  ncursor_view_ = std::make_unique<NCursorView>();
  cells_.push_back(std::make_unique<Cell>(config_.shell));
  ncursor_view_->insert_cell(cells_.back().get(), false);
  ncursor_view_->set_cell_select_mode(false);
  wlr_box output_box{0, 0, 120, 40};
  struct winsize terminal_size {};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal_size) == 0) {
    if (terminal_size.ws_col > 0) {
      output_box.width = terminal_size.ws_col;
    }
    if (terminal_size.ws_row > 0) {
      output_box.height = terminal_size.ws_row;
    }
  }
  if (const char *width = std::getenv("DIFTRAYWM_WIDTH")) {
    output_box.width = std::max(1, std::atoi(width));
  }
  if (const char *height = std::getenv("DIFTRAYWM_HEIGHT")) {
    output_box.height = std::max(1, std::atoi(height));
  }
  ncursor_view_->set_output_box(output_box);
  theme_engine_ = new ThemeEngine();
  plugin_manager_ = new PluginManager();
  lua_engine_ = new LuaEngine();
  command_context_.compositor = this;
  command_context_.active_view = ncursor_view_.get();
  command_context_.ncursor_view = ncursor_view_.get();
  command_context_.active_cell = active_cell();
  command_context_.theme_engine = theme_engine_;
  command_context_.plugin_manager = plugin_manager_;
  command_context_.lua_engine = lua_engine_;
  command_context_.status_line = &status_line_;
  command_bar_.set_context(&command_context_);
  active_view_ = ncursor_view_.get();
  if (lua_engine_) {
    lua_engine_->init();
    lua_engine_->scan_extensions();
  }
  if (plugin_manager_) {
    plugin_manager_->discover();
  }
  status_line_ = "ready";
  return true;
}

int Compositor::run() {
  if (!active_view_ || !ncursor_view_) {
    return 1;
  }
  if (display_) {
    std::cout << "DiftrayWM Wayland display: " << wayland_socket_ << '\n';
  }
  auto *term = active_cell() ? active_cell()->nterm() : nullptr;
  if (term && !term->start()) {
    status_line_ = term->last_error();
  }
  active_view_->layout();

  const bool interactive = !display_ && ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
  if (display_) {
    if (!diftray_wayland_runtime_start(wayland_runtime_)) {
      status_line_ = "failed to start wlroots backend";
      return 1;
    }
    refresh_terminal_display();
    if (term && term->master_fd() >= 0) {
      terminal_source_ = wl_event_loop_add_fd(
          wl_display_get_event_loop(display_.get()), term->master_fd(),
          WL_EVENT_READABLE, &Compositor::terminal_fd_ready, this);
      if (!terminal_source_) {
        status_line_ = "failed to watch terminal PTY";
        stop();
        return 1;
      }
    }
    running_ = true;
    wl_display_run(display_.get());
    running_ = false;
    if (terminal_source_) {
      wl_event_source_remove(terminal_source_);
      terminal_source_ = nullptr;
    }
    stop();
    return 0;
  }
  if (interactive) {
    std::cout << "DiftrayWM ready (" << ncursor_view_->cell_stacks().front().cells.size()
              << " cell); type ':' for commands or 'exit' to quit.\n";
    std::cout << "diftray> " << std::flush;
  }

  running_ = true;
  bool command_mode = false;
  std::size_t printed_output = 0;
  while (running_) {
    if (term && !term->running() && term->master_fd() < 0 && term->last_error().empty()) {
      term->start();
    }
    pollfd descriptors[2]{};
    descriptors[0].fd = STDIN_FILENO;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = term ? term->master_fd() : -1;
    descriptors[1].events = descriptors[1].fd >= 0 ? POLLIN : 0;
    const int timeout = interactive ? -1 : 100;
    const int ready = ::poll(descriptors, 2, timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (term && descriptors[1].fd >= 0 &&
        (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      term->on_readable();
      const auto &output = term->scrollback();
      while (printed_output < output.size()) {
        if (interactive) {
          std::cout << output[printed_output] << std::flush;
        }
        ++printed_output;
      }
    }
    if (descriptors[0].revents & (POLLIN | POLLHUP)) {
      std::string line;
      if (!std::getline(std::cin, line)) {
        running_ = false;
        continue;
      }
      if (!command_mode && (line == "exit" || line == "quit")) {
        running_ = false;
      } else if (!command_mode && line == ":") {
        command_mode = true;
        command_bar_.visible = true;
        if (interactive) {
          std::cout << "command: " << std::flush;
        }
        continue;
      } else if (!command_mode && !line.empty() && line.front() == ':') {
        command_mode = true;
        command_bar_.visible = true;
        line.erase(line.begin());
        command_bar_.dispatch(line);
        term = active_cell() ? active_cell()->nterm() : nullptr;
        printed_output = 0;
        if (term && !term->running() && term->master_fd() < 0) {
          term->start();
        }
        if (interactive) {
          std::cout << "[command] " << command_bar_.status_line() << '\n';
        }
        command_mode = false;
        command_bar_.visible = false;
      } else if (command_mode) {
        if (line == "exit" || line == "cancel" || line.empty()) {
          command_mode = false;
          command_bar_.visible = false;
          if (interactive && running_) {
            std::cout << "diftray> " << std::flush;
          }
          continue;
        }
        command_bar_.dispatch(line);
        term = active_cell() ? active_cell()->nterm() : nullptr;
        printed_output = 0;
        if (term && !term->running() && term->master_fd() < 0) {
          term->start();
        }
        if (interactive) {
          std::cout << "[command] " << command_bar_.status_line() << '\n';
        }
        command_mode = false;
        command_bar_.visible = false;
      } else if (term) {
        term->feed_input(line + "\n");
      }
      if (interactive && running_) {
        std::cout << (command_mode ? "command: " : "diftray> ") << std::flush;
      }
    }
    if (!interactive && ready == 0) {
      running_ = false;
    }
  }
  stop();
  return 0;
}

void Compositor::stop() {
  running_ = false;
  if (terminal_source_) {
    wl_event_source_remove(terminal_source_);
    terminal_source_ = nullptr;
  }
  if (display_) {
    wl_display_terminate(display_.get());
  }
  for (auto &cell : cells_) {
    if (cell && cell->nterm()) {
      cell->nterm()->stop();
    }
  }
}

void Compositor::refresh_terminal_display() {
  if (!wayland_runtime_) {
    return;
  }
  auto *cell = active_cell();
  auto *term = cell ? cell->nterm() : nullptr;
  if (!term) {
    diftray_wayland_runtime_set_terminal_text(wayland_runtime_, "DiftrayWM\n");
    return;
  }
  std::string text;
  for (const auto &chunk : term->scrollback()) {
    text += chunk;
  }
  if (text.empty()) {
    text = "DiftrayWM\n";
  }
  diftray_wayland_runtime_set_terminal_text(wayland_runtime_, text.c_str());
}

int Compositor::terminal_fd_ready(int fd, uint32_t mask, void *data) {
  auto *compositor = static_cast<Compositor *>(data);
  if (!compositor || !(mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
    return 0;
  }
  auto *cell = compositor->active_cell();
  auto *term = cell ? cell->nterm() : nullptr;
  if (!term || term->master_fd() != fd) {
    return 0;
  }
  term->on_readable();
  compositor->refresh_terminal_display();
  return 0;
}

void Compositor::terminal_key_received(void *userdata, uint32_t keysym,
                                       uint32_t modifiers, uint32_t state) {
  auto *compositor = static_cast<Compositor *>(userdata);
  if (compositor) {
    compositor->handle_terminal_key(keysym, modifiers, state);
  }
}

void Compositor::handle_terminal_key(uint32_t keysym, uint32_t modifiers,
                                     uint32_t state) {
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    return;
  }
  auto *cell = active_cell();
  auto *term = cell ? cell->nterm() : nullptr;
  if (!term) {
    return;
  }
  std::string input;
  if (keysym >= 0x20 && keysym <= 0x7e) {
    char character = static_cast<char>(keysym);
    if ((modifiers & WLR_MODIFIER_CTRL) != 0) {
      const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
      if (lower >= 'a' && lower <= 'z') {
        input.push_back(static_cast<char>(lower - 'a' + 1));
      }
    } else {
      input.push_back(character);
    }
  } else {
    switch (keysym) {
    case 0xff08: // BackSpace
      input.push_back('\x7f');
      break;
    case 0xff09: // Tab
      input.push_back('\t');
      break;
    case 0xff0d: // Return
      input.push_back('\n');
      break;
    case 0xff1b: // Escape
      input.push_back('\x1b');
      break;
    case 0xffff: // Delete
      input = "\x1b[3~";
      break;
    default:
      break;
    }
  }
  if (!input.empty()) {
    term->feed_input(input);
  }
}

void Compositor::set_active_view(View *view) {
  active_view_ = view;
  command_context_.active_view = view;
  command_context_.active_cell = active_cell();
  command_context_.active_gcursor = active_gcursor();
  if (view && view->type() == ViewType::NCURSOR) {
    command_context_.ncursor_view = ncursor_view();
  }
}

NCursorView *Compositor::ncursor_view() const { return ncursor_view_.get(); }

Cell *Compositor::active_cell() const {
  return ncursor_view_ ? ncursor_view_->active_cell() : nullptr;
}

GCursorView *Compositor::active_gcursor() const {
  if (auto *cursor = dynamic_cast<GCursorView *>(active_view_)) {
    return cursor;
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
  if (!ncursor_view_) {
    return "spawn failed: no ncursor view";
  }
  cells_.push_back(std::make_unique<Cell>());
  auto *cell = cells_.back().get();
  if (!ncursor_view_->insert_cell(cell, above)) {
    cells_.pop_back();
    return "spawn failed: could not insert cell";
  }
  ncursor_view_->layout();
  rebuild_command_context();
  return std::string("spawned cell ") + cell->id();
}

std::string Compositor::erase_cell(Cell *cell) {
  if (!cell) {
    return "kill failed: no active cell";
  }
  if (cells_.size() <= 1) {
    return "kill failed: cannot remove the last cell";
  }
  if (ncursor_view_) {
    ncursor_view_->remove_cell(cell);
  }
  if (tcursor_view_) {
    tcursor_view_.reset();
    active_view_ = ncursor_view_.get();
  }
  const auto it = std::remove_if(cells_.begin(), cells_.end(),
                                 [cell](const std::unique_ptr<Cell> &owned) { return owned.get() == cell; });
  if (it != cells_.end()) {
    const auto id = cell->id();
    cells_.erase(it, cells_.end());
    rebuild_command_context();
    return "removed cell " + id;
  }
  return "kill failed: cell not owned";
}

std::string Compositor::kill_selected_cell() { return erase_cell(active_cell()); }

std::string Compositor::move_selected_cell(int delta) {
  if (!ncursor_view_) {
    return "move failed: no ncursor view";
  }
  if (ncursor_view_->move_selected_cell(delta)) {
    ncursor_view_->layout();
    rebuild_command_context();
    return "moved selected cell";
  }
  return "move failed";
}

std::string Compositor::toggle_cell_select_mode() {
  if (!ncursor_view_) {
    return "cell select unavailable";
  }
  ncursor_view_->set_cell_select_mode(!ncursor_view_->cell_select_mode());
  return ncursor_view_->cell_select_mode() ? "cell select enabled" : "cell select disabled";
}

std::string Compositor::focus_active_cell() {
  if (auto *cell = active_cell()) {
    if (auto *term = cell->nterm()) {
      term->on_readable();
    }
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
  rebuild_command_context();
  return "promoted cell " + cell->id();
}

std::string Compositor::restore_tcursor() {
  if (!tcursor_view_) {
    return "restore failed: no tcursor";
  }
  tcursor_view_.reset();
  active_view_ = ncursor_view_.get();
  rebuild_command_context();
  return "restored tcursor";
}

std::string Compositor::dock_cursor(const std::string &id) {
  if (!ncursor_view_) {
    return "dock failed: no ncursor view";
  }
  auto existing = std::find_if(gcursors_.begin(), gcursors_.end(), [&](const auto &cursor) {
    return cursor && cursor->word_id() == id;
  });
  if (existing == gcursors_.end()) {
    gcursors_.push_back(std::make_unique<GCursorView>(this, id));
    existing = std::prev(gcursors_.end());
  }
  ncursor_view_->cursor_area().dock(existing->get());
  rebuild_command_context();
  return "docked cursor " + (*existing)->word_id();
}

std::string Compositor::restore_cursor(const std::string &id) {
  if (!ncursor_view_) {
    return "restore failed: no ncursor view";
  }
  auto *cursor = ncursor_view_->cursor_area().find_by_id(id);
  if (!cursor) {
    return "restore failed: cursor not found";
  }
  ncursor_view_->cursor_area().restore(cursor);
  set_active_view(cursor);
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
  }
  return out.str();
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
  theme_engine_->swap_active(std::move(parsed));
  return "theme applied";
}

void WlDisplayDeleter::operator()(wl_display *ptr) const noexcept {
  if (ptr) {
    wl_display_destroy(ptr);
  }
}
void WlrBackendDeleter::operator()(wlr_backend *ptr) const noexcept {
  (void)ptr;
}
void WlrRendererDeleter::operator()(wlr_renderer *ptr) const noexcept {
  (void)ptr;
}
void WlrAllocatorDeleter::operator()(wlr_allocator *ptr) const noexcept {
  (void)ptr;
}
void WlrOutputLayoutDeleter::operator()(wlr_output_layout *ptr) const noexcept {
  (void)ptr;
}
void WlrSceneDeleter::operator()(wlr_scene *ptr) const noexcept {
  (void)ptr;
}
void WlrSeatDeleter::operator()(wlr_seat *ptr) const noexcept {
  (void)ptr;
}
