#include "command/CommandBar.hpp"

#include "compositor/Compositor.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"
#include "views/GCursorView.hpp"
#include "views/TCursorView.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <exception>

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
    if (tokens[1] == "left") {
      return context.compositor->cycle_tab(-1);
    }
    if (tokens[1] == "right") {
      return context.compositor->cycle_tab(1);
    }
    return "move supports: up, down, left, right";
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
    if (tokens[1] == "assign" && tokens.size() >= 4) {
      int slot = -1;
      const auto &key = tokens[3];
      if (key == "F1" || key == "f1" || key == "1") slot = 0;
      else if (key == "F2" || key == "f2" || key == "2") slot = 1;
      else if (key == "F3" || key == "f3" || key == "3") slot = 2;
      else if (key == "F4" || key == "f4" || key == "4") slot = 3;
      return context.compositor->assign_cursor_slot(tokens[2], slot);
    }
    return "cursor supports: dock, restore, list ids, assign";
  }
};

class CursorsHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "cursors"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "cursors command unavailable";
    }
    if (tokens.size() >= 3 && tokens[1] == "view" && tokens[2] == "docked") {
      return context.compositor->list_docked_cursors();
    }
    return "cursors supports: view docked";
  }
};

class LaunchHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "launch"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "launch unavailable";
    }
    return context.compositor->launch_program(join_tokens(tokens, 1));
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
      return "theme supports: load <path>";
    }
    return context.compositor->load_theme_file(join_tokens(tokens, 2));
  }
};

class NoteletHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "notelet"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "notelets unavailable";
    if (tokens.size() == 2 && tokens[1] == "list")
      return context.compositor->list_notelets();
    if (tokens.size() == 2 && tokens[1] == "close")
      return context.compositor->close_notelet();
    if (tokens.size() == 3 && tokens[1] == "open")
      return context.compositor->open_notelet(tokens[2]);
    return "notelet supports: list, open <name>, close";
  }
};

class HelpHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override {
    return command == "help" || command == "h";
  }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "help unavailable";
    if (tokens.size() == 1) return context.compositor->open_help_page("help-index");
    if (tokens[1] == "find") {
      if (tokens.size() < 3) return "help find requires a regex pattern";
      return context.compositor->find_help(join_tokens(tokens, 2));
    }
    if (tokens[1] == "bookmark-set") {
      return tokens.size() == 3 ? context.compositor->set_help_bookmark(tokens[2])
                                : "help bookmark-set requires a name";
    }
    if (tokens[1] == "bookmark-open") {
      return tokens.size() == 3 ? context.compositor->open_help_bookmark(tokens[2])
                                : "help bookmark-open requires a name";
    }
    return context.compositor->open_help_page(tokens[1]);
  }
};

class TabHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "tab"; }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "tab command unavailable";
    }
    if (tokens.size() < 2) {
      return "tab requires next or prev";
    }
    if (tokens[1] == "next" || tokens[1] == "right") {
      return context.compositor->cycle_tab(1);
    }
    if (tokens[1] == "prev" || tokens[1] == "previous" || tokens[1] == "left") {
      return context.compositor->cycle_tab(-1);
    }
    return "tab supports: next, prev";
  }
};

class WorkspaceHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override {
    return command == "workspace";
  }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "workspace command unavailable";
    }
    if (tokens.size() < 2) {
      return "workspace " + std::to_string(context.compositor->current_workspace());
    }
    try {
      const int number = std::stoi(tokens[1]);
      return context.compositor->switch_workspace(number);
    } catch (const std::exception &) {
      return "workspace requires a number 1-10";
    }
  }
};

void register_builtin_handlers(CommandBar &bar) {
  bar.register_handler(std::make_unique<SpawnHandler>());
  bar.register_handler(std::make_unique<KillHandler>());
  bar.register_handler(std::make_unique<MoveHandler>());
  bar.register_handler(std::make_unique<CellHandler>());
  bar.register_handler(std::make_unique<CursorHandler>());
  bar.register_handler(std::make_unique<CursorsHandler>());
  bar.register_handler(std::make_unique<LaunchHandler>());
  bar.register_handler(std::make_unique<SetHandler>());
  bar.register_handler(std::make_unique<ThemeHandler>());
  bar.register_handler(std::make_unique<NoteletHandler>());
  bar.register_handler(std::make_unique<HelpHandler>());
  bar.register_handler(std::make_unique<TabHandler>());
  bar.register_handler(std::make_unique<WorkspaceHandler>());
}
