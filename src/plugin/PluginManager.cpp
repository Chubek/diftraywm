#include "plugin/PluginManager.hpp"

#include "DiftrayWM-Plugin.h"

#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>

namespace {
void plugin_log(const char *) {}
void plugin_status(const char *) {}
void plugin_register_command(const char *, diftraywm_command_callback_t,
                             diftraywm_command_scope_t) {}
void plugin_unregister_command(const char *) {}
void plugin_subscribe(void (*)(void *), void *) {}

const diftraywm_api_t api{
    plugin_log, plugin_status, plugin_register_command, plugin_unregister_command,
    plugin_subscribe, plugin_subscribe, plugin_subscribe};
}

PluginManager::~PluginManager() { unload_all(); }

bool PluginManager::discover() {
  const char *configured = std::getenv("DIFTRAYWM_PLUGIN_PATH");
  const std::filesystem::path directory =
      configured && *configured
          ? configured
          : (std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") /
             ".config/diftraywm/plugins");
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return true;
  }
  bool success = true;
  for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      return false;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".so") {
      success = load(entry.path().string()) && success;
    }
  }
  return success;
}

bool PluginManager::load(const std::string &path) {
  if (path.empty()) {
    return false;
  }
  if (std::find(loaded_.begin(), loaded_.end(), path) != loaded_.end()) {
    return true;
  }
  void *handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    return false;
  }
  auto init = reinterpret_cast<diftraywm_plugin_init_fn>(::dlsym(handle, "diftraywm_plugin_init"));
  auto version = reinterpret_cast<diftraywm_plugin_version_fn>(::dlsym(handle, "diftraywm_plugin_version"));
  if (!init || !version || init(&api) != 0) {
    ::dlclose(handle);
    return false;
  }
  handles_.push_back(handle);
  loaded_.push_back(path);
  return true;
}

void PluginManager::unload_all() {
  for (auto handle = handles_.rbegin(); handle != handles_.rend(); ++handle) {
    if (!*handle) {
      continue;
    }
    auto cleanup = reinterpret_cast<diftraywm_plugin_cleanup_fn>(::dlsym(*handle, "diftraywm_plugin_cleanup"));
    if (cleanup) {
      cleanup();
    }
    ::dlclose(*handle);
  }
  handles_.clear();
  loaded_.clear();
}

const std::vector<std::string> &PluginManager::loaded() const { return loaded_; }
