#include "lua/LuaEngine.hpp"

bool LuaEngine::init() {
  state_ = nullptr;
  return true;
}

void LuaEngine::scan_extensions() {}
