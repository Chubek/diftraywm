#include "command/CommandBar.hpp"

#include "compositor/Compositor.hpp"
#include "nterm/NTerm.hpp"
#include "keymap/Keymap.hpp"
#include "theme/ThemeEngine.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"
#include "views/GCursorView.hpp"
#include "views/TCursorView.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <algorithm>
#include <exception>
#include <vector>

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
    if (tokens.size() == 4 && tokens[1] == "move")
      return context.compositor->move_cell(tokens[2], tokens[3]);
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
    if (tokens.size() == 5 && tokens[1] == "move")
      return context.compositor->move_cursor(tokens[2], tokens[3], tokens[4]);
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
    if (tokens.size() >= 2 && tokens[1] == "show") {
      if (!context.theme_engine) {
        return "theme command unavailable";
      }
      const auto &properties = context.theme_engine->active().tokens;
      std::vector<std::string> names;
      for (const auto &[key, value] : properties) {
        if (key != "css") {
          names.push_back(key);
        }
      }
      if (names.empty()) {
        return "no theme properties";
      }
      std::sort(names.begin(), names.end());
      std::ostringstream out;
      for (const auto &name : names) {
        if (out.tellp() > 0) {
          out << '\n';
        }
        out << name << ": " << properties.at(name);
      }
      return out.str();
    }
    if (tokens.size() < 3 || tokens[1] != "load") {
      return "theme supports: load <path>, show";
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
    if (tokens.size() == 2 && tokens[1] == "refresh")
      return context.compositor->refresh_notelet();
    if (tokens.size() == 2 && tokens[1] == "close")
      return context.compositor->close_notelet();
    if (tokens.size() == 3 && tokens[1] == "open")
      return context.compositor->open_notelet(tokens[2]);
    return "notelet supports: list, open <name>, refresh, close";
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

class MuxHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override {
    return command == "mux";
  }

  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) {
      return "mux command unavailable";
    }
    if (tokens.size() < 2) {
      return "mux supports: split horizontal|vertical, focus next|prev|up|down|left|right, kill, zoom, list";
    }
    if (tokens[1] == "split") {
      if (tokens.size() != 3) {
        return "mux split requires horizontal or vertical";
      }
      if (tokens[2] == "horizontal" || tokens[2] == "h") {
        return context.compositor->mux_split(false);
      }
      if (tokens[2] == "vertical" || tokens[2] == "v") {
        return context.compositor->mux_split(true);
      }
      return "mux split requires horizontal or vertical";
    }
    if (tokens[1] == "next" || tokens[1] == "prev" ||
        tokens[1] == "previous" || tokens[1] == "up" ||
        tokens[1] == "down" || tokens[1] == "left" ||
        tokens[1] == "right") {
      if (tokens.size() != 2) {
        return "mux " + tokens[1] + " takes no arguments";
      }
      return context.compositor->mux_focus(tokens[1] == "previous" ? "prev"
                                                                   : tokens[1]);
    }
    if (tokens[1] == "focus") {
      if (tokens.size() != 3) {
        return "mux focus requires next, prev, up, down, left, or right";
      }
      return context.compositor->mux_focus(tokens[2]);
    }
    if (tokens[1] == "kill" || tokens[1] == "zoom" || tokens[1] == "list") {
      if (tokens.size() != 2) {
        return "mux " + tokens[1] + " takes no arguments";
      }
      if (tokens[1] == "kill") return context.compositor->mux_kill();
      if (tokens[1] == "zoom") return context.compositor->mux_zoom();
      return context.compositor->mux_list();
    }
    return "mux supports: split horizontal|vertical, focus next|prev|up|down|left|right, kill, zoom, list";
  }
};

class WorkspaceCommandHandler final : public CommandHandler {
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

class NCursorHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "ncursor"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "ncursors unavailable";
    if (tokens.size() == 2 && tokens[1] == "list") return context.compositor->list_views();
    if (tokens.size() == 2 && tokens[1] == "new") return context.compositor->spawn_ncursor();
    return "ncursor supports: list, new";
  }
};

class OutputHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "output"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "outputs unavailable";
    if (tokens.size() == 2 && tokens[1] == "list") return context.compositor->list_outputs();
    if (tokens.size() == 3 && tokens[1] == "focus") return context.compositor->focus_output(tokens[2]);
    if (tokens.size() == 3 && tokens[1] == "move") return context.compositor->move_to_output(tokens[2]);
    if (tokens.size() == 4 && (tokens[1] == "rotate" || tokens[1] == "scale" ||
        (tokens[1] == "position" && tokens[3] == "auto")))
      return context.compositor->configure_output(tokens[2], tokens[1], tokens[3]);
    if (tokens.size() == 5 && tokens[1] == "position")
      return context.compositor->configure_output(tokens[2], tokens[1], tokens[3], tokens[4]);
    return "output supports: list, focus <name|next|prev>, move <name>, rotate <name> <0|90|180|270>, scale <name> <0.5..4>, position <name> <x y|auto>";
  }
};


class PluginHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "plugin"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "plugins unavailable";
    if (tokens.size() < 2) {
      return "plugin supports: load <path>, unload <path>, list";
    }
    if (tokens[1] == "load") {
      if (tokens.size() < 3) return "plugin load requires a shared library path";
      return context.compositor->load_plugin(join_tokens(tokens, 2));
    }
    if (tokens[1] == "unload") {
      if (tokens.size() < 3) return "plugin unload requires a shared library path";
      return context.compositor->unload_plugin(join_tokens(tokens, 2));
    }
    if (tokens[1] == "list") return context.compositor->list_plugins();
    return "plugin supports: load <path>, unload <path>, list";
  }
};

class ExtensionHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "extension"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "extensions unavailable";
    if (tokens.size() < 2) {
      return "extension supports: exec <path>, list";
    }
    if (tokens[1] == "exec") {
      if (tokens.size() < 3) return "extension exec requires a path";
      return context.compositor->exec_extension(join_tokens(tokens, 2));
    }
    if (tokens[1] == "list") return context.compositor->list_extensions();
    return "extension supports: exec <path>, list";
  }
};

class ScriptHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "script"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "scripts unavailable";
    if (tokens.size() < 2) {
      return "script supports: source <path.tsc>";
    }
    if (tokens[1] == "source") {
      if (tokens.size() < 3) return "script source requires a Termscript path";
      return context.compositor->source_script(join_tokens(tokens, 2));
    }
    return "script supports: source <path.tsc>";
  }
};

class TerminalHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "terminal"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (tokens.size() < 3 || tokens[1] != "script")
      return "terminal supports: script <path.tsc>";
    if (!context.active_cell || !context.active_cell->nterm())
      return "terminal script requires an active cell";
    return context.active_cell->nterm()->run_script(join_tokens(tokens, 2));
  }
};

class LauncherHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "launcher"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "launcher unavailable";
    if (tokens.size() < 2) {
      return "launcher supports: lock, unlock, toggle, status";
    }
    if (tokens[1] == "lock") {
      if (tokens.size() != 2) return "launcher lock takes no arguments";
      if (context.compositor->launcher_locked()) return "launch bar already locked";
      return context.compositor->toggle_launcher_lock();
    }
    if (tokens[1] == "unlock") {
      if (tokens.size() != 2) return "launcher unlock takes no arguments";
      if (!context.compositor->launcher_locked()) return "launch bar already unlocked";
      return context.compositor->toggle_launcher_lock();
    }
    if (tokens[1] == "toggle") {
      if (tokens.size() != 2) return "launcher toggle takes no arguments";
      return context.compositor->toggle_launcher_lock();
    }
    if (tokens[1] == "status") {
      if (tokens.size() != 2) return "launcher status takes no arguments";
      return context.compositor->launcher_status();
    }
    return "launcher supports: lock, unlock, toggle, status";
  }
};

class KeymapHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "keymap"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "keymap unavailable";
    if (tokens.size() < 2) {
      return "keymap supports: show, reload, profile <name>, reset, path, "
             "check <path>, chord <spec>";
    }
    const std::string &sub = tokens[1];
    if (sub == "show" || sub == "list") {
      if (tokens.size() != 2) return "keymap show takes no arguments";
      return context.compositor->keymap_info();
    }
    if (sub == "path") {
      if (tokens.size() != 2) return "keymap path takes no arguments";
      return "keymap file: " +
             (context.compositor->keymap().profile_path.empty()
                  ? std::string("(none loaded)")
                  : context.compositor->keymap().profile_path);
    }
    if (sub == "reload") {
      if (tokens.size() != 2) return "keymap reload takes no arguments";
      std::string error;
      if (context.compositor->load_keymap(error)) {
        return "reloaded " + context.compositor->keymap().profile_path +
               " (" + std::to_string(context.compositor->keymap().profiles.size()) +
               " profiles)";
      }
      return "keymap reload failed: " + error;
    }
    if (sub == "profile") {
      if (tokens.size() != 3) return "keymap profile requires a profile name";
      // select_profile reports the failure through the status line when the
      // name is unknown, so check the keymap rather than reporting success for
      // a profile that was never entered.
      std::string error;
      if (!context.compositor->keymap().profile_named(tokens[2], error)) {
        return error;
      }
      context.compositor->keymap_select_profile(tokens[2]);
      return "keymap profile: " + tokens[2];
    }
    if (sub == "reset") {
      if (tokens.size() != 2) return "keymap reset takes no arguments";
      context.compositor->keymap_reset_profile();
      return "keymap profile: " + context.compositor->keymap().default_profile;
    }
    if (sub == "chord") {
      // Validates a chord spelling without a file, so a user can check the
      // INI's key names from the Command Bar.
      if (tokens.size() != 3) return "keymap chord requires a chord such as <C-q>";
      KeyChord chord;
      std::string error;
      if (!parse_key_chord(tokens[2], chord, error)) return "keymap chord: " + error;
      return "chord <" + chord.str() + "> is code " + std::to_string(chord.code) +
             " with " + std::to_string(__builtin_popcount(chord.mods)) + " modifier(s)";
    }
    if (sub == "check") {
      // Parses a candidate file without adopting it, so a broken edit can be
      // diagnosed before `keymap reload` replaces a working keymap.
      const std::string path = join_tokens(tokens, 2);
      Keymap candidate;
      std::string error;
      if (!load_keymap(path, candidate, error)) return "keymap check failed: " + error;
      return path + " is valid: " + std::to_string(candidate.profiles.size()) +
             " profiles, prefix " +
             (candidate.prefix.code ? "<" + candidate.prefix.str() + ">"
                                    : std::string("(none)"));
    }
    return "keymap supports: show, reload, profile <name>, reset, path, "
           "check <path>, chord <spec>";
  }
};

class SessionHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "session"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "session control unavailable";
    if (tokens.size() < 2) {
      return "session supports: status, exit, restart";
    }
    if (tokens[1] == "status") return context.compositor->session_status();
    if (tokens[1] == "exit") return context.compositor->request_exit();
    if (tokens[1] == "restart") return context.compositor->request_restart();
    return "session supports: status, exit, restart";
  }
};

class ConfigHandler final : public CommandHandler {
public:
  bool matches(std::string_view command) const override { return command == "config"; }
  std::string execute(const std::vector<std::string> &tokens, CommandScope,
                      CommandContext &context) override {
    if (!context.compositor) return "configuration unavailable";
    if (tokens.size() >= 2) {
      if (tokens[1] == "vars") return context.compositor->config_variables();
      if (tokens[1] == "path") return context.compositor->config_file();
      if (tokens[1] == "open") return context.compositor->open_config();
      if (tokens[1] == "reload") return context.compositor->reload_config();
    }
    if (tokens.size() >= 2 && (tokens[1] == "eval" || tokens[1] == "run")) {
      // The expression parser owns quotes and escapes; do not reconstruct from tokens.
      size_t offset = context.raw_input.find_first_not_of(" \t\r\n");
      for (int i = 0; i < 2; ++i) {
        offset = context.raw_input.find_first_of(" \t\r\n", offset);
        offset = context.raw_input.find_first_not_of(" \t\r\n", offset);
      }
      if (offset != std::string::npos)
        return context.compositor->evaluate_config(context.raw_input.substr(offset), tokens[1] == "run");
    }
    return "config supports: vars, path, open, reload, eval <expression>, run <expression>";
  }
};

void register_builtin_handlers(CommandBar &bar) {
  bar.register_handler(std::make_unique<ConfigHandler>());
  bar.register_handler(std::make_unique<NCursorHandler>());
  bar.register_handler(std::make_unique<OutputHandler>());
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
  bar.register_handler(std::make_unique<MuxHandler>());
  bar.register_handler(std::make_unique<WorkspaceCommandHandler>());
  bar.register_handler(std::make_unique<PluginHandler>());
  bar.register_handler(std::make_unique<ExtensionHandler>());
  bar.register_handler(std::make_unique<ScriptHandler>());
  bar.register_handler(std::make_unique<TerminalHandler>());
  bar.register_handler(std::make_unique<SessionHandler>());
  bar.register_handler(std::make_unique<LauncherHandler>());
  bar.register_handler(std::make_unique<KeymapHandler>());
}
