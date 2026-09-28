#pragma once
#include <memory>
#include <string>
#include <vector>
class CommandBar;

class LuaEngine {
public:
  explicit LuaEngine(CommandBar *bar = nullptr);
  ~LuaEngine();
  bool init();
  void scan_extensions();
  bool load_source(const std::string &name, const std::string &source);
  void notify(const std::string &event);
  void drain_commands();
  bool initialized() const;
  const std::vector<std::string> &extensions() const;
  const std::string &error() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
