// Tests for the launcher taskbar and Caps Lock as Meta.
//
// Caps Lock as Meta is easy to get subtly wrong: the compositor must see Meta
// so bindings fire, but the terminal must still receive the untouched modifier
// set, otherwise holding Caps to type would stop producing capitals. Both
// directions are checked here, as is the config opt-out.
#include "compositor/Compositor.hpp"
#include "config/Config.hpp"
#include "config/ConfigProgram.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"

#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kPressed = WL_KEYBOARD_KEY_STATE_PRESSED;
constexpr uint32_t kLogo = WLR_MODIFIER_LOGO;
constexpr uint32_t kCtrl = WLR_MODIFIER_CTRL;
constexpr uint32_t kCaps = WLR_MODIFIER_CAPS;

void check(bool ok, const std::string &message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string dispatch(Compositor &c, const std::string &command) {
  c.command_bar().dispatch(command);
  return c.command_bar().status_line();
}

void init_compositor(Compositor &c, const std::string &config) {
  setenv("DIFTRAYWM_CONFIG", config.c_str(), 1);
  setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  setenv("DIFTRAYWM_EXTENSION_PATH", "/nonexistent", 1);
  setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent", 1);
  check(c.init(), "init failed: " + c.status_line());
}

}  // namespace

struct CompositorTestAccess {
  static bool launcher_locked(Compositor &c) { return c.launcher_locked_; }
  static bool launcher_mode(Compositor &c) { return c.launcher_mode_; }
  static std::size_t ncursor_count(Compositor &c) {
    std::size_t count = 0;
    for (const auto &line : split_lines(c.list_views()))
      if (!line.empty()) ++count;
    return count;
  }
  static std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> out;
    std::string current;
    for (char ch : text) {
      if (ch == '\n') { out.push_back(current); current.clear(); }
      else current.push_back(ch);
    }
    out.push_back(current);
    return out;
  }
};

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();

  // 1. Caps Lock is an ordinary modifier. It was briefly treated as a second
  // Meta source, and that is reverted, so the launcher still has to be
  // reachable from a real Meta key. A keyboard without a Logo key is served by
  // the keymap INI's [meta] prefix instead, which is configurable.
  {
    Compositor compositor;
    init_compositor(compositor, (root / "diftray.conf").string());
    // Meta+colon opens the NCursor global command bar, i.e. Meta+:.
    check(compositor.handle_key(XKB_KEY_colon, kLogo, kPressed, ':'),
          "Meta+colon was not handled");
    check(compositor.handle_key(XKB_KEY_Escape, 0, kPressed, 0),
          "escape after the bar did not close it");
    // The Logo key must keep working unchanged.
    const std::size_t before = CompositorTestAccess::ncursor_count(compositor);
    check(compositor.handle_key(XKB_KEY_n, kLogo, kPressed, 0),
          "Meta+N was not handled");
    check(CompositorTestAccess::ncursor_count(compositor) == before + 1,
          "Meta+N regressed");
  }

  // 2. Caps Lock is an ordinary modifier again. It was briefly treated as a
  // second Meta source, and that is reverted: Caps+N must type, not open a new
  // NCursor. A keyboard without a Logo key is served by the keymap INI's
  // [init] prefix instead, which is configurable rather than hard-coded.
  {
    Compositor compositor;
    init_compositor(compositor, (root / "diftray.conf").string());
    const std::size_t before = CompositorTestAccess::ncursor_count(compositor);
    compositor.handle_key(XKB_KEY_n, kCaps, kPressed, 0);
    check(CompositorTestAccess::ncursor_count(compositor) == before,
          "Caps+N acted as Meta+N");
    compositor.handle_key(XKB_KEY_d, kCaps, kPressed, 0);
    check(!CompositorTestAccess::launcher_mode(compositor),
          "Caps+D opened the launcher, so Caps is still acting as Meta");
  }

  // 4. The launcher lock, through the Command Bar.
  {
    Compositor compositor;
    init_compositor(compositor, (root / "diftray.conf").string());
    check(!compositor.launcher_locked(), "launcher starts unlocked by default");
    check(dispatch(compositor, "launcher status").find("launch bar: unlocked") !=
              std::string::npos,
          "launcher status reports the initial state");
    check(dispatch(compositor, "launcher lock") == "launch bar locked on top",
          "launcher lock");
    check(compositor.launcher_locked(), "lock did not take effect");
    check(dispatch(compositor, "launcher lock") == "launch bar already locked",
          "locking twice is reported, not silently repeated");
    check(dispatch(compositor, "launcher unlock") == "launch bar unlocked",
          "launcher unlock");
    check(!compositor.launcher_locked(), "unlock did not take effect");
    check(dispatch(compositor, "launcher unlock") == "launch bar already unlocked",
          "unlocking twice is reported");
    check(dispatch(compositor, "launcher toggle") == "launch bar locked on top",
          "launcher toggle");
    check(dispatch(compositor, "launcher toggle") == "launch bar unlocked",
          "launcher toggle back");
    check(dispatch(compositor, "launcher") ==
              "launcher supports: lock, unlock, toggle, status",
          "launcher usage");
    check(dispatch(compositor, "launcher dance").find("launcher supports:") == 0,
          "launcher rejects unknown subcommands");
    check(dispatch(compositor, "launcher lock now").find("takes no arguments") !=
              std::string::npos,
          "launcher lock rejects stray arguments");
    // The status block reports the geometry that the runtime lays out with.
    const auto status = dispatch(compositor, "launcher status");
    check(status.find("anchor: top") != std::string::npos,
          "launcher is anchored to the top: " + status);
    check(status.find("height: ") != std::string::npos,
          "launcher status reports its height: " + status);
  }

  // 5. The task list names the live NCursors and marks the active one.
  {
    Compositor compositor;
    init_compositor(compositor, (root / "diftray.conf").string());
    const std::string tasks = compositor.launcher_tasks();
    check(tasks.find("[primary") != std::string::npos,
          "the active NCursor is marked: " + tasks);
    dispatch(compositor, "ncursor new");
    const std::string with_tabs = compositor.launcher_tasks();
    check(with_tabs != tasks, "a second NCursor appears in the taskbar");
    check(with_tabs.find("[") != std::string::npos,
          "exactly one task is active: " + with_tabs);
  }

  // 6. Config: launcher_locked and the launcher bar metrics.
  {
    char dir[] = "/tmp/diftray-launcher-cfg-XXXXXX";
    check(mkdtemp(dir) != nullptr, "mkdtemp failed");
    const auto base = std::filesystem::path(dir);
    const auto write = [&](const char *name, const std::string &body) {
      const auto path = base / name;
      std::ofstream(path) << body;
      return path.string();
    };
    // DSL
    {
      CompositorConfig c;
      std::string error;
      check(load_compositor_config(write("a.conf",
          "general {\n launcher_locked = true\n launcher_bar_height = 44\n"
          " launcher_bar_color = #10203080\n}\n"),
          c, error), error);
      check(c.launcher_locked && c.launcher_bar_height == 44 &&
                c.launcher_bar_color[0] == 0x10 / 255.f,
            "DSL launcher settings");
    }
    // TOML uses real booleans, not strings.
    {
      CompositorConfig c;
      std::string error;
      check(load_compositor_config(write("b.toml",
          "[general]\nlauncher_locked = true\n"), c, error), error);
      check(c.launcher_locked, "TOML booleans");
    }
    // YAML
    {
      CompositorConfig c;
      std::string error;
      check(load_compositor_config(write("c.yaml",
          "general:\n  launcher_locked: true\n"), c, error), error);
      check(c.launcher_locked, "YAML booleans");
    }
    // launcher_locked in the config must be what the compositor starts with.
    {
      CompositorConfig c;
      std::string error;
      check(load_compositor_config(write("d.conf",
          "general {\n launcher_locked = true\n}\n"), c, error), error);
      Compositor compositor;
      init_compositor(compositor, (base / "d.conf").string());
      check(CompositorTestAccess::launcher_locked(compositor),
            "the launcher did not start locked from the configuration");
      // A runtime lock is remembered, so the next config reload keeps it.
      check(dispatch(compositor, "launcher unlock") == "launch bar unlocked",
            "unlock");
      check(dispatch(compositor, "config reload").find("reloaded") == 0,
            "config reload after unlocking");
      check(!CompositorTestAccess::launcher_locked(compositor),
            "the runtime lock was overwritten by the config value on reload");
    }
    // Invalid values are rejected.
    for (const auto *body : {"general {\n launcher_locked = maybe\n}\n",
                             "general {\n launcher_locked = 1.5\n}\n",
                             "general {\n launcher_bar_height = 0\n}\n",
                             "general {\n launcher_bar_color = notacolor\n}\n"}) {
      CompositorConfig c;
      std::string error;
      const auto path = base / "bad.conf";
      std::ofstream(path) << body;
      check(!load_compositor_config(path.string(), c, error),
            std::string("accepted invalid launcher config: ") + body);
    }
    std::filesystem::remove_all(dir);
  }

  std::cout << "launcher taskbar and Caps Lock as Meta checks passed\n";
  return EXIT_SUCCESS;
}
