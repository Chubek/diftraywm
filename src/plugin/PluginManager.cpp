#include "plugin/PluginManager.hpp"
#include "command/CommandBar.hpp"
#include "DiftrayWM-Plugin.h"
#include <dynalo/dynalo.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <utility>

struct PluginManager::Plugin {
  explicit Plugin(const std::string &path) : library(path) {}
  dynalo::library library;
  diftraywm_plugin_cleanup_fn cleanup = nullptr;
  struct Hook { void (*callback)(void *); void *data; };
  std::map<std::string, std::vector<Hook>> hooks;
};
struct PluginManager::Api {
  static thread_local PluginManager *manager;
  static thread_local Plugin *plugin;
  struct Context {
    PluginManager *previous_manager = manager;
    Plugin *previous_plugin = plugin;
    Context(PluginManager *m, Plugin *p) { manager = m; plugin = p; }
    ~Context() { manager = previous_manager; plugin = previous_plugin; }
  };
  static void log(const char *text) { if (text) std::cerr << "plugin: " << text << '\n'; }
  static void status(const char *text) { if (manager && manager->bar_ && text) manager->bar_->set_status_line(text); }
  static void register_command(const char *name, diftraywm_command_callback_t callback, diftraywm_command_scope_t scope) {
    if (!manager || !manager->bar_ || !plugin || !name || !callback) return;
    if (scope != DIFTRAYWM_COMMAND_SCOPE_CELL && scope != DIFTRAYWM_COMMAND_SCOPE_NCURSOR_GLOBAL) return;
    auto *m = manager;
    auto *p = plugin;
    if (!m->bar_->register_command(name, p, [m, p, callback](const auto &tokens, CommandScope) {
      Context context(m, p);
      std::vector<const char *> args;
      for (const auto &token : tokens) args.push_back(token.c_str());
      try { callback(args.data(), static_cast<int>(args.size())); }
      catch (...) { return std::string("plugin command failed"); }
      return std::string{};
    }, scope == DIFTRAYWM_COMMAND_SCOPE_CELL ? CommandScope::CELL : CommandScope::NCURSOR_GLOBAL))
      m->error_ = "plugin command collision: " + std::string(name);
  }
  static void unregister_command(const char *name) {
    if (manager && manager->bar_ && plugin && name) manager->bar_->unregister_command(name, plugin);
  }
  static void subscribe(const char *event, void (*callback)(void *), void *data) {
    if (plugin && callback && plugin->hooks[event].size() < 256) plugin->hooks[event].push_back({callback, data});
  }
  static const diftraywm_api_t table;
};
thread_local PluginManager *PluginManager::Api::manager = nullptr;
thread_local PluginManager::Plugin *PluginManager::Api::plugin = nullptr;
const diftraywm_api_t PluginManager::Api::table{
  log, status, register_command, unregister_command,
  [](auto callback, auto data) { subscribe("frame", callback, data); },
  [](auto callback, auto data) { subscribe("view", callback, data); },
  [](auto callback, auto data) { subscribe("input", callback, data); }
};
PluginManager::PluginManager(CommandBar *bar) : bar_(bar) {}
PluginManager::~PluginManager() { unload_all(); }

bool PluginManager::discover() {
  const char *configured = std::getenv("DIFTRAYWM_PLUGIN_PATH");
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  const auto root = xdg && *xdg ? std::filesystem::path(xdg) :
      std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config";
  const auto directory = configured && *configured ? std::filesystem::path(configured) : root / "diftraywm/plugins";
  std::error_code ec;
  if (!std::filesystem::exists(directory, ec)) return !ec;
  std::vector<std::string> paths;
  for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && it->path().extension() == ".so") paths.push_back(it->path().string());
  if (ec) { error_ = ec.message(); return false; }
  std::sort(paths.begin(), paths.end());
  std::string errors;
  for (const auto &path : paths) {
    if (!load(path)) errors += path + ": " + error_ + "\n";
  }
  error_ = std::move(errors);
  return error_.empty();
}

bool PluginManager::load(const std::string &path) {
  if (notifying_) { error_ = "cannot load a plugin during an event"; return false; }
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(path, ec).string();
  if (ec) { error_ = ec.message(); return false; }
  if (std::find(loaded_.begin(), loaded_.end(), canonical) != loaded_.end()) return true;
  std::unique_ptr<Plugin> plugin;
  try {
    plugin = std::make_unique<Plugin>(canonical);
    const auto init = plugin->library.get_function<int(const diftraywm_api_t *)>("diftraywm_plugin_init");
    const auto version = plugin->library.get_function<const char *()>("diftraywm_plugin_version");
    plugin->cleanup = plugin->library.get_function<void()>("diftraywm_plugin_cleanup");
    const char *reported = version();
    if (!reported || !*reported) throw std::runtime_error("plugin version is empty");
    Api::Context context(this, plugin.get());
    error_.clear();
    if (init(&Api::table) != 0 || !error_.empty()) throw std::runtime_error(error_.empty() ? "plugin initialization failed" : error_);
    plugins_.push_back(std::move(plugin));
    loaded_.push_back(canonical);
    return true;
  } catch (const std::exception &exception) {
    error_ = exception.what();
    if (plugin) {
      Api::Context context(this, plugin.get());
      if (plugin->cleanup) { try { plugin->cleanup(); } catch (...) {} }
      if (bar_) bar_->unregister_owner(plugin.get());
    }
    return false;
  }
}
bool PluginManager::unload(const std::string &path) {
  if (notifying_) { error_ = "cannot unload a plugin during an event"; return false; }
  if (path.empty()) { error_ = "plugin path is empty"; return false; }
  std::error_code ec;
  // Resolve the same way load() does, so "unload ./plugin.so" finds the
  // canonical entry the manager recorded.
  const auto canonical = std::filesystem::canonical(path, ec).string();
  if (ec) { error_ = ec.message(); return false; }
  const auto it = std::find(loaded_.begin(), loaded_.end(), canonical);
  if (it == loaded_.end()) { error_ = "plugin is not loaded: " + canonical; return false; }
  const auto index = static_cast<std::size_t>(std::distance(loaded_.begin(), it));
  // plugins_ and loaded_ are appended in lockstep.
  if (index >= plugins_.size()) { error_ = "plugin table is inconsistent"; return false; }
  auto plugin = std::move(plugins_[index]);
  {
    Api::Context context(this, plugin.get());
    if (plugin->cleanup) { try { plugin->cleanup(); } catch (...) {} }
    if (bar_) bar_->unregister_owner(plugin.get());
  }
  plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
  loaded_.erase(it);
  error_.clear();
  return true;
}

void PluginManager::unload_all() {
  if (notifying_) return;
  for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
    Api::Context context(this, it->get());
    try { (*it)->cleanup(); } catch (...) {}
    if (bar_) bar_->unregister_owner(it->get());
  }
  plugins_.clear(); loaded_.clear();
}
void PluginManager::notify(const std::string &event) {
  if (notifying_) return;
  notifying_ = true;
  for (const auto &plugin : plugins_) {
    Api::Context context(this, plugin.get());
    const auto callbacks = plugin->hooks[event];
    for (const auto &hook : callbacks) {
      try { hook.callback(hook.data); }
      catch (...) { error_ = "plugin event failed: " + event; }
    }
  }
  notifying_ = false;
}
const std::vector<std::string> &PluginManager::loaded() const { return loaded_; }
