#include "command/CommandBar.hpp"

#include "command/CommandHandler.hpp"
#include "input/TextInput.hpp"

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
    text_input::erase_last(input_buffer);
    return;
  }
  text_input::append(input_buffer, key);
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
  if (dispatch_depth_ >= 16) {
    set_status_line("command recursion limit exceeded"); return false;
  }
  std::string raw_input(input);
  struct DispatchGuard {
    unsigned &depth; CommandContext &context; std::string previous;
    ~DispatchGuard() { --depth; context.raw_input = std::move(previous); }
  } guard{dispatch_depth_, *context_, std::move(context_->raw_input)};
  ++dispatch_depth_;
  context_->raw_input = std::move(raw_input);
  for (auto &handler : handlers_) {
    if (handler && handler->matches(tokens.front())) {
      const auto result = handler->execute(tokens, scope, *context_);
      if (!result.empty() || tokens.front() == "config") {
        set_status_line(result);
      }
      return true;
    }
  }
  if (auto it = extensions_.find(tokens.front()); it != extensions_.end()) {
    auto command = it->second; // callbacks may unregister themselves
    if (command.required_scope && *command.required_scope != scope) {
      set_status_line("command is unavailable in this scope"); return false;
    }
    const auto result = command.callback(tokens, scope);
    if (!result.empty()) set_status_line(result);
    return true;
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

bool CommandBar::register_command(const std::string &name, const void *owner,
                                 ExtensionCallback callback, std::optional<CommandScope> required_scope) {
  if (name.empty() || !owner || !callback || name.find_first_of(" \t\r\n\"") != std::string::npos) return false;
  for (const auto &handler : handlers_) if (handler->matches(name)) return false;
  return extensions_.emplace(name, ExtensionCommand{owner, std::move(callback), required_scope}).second;
}
void CommandBar::unregister_command(const std::string &name, const void *owner) {
  const auto it = extensions_.find(name);
  if (it != extensions_.end() && it->second.owner == owner) extensions_.erase(it);
}
void CommandBar::unregister_owner(const void *owner) {
  std::erase_if(extensions_, [owner](const auto &entry) { return entry.second.owner == owner; });
}

std::optional<CommandScope> CommandBar::required_scope(const std::string &command) const {
  const auto tokens = tokenize(command);
  if (tokens.empty()) return std::nullopt;
  for (const auto &handler : handlers_)
    if (handler && handler->matches(tokens.front())) return std::nullopt;
  const auto it = extensions_.find(tokens.front());
  if (it == extensions_.end()) return std::nullopt;
  return it->second.required_scope;
}
