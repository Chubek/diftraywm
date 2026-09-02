#include "plugin/PluginManager.hpp"

bool PluginManager::discover() { return true; }
bool PluginManager::load(const std::string &path) {
  loaded_.push_back(path);
  return true;
}
void PluginManager::unload_all() { loaded_.clear(); }
