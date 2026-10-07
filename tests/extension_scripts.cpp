// Covers the Lua extensions shipped in extensions/. Each script is loaded into
// the real engine, so a syntax error, a missing sandbox function or a runaway
// loop fails here rather than on someone's desktop.
#include "compositor/Compositor.hpp"
#include "lua/LuaEngine.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void check(bool value, const std::string &message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string slurp(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  check(file.good(), "cannot read extension: " + path.string());
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

// Every script in the directory, in the same order the compositor loads them.
std::vector<std::filesystem::path> extension_files(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> paths;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    std::cerr << "no extension directory at " << root << '\n';
    std::exit(1);
  }
  for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".lua") {
      paths.push_back(it->path());
    }
  }
  check(!ec, "cannot read the extension directory");
  std::sort(paths.begin(), paths.end());
  return paths;
}

// One named assertion about a status line, so a failure says what broke rather
// than printing two strings and leaving the reader to compare them.
void expect(CommandBar &bar, const std::string &command, const std::string &needle,
            const std::string &file) {
  bar.dispatch(command);
  const auto &status = bar.status_line();
  if (status.find(needle) == std::string::npos) {
    std::cerr << file << ": `" << command << "` reported \"" << status
              << "\", expected to contain \"" << needle << "\"\n";
    std::exit(1);
  }
}

}  // namespace

