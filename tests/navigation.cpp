#include "compositor/Compositor.hpp"
#include "config/Config.hpp"

#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
  const auto config_path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                           "diftray.conf";
  CompositorConfig defaults;
  std::string error;
  if (!load_compositor_config(config_path.string(), defaults, error) ||
      defaults.theme != "themes/default.css" || defaults.status_bar_height != 24) {
    std::cerr << "shipped configuration failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  setenv("DIFTRAYWM_CONFIG", config_path.c_str(), 1);
  setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  Compositor compositor;
  if (!compositor.init()) {
    std::cerr << compositor.status_line() << '\n';
    return EXIT_FAILURE;
  }
  if (compositor.apply_theme_css(":root { border-size: 2px; background-color: #09121a; }") !=
      "theme applied" ||
      compositor.apply_theme_css(":root { border-size: nonsense; }") == "theme applied") {
    std::cerr << "theme parsing or validation failed\n";
    return EXIT_FAILURE;
  }
  if (compositor.current_workspace() != 1) {
    std::cerr << "default workspace is not 1\n";
    return EXIT_FAILURE;
  }
  if (!compositor.command_bar().dispatch("notelet list") ||
      compositor.command_bar().status_line().find("hello") == std::string::npos ||
      !compositor.command_bar().dispatch("notelet open hello") ||
      compositor.command_bar().status_line() != "opened notelet hello" ||
      !compositor.handle_key(XKB_KEY_x, 0, WL_KEYBOARD_KEY_STATE_PRESSED, 'x') ||
      !compositor.command_bar().dispatch("notelet close") ||
      compositor.command_bar().status_line() != "removed cell") {
    std::cerr << "Notelet command or input failed\n";
    return EXIT_FAILURE;
  }

  const auto spawned = compositor.spawn_ncursor();
  if (spawned.find("opened ncursor") != 0) {
    std::cerr << "spawn ncursor failed: " << spawned << '\n';
    return EXIT_FAILURE;
  }

  const auto next = compositor.cycle_tab(1);
  if (next.find("tab ") != 0) {
    std::cerr << "tab next failed: " << next << '\n';
    return EXIT_FAILURE;
  }
  const auto prev = compositor.cycle_tab(-1);
  if (prev.find("tab ") != 0) {
    std::cerr << "tab prev failed: " << prev << '\n';
    return EXIT_FAILURE;
  }

  if (!compositor.handle_key(XKB_KEY_Right, WLR_MODIFIER_LOGO,
                             WL_KEYBOARD_KEY_STATE_PRESSED, 0)) {
    std::cerr << "Meta+Right not handled\n";
    return EXIT_FAILURE;
  }
  if (!compositor.handle_key(XKB_KEY_Left, WLR_MODIFIER_LOGO,
                             WL_KEYBOARD_KEY_STATE_PRESSED, 0)) {
    std::cerr << "Meta+Left not handled\n";
    return EXIT_FAILURE;
  }

  if (!compositor.handle_key(XKB_KEY_2, WLR_MODIFIER_LOGO,
                             WL_KEYBOARD_KEY_STATE_PRESSED, 0) ||
      compositor.current_workspace() != 2) {
    std::cerr << "Meta+2 did not switch workspace\n";
    return EXIT_FAILURE;
  }
  if (compositor.cycle_tab(1) != "only one tab") {
    std::cerr << "new workspace should start with one tab\n";
    return EXIT_FAILURE;
  }

  if (!compositor.command_bar().dispatch("workspace 1") ||
      compositor.current_workspace() != 1) {
    std::cerr << "workspace command failed: "
              << compositor.command_bar().status_line() << '\n';
    return EXIT_FAILURE;
  }
  if (!compositor.command_bar().dispatch("tab next")) {
    std::cerr << "tab command failed: " << compositor.command_bar().status_line()
              << '\n';
    return EXIT_FAILURE;
  }

  if (!compositor.handle_key(XKB_KEY_0, WLR_MODIFIER_LOGO,
                             WL_KEYBOARD_KEY_STATE_PRESSED, 0) ||
      compositor.current_workspace() != 10) {
    std::cerr << "Meta+0 did not switch to workspace 10\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
