#pragma once
#include <memory>
#include <string>
#include <vector>
class CommandBar;

class PluginManager {
public:
  explicit PluginManager(CommandBar *bar = nullptr);
  ~PluginManager();
  bool discover();
  bool load(const std::string &path);
  // Unloads a single plugin, running its cleanup and dropping the commands it
  // registered. Unknown or already-unloaded paths fail.
  bool unload(const std::string &path);
  void unload_all();
  void notify(const std::string &event);
  const std::vector<std::string> &loaded() const;
  const std::string &error() const { return error_; }
private:
  struct Plugin;
  struct Api;
  CommandBar *bar_;
  std::vector<std::string> loaded_;
  std::vector<std::unique_ptr<Plugin>> plugins_;
  std::string error_;
  bool notifying_ = false;
};
