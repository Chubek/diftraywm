#include "command/CommandBar.hpp"

#include "command/CommandHandler.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>

namespace {
std::vector<std::string> tokenize(std::string_view input) {
  std::vector<std::string> tokens;
  std::string current;
  bool in_quotes = false;
  for (char ch : input) {
    if (ch == '"') {
      in_quotes = !in_quotes;
      continue;
    }
    if (!in_quotes && std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}
}

CommandBar::CommandBar() {
  register_builtin_handlers(*this);
}

void CommandBar::handle_key(unsigned int key) {
  if (key == '\n' || key == '\r') {
    dispatch(input_buffer);
    input_buffer.clear();
    return;
  }
  if (key == '\b' || key == 127) {
    if (!input_buffer.empty()) {
      input_buffer.pop_back();
    }
    return;
  }
  if (key < 128 && std::isprint(static_cast<unsigned char>(key))) {
    input_buffer.push_back(static_cast<char>(key));
  }
}

bool CommandBar::dispatch(std::string_view input) {
  const auto tokens = tokenize(input);
  if (tokens.empty()) {
    return false;
  }
  if (!context_) {
    set_status_line("command context unavailable");
    return false;
  }
  for (auto &handler : handlers_) {
    if (handler && handler->matches(tokens.front())) {
      const auto result = handler->execute(tokens, scope, *context_);
      if (!result.empty()) {
        set_status_line(result);
      }
      return true;
    }
  }
  set_status_line("unknown command: " + tokens.front());
  return false;
}

void CommandBar::register_handler(std::unique_ptr<CommandHandler> handler) {
  handlers_.push_back(std::move(handler));
}

void CommandBar::set_context(CommandContext *context) { context_ = context; }

const std::string &CommandBar::status_line() const { return status_line_; }
void CommandBar::set_status_line(std::string message) { status_line_ = std::move(message); }
