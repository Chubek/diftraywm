#pragma once

#include "command/CommandContext.hpp"
#include "command/CommandHandler.hpp"

#include <memory>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CommandBar {
public:
  CommandBar();
  void handle_key(unsigned int key);
  bool dispatch(std::string_view input);
  void register_handler(std::unique_ptr<CommandHandler> handler);
  void set_context(CommandContext *context);
  const std::string &status_line() const;
  void set_status_line(std::string message);

  using ExtensionCallback = std::function<std::string(const std::vector<std::string> &, CommandScope)>;
  bool register_command(const std::string &name, const void *owner, ExtensionCallback callback,
                        std::optional<CommandScope> required_scope = {});
  void unregister_command(const std::string &name, const void *owner);
  void unregister_owner(const void *owner);

  std::string input_buffer;
  bool visible = false;
  CommandScope scope = CommandScope::CELL;

private:
  struct ExtensionCommand {
    const void *owner;
    ExtensionCallback callback;
    std::optional<CommandScope> required_scope;
  };
  std::map<std::string, ExtensionCommand> extensions_;
  std::vector<std::unique_ptr<CommandHandler>> handlers_;
  CommandContext *context_ = nullptr;
  std::string status_line_;
};

void register_builtin_handlers(CommandBar &bar);
