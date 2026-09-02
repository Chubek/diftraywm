#pragma once

#include <string>
#include <vector>

class LuaEngine {
public:
  bool init();
  void scan_extensions();
  bool initialized() const;
  const std::vector<std::string> &extensions() const;

private:
  void *state_ = nullptr;
  bool initialized_ = false;
  std::vector<std::string> extensions_;
};
