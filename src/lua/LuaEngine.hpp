#pragma once

#include <string>

class LuaEngine {
public:
  bool init();
  void scan_extensions();

private:
  void *state_ = nullptr;
};
