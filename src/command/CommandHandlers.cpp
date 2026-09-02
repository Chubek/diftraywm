#include "command/CommandBar.hpp"

#include "compositor/Compositor.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"
#include "views/GCursorView.hpp"
#include "views/TCursorView.hpp"

#include <memory>
#include <sstream>

std::string join_tokens(const std::vector<std::string> &tokens, std::size_t start) {
  std::ostringstream out;
  for (std::size_t i = start; i < tokens.size(); ++i) {
    if (i > start) {
      out << ' ';
    }
    out << tokens[i];
  }
  return out.str();
}

class SpawnHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "spawn"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "spawn failed: compositor unavailable";
    }
    if (tokens.size() < 2) {
      return "spawn requires a placement";
    }
    if (tokens[1] == "above") {
      return context.compositor->spawn_cell(true);
    }
    if (tokens[1] == "below") {
      return context.compositor->spawn_cell(false);
    }
    return "spawn supports: above, below";
  }
};

class KillHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "kill"; }

  std::string execute(const std::vector<std::string> &, CommandScope,
                      CommandContext &context) override {
    return context.compositor ? context.compositor->kill_selected_cell()
                              : "kill failed: compositor unavailable";
  }
};

class MoveHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "move"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "move failed: compositor unavailable";
    }
    if (tokens.size() < 2) {
      return "move requires a direction";
    }
    if (tokens[1] == "up") {
      return context.compositor->move_selected_cell(-1);
    }
    if (tokens[1] == "down") {
      return context.compositor->move_selected_cell(1);
    }
    return "move supports: up, down";
  }
};

class CellHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "cell"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "cell command unavailable";
    }
    if (tokens.size() < 2) {
      return "cell requires a subcommand";
    }
    if (tokens[1] == "select") {
      return context.compositor->toggle_cell_select_mode();
    }
    if (tokens[1] == "focus") {
      return context.compositor->focus_active_cell();
    }
    if (tokens[1] == "promote") {
      return context.compositor->promote_active_cell_to_tcursor();
    }
    if (tokens[1] == "restore") {
      return context.compositor->restore_tcursor();
    }
    if (tokens[1] == "spawn" && tokens.size() >= 3) {
      if (tokens[2] == "above") {
        return context.compositor->spawn_cell(true);
      }
      if (tokens[2] == "below") {
        return context.compositor->spawn_cell(false);
      }
    }
    return "cell supports: select, focus, promote, restore, spawn";
  }
};

class CursorHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "cursor"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "cursor command unavailable";
    }
    if (tokens.size() < 2) {
      return "cursor requires a subcommand";
    }
    if (tokens[1] == "dock" && tokens.size() >= 3) {
      return context.compositor->dock_cursor(tokens[2]);
    }
    if (tokens[1] == "restore" && tokens.size() >= 3) {
      return context.compositor->restore_cursor(tokens[2]);
    }
    if (tokens[1] == "list" && tokens.size() >= 3 && tokens[2] == "ids") {
      return context.compositor->list_cursor_ids();
    }
    return "cursor supports: dock, restore, list ids";
  }
};

class SetHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "set"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope scope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "set command unavailable";
    }
    if (tokens.size() < 3) {
      return "set requires a key and value";
    }
    if (tokens[1] == "shell") {
      return context.compositor->set_shell_override(join_tokens(tokens, 2), scope);
    }
    if (tokens[1] == "theme") {
      return context.compositor->apply_theme_css(join_tokens(tokens, 2));
    }
    return "set supports: shell, theme";
  }
};

class ThemeHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "theme"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "theme command unavailable";
    }
    if (tokens.size() < 3 || tokens[1] != "load") {
      return "theme supports: load <css>";
    }
    return context.compositor->apply_theme_css(join_tokens(tokens, 2));
  }
};

void register_builtin_handlers(CommandBar &bar) {
  bar.register_handler(std::make_unique<SpawnHandler>());
  bar.register_handler(std::make_unique<KillHandler>());
  bar.register_handler(std::make_unique<MoveHandler>());
  bar.register_handler(std::make_unique<CellHandler>());
  bar.register_handler(std::make_unique<CursorHandler>());
  bar.register_handler(std::make_unique<SetHandler>());
  bar.register_handler(std::make_unique<ThemeHandler>());
}
