#pragma once

#include <string>
#include <vector>

class PluginManager {
public:
  bool discover();
  bool load(const std::string &path);
  void unload_all();

private:
  std::vector<std::string> loaded_;
};
