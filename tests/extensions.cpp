#include "compositor/Compositor.hpp"
#include "lua/LuaEngine.hpp"
#include "plugin/PluginManager.hpp"
#include <cstdlib>
#include <iostream>
#include <chrono>

void check(bool value, const std::string &message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
  setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent/diftray", 1);
  setenv("DIFTRAYWM_EXTENSION_PATH", "/nonexistent/diftray", 1);
  Compositor compositor;
  check(compositor.init(), "compositor init failed");
  auto &bar = compositor.command_bar();
  {
    PluginManager manager(&bar);
    check(!manager.load(FAILED_PLUGIN), "accepted failing plugin");
    check(!bar.dispatch("fixture"), "failed plugin leaked command");
    check(manager.load(TEST_PLUGIN), manager.error());
    check(manager.load(TEST_PLUGIN) && manager.loaded().size() == 1, "duplicate plugin load");
    check(bar.dispatch("fixture test") && bar.status_line() == "test", "native command failed");
    bar.scope = CommandScope::NCURSOR_GLOBAL;
    check(!bar.dispatch("fixture"), "native scope ignored");
    bar.scope = CommandScope::CELL;
    for (const auto *event : {"frame", "view", "input"}) {
      manager.notify(event);
      check(bar.status_line() == "plugin " + std::string(event), "native event failed");
    }
    manager.unload_all();
    check(!bar.dispatch("fixture"), "unloaded plugin leaked command");
    check(manager.load(TEST_PLUGIN), manager.error());
  }
  check(!bar.dispatch("fixture"), "plugin destructor leaked command");
  {
    LuaEngine lua(&bar);
    check(lua.init(), "Lua init failed");
    check(lua.load_source("test", R"(
      assert(io == nil and os == nil and package == nil and debug == nil)
      assert(load == nil and dofile == nil and pcall == nil and coroutine == nil)
      diftray.register_command('luatest', function(tokens, scope)
        diftray.status(tokens[2] .. ':' .. scope)
      end)
      diftray.on('view', function(event) diftray.status('lua ' .. event) end)
      diftray.command('luatest queued')
    )"), lua.error());
    lua.drain_commands();
    check(bar.status_line() == "queued:global", "queued Lua command failed");
    check(bar.dispatch("luatest live") && bar.status_line() == "live:cell", "Lua command failed");
    lua.notify("view");
    check(bar.status_line() == "lua view", "Lua event failed");
    check(!lua.load_source("failed", "diftray.register_command('rollback', function() end); diftray.command('luatest leaked'); error('fail')"), "accepted Lua error");
    check(!bar.dispatch("rollback"), "failed Lua leaked registration");
    bar.set_status_line("unchanged"); lua.drain_commands();
    check(bar.status_line() == "unchanged", "failed Lua leaked queued command");
    const auto started = std::chrono::steady_clock::now();
    check(!lua.load_source("loop", "while true do end"), "unbounded Lua loop");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(1), "Lua timeout too slow");
    check(!lua.load_source("memory", "local x = string.rep('x', 32 * 1024 * 1024)"), "unbounded Lua memory");
    check(lua.load_source("bad event", "diftray.on('input', function() while true do end end)"), lua.error());
    lua.notify("input");
    check(lua.error().find("budget") != std::string::npos, "event loop not bounded");
    check(bar.dispatch("luatest after") && bar.status_line() == "after:cell", "Lua failure poisoned other scripts");
  }
  check(!bar.dispatch("luatest"), "Lua destructor leaked command");
}
