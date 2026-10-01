// Session control surface for diftrayctl.
//
// Every entry point here is reachable from the Command Bar (so it is
// scriptable from extensions and plugins) and from the control socket (so it is
// scriptable from a shell). Both paths funnel through the same public methods
// and therefore share validation, scoping and status reporting.
#include "compositor/Compositor.hpp"
#include "compositor/ControlServer.hpp"
#include "compositor/WaylandRuntime.h"
#include "ctl/ControlSocketPath.hpp"

#include "nterm/NTerm.hpp"
#include "plugin/PluginManager.hpp"
#include "lua/LuaEngine.hpp"
#include "config/Config.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"
#include "views/GCursorView.hpp"

#include <termlib.h>
#include <wayland-server-core.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
// Bound every diftrayctl-triggered file read; the Lua engine, PluginManager
// and Termscript all have their own limits, this keeps the error readable.
constexpr std::size_t kMaxControlFileBytes = 1024 * 1024;
constexpr std::size_t kMaxScriptOutputBytes = 64 * 1024;

std::string read_bounded_file(const std::string &path, std::string &error) {
  error.clear();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    error = ec ? ec.message() : "not a regular file";
    return {};
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = ec.message();
    return {};
  }
  if (size > kMaxControlFileBytes) {
    error = "file exceeds 1 MiB";
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open file";
    return {};
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    input.read(content.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
      error = "short read";
      return {};
    }
  }
  if (content.find('\0') != std::string::npos) {
    error = "file contains NUL";
    return {};
  }
  return content;
}

const char *editor_command() {
  if (const char *visual = std::getenv("VISUAL"); visual && *visual) return visual;
  if (const char *editor = std::getenv("EDITOR"); editor && *editor) return editor;
  return "vi";
}

// Caps are rendered as a dimmer bracket so the taskbar reads as a taskbar and
// not as a list of table names.
std::string task_entry(const std::string &label, bool active, std::size_t cells) {
  std::string entry = active ? "[" + label : " " + label;
  if (cells > 0) {
    entry += ':';
    entry += std::to_string(cells);
  }
  if (!active) {
    entry.push_back(']');
  }
  return entry;
}
}

const std::string &Compositor::control_socket() const {
  static const std::string empty;
  return control_server_ ? control_server_->path() : empty;
}

// Single place that projects the configuration onto the runtime's style
// struct, so init, theme changes and config reload cannot drift apart.
diftray_wayland_style Compositor::wayland_style() const {
  return diftray_wayland_style{
      config_.border_size,
      config_.command_bar_height,
      config_.status_bar_height,
      config_.launcher_bar_height,
      {config_.border_color[0], config_.border_color[1], config_.border_color[2],
       config_.border_color[3]},
      {config_.background_color[0], config_.background_color[1],
       config_.background_color[2], config_.background_color[3]},
      {config_.command_bar_color[0], config_.command_bar_color[1],
       config_.command_bar_color[2], config_.command_bar_color[3]},
      {config_.launcher_bar_color[0], config_.launcher_bar_color[1],
       config_.launcher_bar_color[2], config_.launcher_bar_color[3]},
      {1.0f, 0.72f, 0.18f, 1.0f}};
}

bool Compositor::install_control_socket() {
  if (!display_) return false;
  control_server_ = std::make_unique<ControlServer>();
  if (!control_server_->start(display_.get(), diftray::control_socket_path(),
                              [this](const std::string &command) {
                                return run_control_command(command);
                              })) {
    std::cerr << "diftrayctl unavailable: " << control_server_->error() << '\n';
    control_server_.reset();
    return false;
  }
  return true;
}

void Compositor::remove_control_socket() {
  if (control_server_) {
    control_server_->stop();
    control_server_.reset();
  }
}

std::pair<bool, std::string> Compositor::run_control_command(const std::string &command) {
  if (command.empty()) {
    return {false, "empty command"};
  }
  // diftrayctl is an out-of-band control path, so it does not inherit whatever
  // scope the on-screen Command Bar was last left at. Builtin commands take
  // the NCursor-global scope; a plugin or extension command that declared a
  // cell scope still runs, against the active cell.
  const CommandScope saved_scope = command_bar_.scope;
  const auto required = command_bar_.required_scope(command);
  command_bar_.scope = (required && *required == CommandScope::CELL)
                           ? CommandScope::CELL
                           : CommandScope::NCURSOR_GLOBAL;
  const bool accepted = command_bar_.dispatch(command);
  std::string result = command_bar_.status_line();
  command_bar_.scope = saved_scope;
  status_line_ = result;
  // Extension queues and pending repaints still need to run, and they are
  // normally drained by the frame handler; a one-shot CLI command has no frame
  // to wait for.
  if (lua_engine_) lua_engine_->drain_commands();
  // A teardown command is on its way out; repainting would only race the exit.
  if (!exit_requested_) relayout();
  return {accepted, std::move(result)};
}

