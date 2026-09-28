#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

struct ConfigKeysym {
  uint32_t value;
  bool operator==(const ConfigKeysym &) const = default;
};
struct ConfigKeycode {
  uint32_t value;
  bool operator==(const ConfigKeycode &) const = default;
};
struct ConfigCommands {
  std::vector<std::string> values;
  bool operator==(const ConfigCommands &) const = default;
};
using ConfigValue = std::variant<double, bool, std::string, ConfigKeysym,
                                 ConfigKeycode, ConfigCommands>;

struct ConfigBinding {
  std::variant<ConfigKeysym, ConfigKeycode> key;
  uint32_t modifiers = 0;
  bool global = true;
  std::vector<std::string> commands;
  bool matches(uint32_t keysym, uint32_t keycode, uint32_t mods) const;
};

// Pure, bounded configuration expressions. No filesystem, process or Lua APIs.
class ConfigProgram {
public:
  static std::shared_ptr<const ConfigProgram> compile(const std::string &source,
                                                      const std::string &name,
                                                      std::string &error);
  ~ConfigProgram();
  bool evaluate(const std::string &expression, ConfigValue &out,
                std::string &error) const;
  bool expand(const std::string &input, std::string &out,
              std::string &error) const;
  const std::vector<ConfigBinding> &bindings() const;
  const std::map<std::string, ConfigValue> &variables() const;
  static std::string describe(const ConfigValue &value);
  static bool commands(const ConfigValue &value, std::vector<std::string> &out,
                       std::string &error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  ConfigProgram();
};
