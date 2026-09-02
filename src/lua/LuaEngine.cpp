#include "lua/LuaEngine.hpp"

#include <cstdlib>
#include <filesystem>

bool LuaEngine::init() {
  state_ = this;
  initialized_ = true;
  return true;
}

void LuaEngine::scan_extensions() {
  extensions_.clear();
  const char *home = std::getenv("HOME");
  const std::filesystem::path directory =
      std::filesystem::path(home && *home ? home : ".") / ".config/diftraywm/extensions";
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return;
  }
  for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      break;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".lua") {
      extensions_.push_back(entry.path().string());
    }
  }
}

bool LuaEngine::initialized() const { return initialized_; }
const std::vector<std::string> &LuaEngine::extensions() const { return extensions_; }