void Compositor::set_launcher_locked(bool locked) {
  launcher_locked_ = locked;
  // Persist the new default so a restart keeps the user's choice.
  config_.launcher_locked = locked;
  refresh_launcher_bar();
  relayout();
}

std::string Compositor::toggle_launcher_lock() {
  set_launcher_locked(!launcher_locked_);
  return launcher_locked_ ? "launch bar locked on top" : "launch bar unlocked";
}

std::string Compositor::launcher_status() const {
  std::ostringstream out;
  out << "launch bar: " << (launcher_locked_ ? "locked" : "unlocked") << '\n';
  out << "anchor: top" << '\n';
  out << "height: " << config_.launcher_bar_height << '\n';
  out << "visible: " << ((launcher_locked_ || launcher_mode_) ? "yes" : "no") << '\n';
  return out.str();
}

std::string Compositor::launcher_tasks() const {
  std::ostringstream out;
  const auto tabs = ncursors_on_workspace(current_workspace_);
  for (const auto *view : tabs) {
    if (!view) {
      continue;
    }
    std::size_t cells = 0;
    for (const auto &stack : view->cell_stacks()) cells += stack.cells.size();
    if (out.tellp() > 0) {
      out << ' ';
    }
    out << task_entry(view->id(), view == active_ncursor_, cells);
  }
  for (const auto *cursor : live_gcursors_on_workspace(current_workspace_)) {
    if (!cursor) {
      continue;
    }
    if (out.tellp() > 0) {
      out << ' ';
    }
    out << task_entry(cursor->word_id(), cursor == active_gcursor(), 0);
  }
  return out.str();
}

void Compositor::refresh_launcher_bar() {
  if (!wayland_runtime_) {
    return;
  }
  // The launcher is a top-anchored taskbar: it is on screen whenever it is
  // locked, and otherwise only while the launcher input has focus.
  const bool visible = launcher_locked_ || launcher_mode_;
  if (!visible) {
    diftray_wayland_runtime_set_launcher_bar(wayland_runtime_, false, "");
    return;
  }
  std::string text;
  if (launcher_mode_) {
    text = "launch> " + command_bar_.input_buffer;
  }
  const std::string tasks = launcher_tasks();
  if (!tasks.empty()) {
    if (!text.empty()) {
      text += "  |  ";
    }
    text += tasks;
  }
  diftray_wayland_runtime_set_launcher_bar(wayland_runtime_, true, text.c_str());
}

std::string Compositor::request_exit() {
  // Only ask the loop to stop. This runs inside the control-socket callback and
  // the reply is written after dispatch returns, so tearing anything down here
  // would cut diftrayctl off before it reads the answer. wl_display_terminate
  // breaks the loop once the current callback (including that write) finishes.
  exit_requested_ = true;
  if (display_) wl_display_terminate(display_.get());
  return "exiting session";
}

std::string Compositor::request_restart() {
  restart_requested_ = true;
  exit_requested_ = true;
  if (display_) wl_display_terminate(display_.get());
  return "restarting session";
}

std::string Compositor::session_status() const {
  std::ostringstream out;
  std::size_t cells = 0;
  for (const auto &view : ncursor_views_) {
    if (!view) continue;
    for (const auto &stack : view->cell_stacks()) cells += stack.cells.size();
  }
  std::size_t live_cursors = 0;
  for (const auto &cursor : gcursors_)
    if (cursor && !cursor->docked()) ++live_cursors;
  out << "DiftrayWM 0.1\n";
  out << "workspace: " << current_workspace_ << "\n";
  out << "output: " << active_output_ << "\n";
  out << "ncursors: " << ncursor_views_.size() << "\n";
  out << "cells: " << cells << "\n";
  out << "gcursors: " << live_cursors << " live, " << gcursors_.size() << " total\n";
  out << "plugins: " << (plugin_manager_ ? plugin_manager_->loaded().size() : 0) << "\n";
  out << "extensions: " << (lua_engine_ ? lua_engine_->extensions().size() : 0) << "\n";
  out << "config: " << config_file() << "\n";
  out << "control socket: " << (control_server_ ? control_server_->path() : "(none)") << "\n";
  out << "font: " << config_.font << ' ' << config_.font_size << "\n";
  out << "theme: " << (config_.theme.empty() ? "(none)" : config_.theme) << "\n";
  if (active_cell()) {
    out << "active cell: " << active_cell()->id() << "\n";
  }
  return out.str();
}

std::string Compositor::load_plugin(const std::string &path) {
  if (!plugin_manager_) return "plugin system unavailable";
  if (path.empty()) return "plugin load requires a shared library path";
  if (!plugin_manager_->load(path)) {
    return "plugin load failed: " + plugin_manager_->error();
  }
  return "loaded plugin " + path;
}

std::string Compositor::unload_plugin(const std::string &path) {
  if (!plugin_manager_) return "plugin system unavailable";
  if (path.empty()) return "plugin unload requires a shared library path";
  if (!plugin_manager_->unload(path)) {
    return "plugin unload failed: " + plugin_manager_->error();
  }
  return "unloaded plugin " + path;
}