int main(int argc, char **argv) {
  const std::filesystem::path directory =
      argc > 1 ? argv[1] : std::filesystem::path(EXTENSION_DIR);
  const auto files = extension_files(directory);
  check(!files.empty(), "no extensions found in " + directory.string());

  setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent/diftray", 1);
  Compositor compositor;
  check(compositor.init(), "compositor init failed");
  auto &bar = compositor.command_bar();

  LuaEngine lua(&bar);
  check(lua.init(), "Lua init failed");
  for (const auto &path : files) {
    check(lua.load_source(path.string(), slurp(path)), path.string() + ": " + lua.error());
  }
  check(lua.extensions().size() == files.size(), "not every extension loaded");

  // The commands each script must answer for. Two words: the extension's own
  // name and a subcommand, so a renamed subcommand is caught here.
  struct Entry { const char *script; const char *command; };
  const std::vector<Entry> registered = {
      {"autocomplete.lua", "ac"},        {"autocomplete.lua", "ac stats"},
      {"keyremap.lua", "remap"},          {"keyremap.lua", "remap stats"},
      {"history.lua", "hist"},            {"history.lua", "hist stats"},
      {"macro.lua", "macro"},             {"macro.lua", "macro stats"},
      {"workspace.lua", "ws"},            {"workspace.lua", "ws board"},
      {"skin.lua", "skin"},               {"skin.lua", "skin keys"},
      {"notepad.lua", "note"},            {"notepad.lua", "note ls"},
      {"pulse.lua", "pulse"},              {"pulse.lua", "pulse alerts"},
  };
  for (const auto &entry : registered) {
    bar.dispatch(entry.command);
    check(bar.status_line().find("unknown command") == std::string::npos,
          std::string(entry.script) + " did not register `" + entry.command + "`");
  }

  // autocomplete: search, learn, rank, replay, forget.
  expect(bar, "ac", "ac:", "autocomplete.lua");
  expect(bar, "ac key", "prefix match", "autocomplete.lua");
  expect(bar, "ac find theme", "theme", "autocomplete.lua");
  expect(bar, "ac find zzzznothing", "nothing matches", "autocomplete.lua");
  expect(bar, "ac add my custom line", "learned", "autocomplete.lua");
  expect(bar, "ac top 3", "my custom line", "autocomplete.lua");
  expect(bar, "ac find my custom", "my custom line", "autocomplete.lua");
  expect(bar, "ac show 1", "my custom line", "autocomplete.lua");
  expect(bar, "ac pick 99", "needs an index", "autocomplete.lua");
  expect(bar, "ac rm my custom line", "forgot", "autocomplete.lua");
  expect(bar, "ac rm keymap show", "keeps", "autocomplete.lua");
  expect(bar, "ac decay", "aged", "autocomplete.lua");
  expect(bar, "ac clear", "forgot every learned line", "autocomplete.lua");
  expect(bar, "ac", "lines, most likely first", "autocomplete.lua");

  // keyremap: chord validation mirrors the compositor's own table.
  expect(bar, "remap", "0 chords", "keyremap.lua");
  expect(bar, "remap add", "needs a chord", "keyremap.lua");
  expect(bar, "remap add <M-f3> spawn below", "Diftray(spawn below)", "keyremap.lua");
  expect(bar, "remap add <M-C-M-x> spawn below", "duplicate modifier", "keyremap.lua");
  expect(bar, "remap add <M-nosuchkey> spawn below", "unknown key", "keyremap.lua");
  expect(bar, "remap add <M-f4> Bogus(1)", "unknown action", "keyremap.lua");
  expect(bar, "remap add <M-f5> Exec(foot)", "compositor-only", "keyremap.lua");
  expect(bar, "remap", "2 chords", "keyremap.lua");
  expect(bar, "remap find spawn", "remap find spawn", "keyremap.lua");
  expect(bar, "remap find nothingmatches", "no chord or action mentions",
         "keyremap.lua");
  expect(bar, "remap rm <M-f9>", "needs a chord that is in the table",
         "keyremap.lua");
  expect(bar, "remap rm <M-f3>", "removed", "keyremap.lua");
  expect(bar, "remap", "1 chord", "keyremap.lua");
  expect(bar, "remap write", "[profile-1]", "keyremap.lua");
  expect(bar, "remap write", "<M-f5> = Exec(foot)", "keyremap.lua");
  expect(bar, "remap write", "diftrayremap honours Ignore()", "keyremap.lua");
  expect(bar, "remap profile work", "[work]", "keyremap.lua");
  expect(bar, "remap prefix <M-p>", "[init] prefix is now", "keyremap.lua");
  // The two prefixes must not collide; a shared chord would make whichever ran
  // first swallow the key.
  expect(bar, "remap prefix <C-q>", "two jobs", "keyremap.lua");
  expect(bar, "remap metaprefix <M-p>", "must", "keyremap.lua");
  expect(bar, "remap check <M-nope>", "not a chord", "keyremap.lua");
  expect(bar, "remap clear", "emptied", "keyremap.lua");
  // The local chord table has to agree with the compositor's own parser in both
// directions. keymap chord is the authority, so it decides the expectation and
// the extension is checked against it. `<G-colon>` is in the list because it
// appears in a keymap.ini comment and must be rejected, not accepted by a table
// that guessed a different set of names.
  for (const char *chord : {"<C-f8>", "<M-q>", "<M-esc>", "<G-semicolon>",
                            "<G-colon>", "<C-G-Up>", "<G-f1>", "<G-0>",
                            "<G-leftbrace>", "<C-M-a>", "<S-a>", "a", ";",
                            "<M-1>", "<A-f4>", "<M-nosuchkey>", "<C-C-a>"}) {
    bar.dispatch(std::string("keymap chord ") + chord);
    const bool compositor_accepts =
        bar.status_line().find("is code") != std::string::npos;
    bar.dispatch(std::string("remap add ") + chord + " Ignore()");
    const bool extension_accepts =
        bar.status_line().find("= Ignore()") != std::string::npos;
    check(compositor_accepts == extension_accepts,
          std::string("keyremap.lua disagrees with the compositor about ") +
              chord + ": compositor " +
              (compositor_accepts ? "accepts" : "rejects") + ", extension " +
              (extension_accepts ? "accepts" : "rejects"));
    bar.dispatch("remap clear");
  }

  // history: record, search, plan an undo, and refuse to undo what it cannot.
  expect(bar, "hist", "empty", "history.lua");
  expect(bar, "hist record spawn below", "recorded", "history.lua");
  expect(bar, "hist record tab next", "undo with tab prev", "history.lua");
  expect(bar, "hist record mux split horizontal", "undo with mux kill", "history.lua");
  expect(bar, "hist record workspace 4", "no safe reverse", "history.lua");
  expect(bar, "hist", "newest first", "history.lua");
  expect(bar, "hist 2", "last 2 entries", "history.lua");
  expect(bar, "hist search spawn", "spawn below", "history.lua");
  expect(bar, "hist search nothingmatches", "nothing recorded mentions", "history.lua");
  expect(bar, "hist undo --dry", "dry run", "history.lua");
  expect(bar, "hist undo 4", "nothing was changed", "history.lua");
  expect(bar, "hist once tab prev", "ran and recorded", "history.lua");
  expect(bar, "hist clear", "cleared", "history.lua");

  // macro: create, extend, guard the name, replay.
  expect(bar, "macro", "0 macros", "macro.lua");
  expect(bar, "macro save", "needs a name", "macro.lua");
  expect(bar, "macro save run", "subcommand", "macro.lua");
  expect(bar, "macro save wide output focus next", "output focus next", "macro.lua");
  expect(bar, "macro add wide mux split horizontal", "step 2", "macro.lua");
  expect(bar, "macro add wide spawn below", "step 3", "macro.lua");
  expect(bar, "macro show wide", "3 steps", "macro.lua");
  expect(bar, "macro show wide", "mux split horizontal", "macro.lua");
  expect(bar, "macro add ghost spawn below", "macro save ghost first", "macro.lua");
  expect(bar, "macro run ghost", "needs a macro name", "macro.lua");
  expect(bar, "macro run 99", "outside", "macro.lua");
  expect(bar, "macro find mux", "wide", "macro.lua");
  expect(bar, "macro find nothingmatches", "nothing matches", "macro.lua");
  expect(bar, "macro rename wide w", "renamed to w", "macro.lua");
  expect(bar, "macro", "1 macro", "macro.lua");
  expect(bar, "macro clear", "cleared", "macro.lua");

  // workspace: labels, ordering and the 1..10 range.
  expect(bar, "ws", "unlabelled", "workspace.lua");
  expect(bar, "ws mark 3 editor", "is now", "workspace.lua");
  expect(bar, "ws mark 1 chat", "is now", "workspace.lua");
  expect(bar, "ws mark 10 mail", "is now", "workspace.lua");
  expect(bar, "ws mark 0 zero", "1 to 10", "workspace.lua");
  expect(bar, "ws mark 11 eleven", "1 to 10", "workspace.lua");
  expect(bar, "ws mark 3", "needs a label", "workspace.lua");
  expect(bar, "ws", "editor", "workspace.lua");
  expect(bar, "ws find ed", "editor", "workspace.lua");
  expect(bar, "ws find nothingmatches", "no label mentions", "workspace.lua");
  expect(bar, "ws mark 1 talk", "relabelled", "workspace.lua");
  expect(bar, "ws unmark 1", "lost the label", "workspace.lua");
  expect(bar, "ws next", "no current workspace was recorded", "workspace.lua");
  expect(bar, "ws clear", "cleared", "workspace.lua");

  // skin: property names are hyphenated, and an unknown one is called out
  // rather than silently stored as a token nothing reads.
  expect(bar, "skin", "0 presets", "skin.lua");
  expect(bar, "skin set", "needs a name", "skin.lua");
  expect(bar, "skin set big border-size 1px", "border-size", "skin.lua");
  expect(bar, "skin set big border-size", "needs a value", "skin.lua");
  expect(bar, "skin set big bogus-thing 4", "does not read", "skin.lua");
  expect(bar, "skin keys", "background-color", "skin.lua");
  expect(bar, "skin edit big border-size 2px", "replaced", "skin.lua");
  expect(bar, "skin apply big", "border-size: 2px", "skin.lua");
  expect(bar, "skin apply ghost", "needs a preset name", "skin.lua");
  expect(bar, "skin add plain :root { border-size: 2px; }", "border-size: 2px",
         "skin.lua");
  expect(bar, "skin find border", "big", "skin.lua");
  expect(bar, "skin rm big", "forgot", "skin.lua");
  expect(bar, "skin rm big", "needs a preset name", "skin.lua");
  expect(bar, "skin clear", "cleared", "skin.lua");

  // note: names for identifiers, and a pin that reaches the compositor.
  expect(bar, "note", "0 cells, 0 cursors", "notepad.lua");
  expect(bar, "note watch 3af1 vim", "watching cell", "notepad.lua");
  expect(bar, "note watch 3af1 neovim", "renamed cell", "notepad.lua");
  expect(bar, "note watch 3af1", "needs a name", "notepad.lua");
  expect(bar, "note watch-cursor quail browser", "watching cursor", "notepad.lua");
  expect(bar, "note", "neovim", "notepad.lua");
  expect(bar, "note pin browser F1", "Quick Restore F1", "notepad.lua");
  expect(bar, "note pin", "needs a cursor identifier", "notepad.lua");
  expect(bar, "note where", "cell move", "notepad.lua");
  expect(bar, "note find neo", "3af1", "notepad.lua");
  expect(bar, "note find nothingmatches", "no cell is named", "notepad.lua");
  expect(bar, "note unwatch 3af1", "stopped watching cell", "notepad.lua");
  expect(bar, "note unwatch 3af1", "does not have", "notepad.lua");
  expect(bar, "note clear", "cleared", "notepad.lua");

  // pulse: counters, gaps and the alert state machine.
  expect(bar, "pulse", "nothing has been observed yet", "pulse.lua");
  expect(bar, "pulse pause", "paused", "pulse.lua");
  expect(bar, "pulse frames", "0 seen", "pulse.lua");
  expect(bar, "pulse keys", "0 seen", "pulse.lua");
  expect(bar, "pulse alerts", "alerts is off", "pulse.lua");
  expect(bar, "pulse alerts on", "alerts on", "pulse.lua");
  expect(bar, "pulse alerts maybe", "alerts is on", "pulse.lua");
  expect(bar, "pulse threshold", "threshold is 600 frames", "pulse.lua");
  expect(bar, "pulse threshold 120", "idle after 120 frames", "pulse.lua");
  expect(bar, "pulse threshold 0", "1 to 1000000", "pulse.lua");
  expect(bar, "pulse resume", "resumed", "pulse.lua");
  expect(bar, "pulse reset", "reset", "pulse.lua");
  expect(bar, "pulse nonsense", "supports:", "pulse.lua");

  // Every script must survive being loaded with the sandbox taken away. This is
  // what stops a future edit from reaching for io or os and only failing on a
  // user's machine.
  check(lua.load_source("sandbox", R"(
      assert(io == nil and os == nil and package == nil and debug == nil)
      assert(load == nil and dofile == nil and pcall == nil and coroutine == nil)
      assert(setmetatable == nil and getmetatable == nil and print == nil)
      assert(string.dump == nil and collectgarbage == nil)
      diftray.register_command('sandboxprobe', function(tokens, scope)
        diftray.status(tokens[2] .. ':' .. scope)
      end)
    )"), lua.error());

  // Event hooks are shared by every script, so all of them are fired here. A
  // callback that throws takes its own event's hooks down with it, which would
  // otherwise show up much later as a silently dead extension.
  lua.notify("view");
  check(lua.error().empty(), "a view hook failed: " + lua.error());
  lua.notify("input");
  check(lua.error().empty(), "an input hook failed: " + lua.error());
  lua.notify("frame");
  check(lua.error().empty(), "a frame hook failed: " + lua.error());

  // pulse counts these, so the hooks have to actually have run rather than
  // merely not have thrown.
  expect(bar, "pulse", "frames  ", "pulse.lua");
  expect(bar, "pulse", "frames  1", "pulse.lua");
  expect(bar, "pulse", "views   1", "pulse.lua");
  expect(bar, "pulse", "keys    1", "pulse.lua");
  expect(bar, "pulse frames", "1 seen", "pulse.lua");
  expect(bar, "pulse reset", "reset", "pulse.lua");
  expect(bar, "pulse frames", "0 seen", "pulse.lua");

  std::cout << files.size() << " extensions passed\n";
}
