#pragma once

#include <string>
#include <string_view>
#include <vector>

class CommandContext;

enum class CommandScope {
  CELL,
  NCURSOR_GLOBAL,
};

class CommandHandler {
public:
  virtual ~CommandHandler() = default;
  virtual bool matches(std::string_view command) const = 0;
  virtual std::string execute(const std::vector<std::string> &tokens, CommandScope scope,
                              CommandContext &context) = 0;
};