std::string Compositor::list_plugins() const {
  if (!plugin_manager_ || plugin_manager_->loaded().empty()) return "no plugins loaded";
  std::ostringstream out;
  for (const auto &path : plugin_manager_->loaded()) {
    if (out.tellp() > 0) out << '\n';
    out << path;
  }
  return out.str();
}

std::string Compositor::exec_extension(const std::string &path) {
  if (!lua_engine_ || !lua_engine_->initialized()) return "extension system unavailable";
  if (path.empty()) return "extension exec requires a path";
  std::string error;
  const std::string source = read_bounded_file(path, error);
  if (!error.empty()) return "cannot read extension " + path + ": " + error;
  if (!lua_engine_->load_source(path, source)) {
    return "extension failed: " + lua_engine_->error();
  }
  return "loaded extension " + path;
}

std::string Compositor::list_extensions() const {
  if (!lua_engine_ || lua_engine_->extensions().empty()) return "no extensions loaded";
  std::ostringstream out;
  for (const auto &name : lua_engine_->extensions()) {
    if (out.tellp() > 0) out << '\n';
    out << name;
  }
  return out.str();
}

std::string Compositor::open_config() {
  if (config_path_.empty()) {
    return "no configuration file in use";
  }
  std::error_code ec;
  if (!std::filesystem::exists(config_path_, ec)) {
    return "configuration file is missing: " + config_path_;
  }
  // Shell-quote the path: it is discovered from the environment and may sit
  // under a directory that contains spaces.
  std::string quoted = "'";
  for (const char ch : config_path_) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += "'";
  const std::string editor = editor_command();
  const std::string command = editor + " " + quoted;
  launch_program(command);
  return "opened " + config_path_ + " with " + editor;
}

std::string Compositor::reload_config() {
  if (config_path_.empty()) {
    return "no configuration file in use";
  }
  CompositorConfig next;
  std::string error;
  if (!load_compositor_config(config_path_, next, error)) {
    // Atomic: the running configuration is untouched when the new file is bad.
    return "reload failed: " + error;
  }
  const std::filesystem::path base =
      std::filesystem::path(config_path_).parent_path();
  if (!next.theme.empty() && std::filesystem::path(next.theme).is_relative()) {
    next.theme = (base / next.theme).lexically_normal().string();
  }
  if (!next.help_path.empty() && std::filesystem::path(next.help_path).is_relative()) {
    next.help_path = (base / next.help_path).lexically_normal().string();
  }
  const bool font_changed =
      next.font != config_.font || next.font_size != config_.font_size;
  config_ = std::move(next);
  if (!config_.word_pool.empty()) {
    setenv("DIFTRAYWM_WORD_POOL", config_.word_pool.c_str(), 0);
  }
  help_pager_.set_search_path(config_.help_path.empty()
                                  ? (base / "help").string() + ":" + DIFTRAY_HELP_DEFAULT_PATH
                                  : config_.help_path);
  if (font_changed && !nterm_renderer_.init(config_.font, config_.font_size)) {
    return "reloaded configuration but the font could not be applied";
  }
  if (!config_.theme.empty()) {
    const std::string result = load_theme_file(config_.theme);
    if (result != "theme applied") {
      return "reloaded configuration but the theme failed: " + result;
    }
  }
  if (display_) {
    const diftray_wayland_style style = wayland_style();
    diftray_wayland_runtime_set_style(wayland_runtime_, &style);
  }
  // The keymap follows the config, since the config only names the file. A
  // broken keymap is reported without failing the reload: the keys the user
  // had a moment ago are better than none, and `keymap show` explains why.
  std::string keymap_status;
  if (!config_.keymap.empty() && !load_keymap(keymap_status)) {
    return "reloaded " + config_path_ + " but the keymap failed: " + keymap_status;
  }
  relayout();
  return "reloaded " + config_path_;
}

std::string Compositor::source_script(const std::string &path) {
  if (path.empty()) return "script source requires a path";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return ec ? "cannot read script: " + ec.message()
              : "script source requires a regular file: " + path;
  }
  // Termscript runs on the compositor thread, so cap the work: this is an
  // interactive control path, not a batch runner. Long jobs belong in a notelet,
  // which already runs Termscript in a dedicated worker.
  DT_Error dt_error{};
  DT_TermVM *vm = dt_termscript_create(&dt_error);
  if (!vm) {
    return "cannot create Termscript VM: " + std::string(dt_error.message);
  }
  char *output = nullptr;
  const DT_Status status = dt_termscript_run_file(vm, path.c_str(), &output, &dt_error);
  std::string result;
  if (status != DT_OK) {
    result = "script failed: " + std::string(dt_error.message);
  } else if (output && std::char_traits<char>::length(output) <= kMaxScriptOutputBytes) {
    result = output;
  } else if (output) {
    result = "script output exceeds 64 KiB";
  } else {
    result = "sourced " + path;
  }
  std::free(output);
  dt_termscript_free(vm);
  return result;
}
