#include "compositor/Compositor.hpp"
#include "config/Config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static void check(bool ok, const std::string &message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
int main() {
  std::string error;
  const std::string source = R"CFG(
let base = 7;
let code = keycode(33);
let symbol = keysym("F8");
let text = "scratchpad";
let flag = true;
fn twice(n) = n * 2;
fn workspace(n) = "workspace " + str(n);
fn factorial(n) = n <= 1 ? 1 : n * factorial(n - 1);
fn eager(a, b) = a;
macro first(a, b) = a;
macro notebook(name) = commands("spawn below", "notelet open " + name);
macro nested(x) = first(x, 1 / 0);
fn caller(x) = nested(x + 1);
fn lexical(base) = global();
fn global() = base;
fn loop() = loop();
fn command_loop() = "config run command_loop()";
eval first(true, 1 / 0);
bind("Meta", symbol, workspace(3));
bind("Meta", code, workspace(4));
bind("Meta+Shift", keysym("F9"), commands(workspace(5), workspace(6)), "cell");
bind("Meta", keysym("F10"), "capture", "cell");
bind("Meta", keysym("F11"), "capture", "global");
)CFG";
  auto program = ConfigProgram::compile(source, "test", error);
  check(bool(program), error);
  const auto eval = [&](const std::string &expression,
                        const std::string &expected) {
    ConfigValue value;
    check(program->evaluate(expression, value, error),
          expression + ": " + error);
    check(ConfigProgram::describe(value) == expected,
          expression + ": unexpected value " + ConfigProgram::describe(value));
  };
  eval("twice(base)", "14");
  eval("factorial(6)", "720");
  eval("nested(42)", "42");
  eval("caller(9)", "10");
  eval("lexical(100)", "7");
  eval("symbol", "keysym(\"F8\")");
  eval("code", "keycode(33)");
  eval("keysym(\"Return\") == keycode(28)", "false");
  eval("first(3, missing)", "3");
  eval("false && missing", "false");
  eval("flag ? text : missing", "scratchpad");
  eval("notebook(text)",
       "commands(\"spawn below\", \"notelet open scratchpad\")");
  for (const auto &expression :
       {"eager(1, 1 / 0)", "loop()", "unknown", "twice()", "1 + true",
        "keysym(\"NotARealSymbol\")", "keycode(0)", "keycode(768)",
        "keycode(1.5)", "1e308 * 1e308", "true ? 1", "1 / 0", "commands(7)",
        "str(code)"}) {
    ConfigValue value = std::string("unchanged");
    check(!program->evaluate(expression, value, error),
          std::string("accepted invalid expression: ") + expression);
    check(std::get<std::string>(value) == "unchanged",
          "failed evaluation changed result");
  }
  for (const auto &bad :
       {"let x = 1; let x = 2;", "fn x(a,a) = a;", "let keysym = 2;",
        "let a = b; let b = 1;", "fn eager(a,b) = a; eval eager(1,1/0);",
        "bind(\"Meta\", 33, \"workspace 2\");",
        "bind(\"Meta+\", keycode(33), \"workspace 2\");",
        "bind(\"Meta\", keycode(33), \"\");",
        "bind(\"Meta\", keycode(33), \"workspace 2\"); bind(\"Super\", "
        "keycode(33), \"workspace 3\");",
        "let x = 1", "eval unknown();",
        "bind(\"Meta\", keysym(\"F1\"), \"workspace 2\", \"bad\");"}) {
    check(!ConfigProgram::compile(bad, "invalid", error),
          std::string("accepted invalid program: ") + bad);
  }
  const auto &bindings = program->bindings();
  check(bindings.size() == 5 && !bindings[2].global, "binding scopes");
  check(bindings[1].matches(XKB_KEY_z, 33, 64 | 2 | 16),
        "physical key with locks");
  check(!bindings[1].matches(XKB_KEY_F8, 41, 64),
        "physical code must not add XKB offset");
  check(!bindings[0].matches(XKB_KEY_F8, 0, 64 | 1),
        "extra modifier must not match");
  check(bindings[0].matches(XKB_KEY_F8, 20, 64), "symbol follows layout");

  char dir[] = "/tmp/diftray-program-XXXXXX";
  check(mkdtemp(dir), "temporary directory");
  const auto file = [&](const std::string &name, const std::string &body) {
    auto path = std::filesystem::path(dir) / name;
    std::ofstream(path) << body;
    return path.string();
  };
  const std::string settings_program =
      source + "\nlet rotation = 90; let label = \"DP-1\";\n";
  std::string yaml = "program: |\n";
  for (std::istringstream lines(settings_program); !lines.eof();) {
    std::string line;
    std::getline(lines, line);
    yaml += "  " + line + "\n";
  }
  const auto dsl =
      file("settings.conf",
           "program {\n" + settings_program +
               "}\ngeneral {\n font_size = $(twice(base))\n}\nmonitor {\n name "
               "= $(label)\n rotation = $(rotation)\n}\n");
  const auto toml = file(
      "settings.toml",
      "program = '''\n" + settings_program +
          "'''\n[general]\nfont_size = '$(twice(base))'\n[[monitors]]\nname = "
          "'$(label)'\nrotation = '$(rotation)'\n");
  const auto yaml_path =
      file("settings.yaml",
           yaml + "general:\n  font_size: '$(twice(base))'\nmonitors:\n  - "
                  "name: '$(label)'\n    rotation: '$(rotation)'\n");
  for (const auto &path : {dsl, toml, yaml_path}) {
    CompositorConfig config;
    check(load_compositor_config(path, config, error), path + ": " + error);
    check(config.font_size == 14 && config.monitors.size() == 1 &&
              config.monitors[0].rotation == 90 &&
              config.monitors[0].name == "DP-1" &&
              config.program->bindings().size() == 5,
          "formats must agree");
    auto previous = config.program;
    const auto bad =
        file("bad.conf",
             "program { let x = 2; }\ngeneral {\n font_size = $(missing)\n}\n");
    check(!load_compositor_config(bad, config, error) &&
              config.program == previous && config.monitors[0].rotation == 90,
          "configuration loads must be atomic");
  }
  check(!ConfigProgram::compile(std::string(262145, ' '), "large", error),
        "source bound");
  CompositorConfig duplicate;
  check(
      !load_compositor_config(
          file("duplicate.conf", "program {}\nprogram {}\n"), duplicate, error),
      "duplicate program");
  std::string deep = "1";
  for (int i = 0; i < 1000; ++i)
    deep += " + 1";
  ConfigValue unused;
  check(!program->evaluate(deep, unused, error),
        "bound left-associated expression trees");
  check(!program->evaluate(std::string(100, '(') + "1" + std::string(100, ')'),
                           unused, error),
        "bound parser nesting");
  check(!ConfigProgram::compile("fn expand(n) = n == 0 ? 1 : expand(n-1) + "
                                "expand(n-1); eval expand(20);",
                                "budget", error),
        "bound evaluation work");
  for (const auto &extension : {"conf", "toml", "yaml"}) {
    CompositorConfig example;
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "examples" / (std::string("program.") + extension);
    check(load_compositor_config(path.string(), example, error), error);
    check(example.program->bindings().size() == 2 && example.font_size == 14 &&
              example.monitors[0].rotation == 90,
          "shipped program examples");
  }
  CompositorConfig quoted;
  check(load_compositor_config(
            file("quoted.conf", "program { let x = \"a}b\"; # } comment\n}\n"),
            quoted, error),
        error);

  setenv("DIFTRAYWM_CONFIG", dsl.c_str(), 1);
  setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  setenv("DIFTRAYWM_EXTENSION_PATH", "/nonexistent", 1);
  setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent", 1);
  Compositor compositor;
  check(compositor.init(), compositor.status_line());
  check(compositor.handle_key(XKB_KEY_F8, 64, 1, 0, 33) &&
            compositor.current_workspace() == 4,
        "physical binding must win");
  check(compositor.handle_key(XKB_KEY_F8, 64, 1, 0, 34) &&
            compositor.current_workspace() == 3,
        "symbol binding must execute");
  compositor.handle_key(XKB_KEY_z, 64 | 2 | 16, 1, 'z', 33);
  check(compositor.current_workspace() == 4,
        "physical binding independent of symbol and locks");
  compositor.handle_key(XKB_KEY_F8, 64, 0, 0, 34);
  check(compositor.current_workspace() == 4, "release must not dispatch");
  compositor.handle_key(XKB_KEY_F9, 64 | 1, 1, 0, 67);
  check(compositor.current_workspace() == 6, "command sequence");
  auto &bar = compositor.command_bar();
  CommandScope observed = CommandScope::NCURSOR_GLOBAL;
  check(bar.register_command("capture", &observed,
                             [&](const auto &, CommandScope scope) {
                               observed = scope;
                               return std::string("captured");
                             }),
        "register scope observer");
  bar.scope = CommandScope::NCURSOR_GLOBAL;
  compositor.handle_key(XKB_KEY_F10, 64, 1, 0, 68);
  check(observed == CommandScope::CELL &&
            bar.scope == CommandScope::NCURSOR_GLOBAL,
        "cell binding scope and restoration");
  bar.scope = CommandScope::CELL;
  compositor.handle_key(XKB_KEY_F11, 64, 1, 0, 87);
  check(observed == CommandScope::NCURSOR_GLOBAL &&
            bar.scope == CommandScope::CELL,
        "global binding scope and restoration");
  bar.unregister_owner(&observed);
  bar.dispatch("config run notebook(\"hello\")");
  check(bar.status_line() == "opened notelet hello", "execute Notelet macro");
  bar.dispatch("config eval notebook(\"hello\")");
  check(bar.status_line() ==
            "commands(\"spawn below\", \"notelet open hello\")",
        "preserve expression quotes");
  bar.dispatch("config eval \"\"");
  check(bar.status_line().empty(), "evaluate empty string");
  bar.dispatch("config run workspace(8)");
  check(compositor.current_workspace() == 8, "run function");
  bar.dispatch("config run command_loop()");
  check(bar.status_line().find("recursion limit") != std::string::npos,
        "command recursion bound");
  bar.dispatch("config vars");
  check(bar.status_line().find("code = keycode(33)") != std::string::npos,
        "list typed variables");
  std::filesystem::remove_all(dir);
}
