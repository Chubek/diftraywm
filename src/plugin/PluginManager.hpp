#pragma once

#include <string>
#include <vector>

class PluginManager {
public:
  ~PluginManager();
  bool discover();
  bool load(const std::string &path);
  void unload_all();
  const std::vector<std::string> &loaded() const;

private:
  std::vector<std::string> loaded_;
  std::vector<void *> handles_;
};
