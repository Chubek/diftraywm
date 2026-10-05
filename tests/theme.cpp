#include "compositor/Compositor.hpp"
#include "theme/ThemeEngine.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

struct CompositorTestAccess {
  static const CompositorConfig &config(const Compositor &c) { return c.config_; }
  static const NTermRenderer::Style &style(const Compositor &c) { return c.nterm_renderer_.style(); }
};
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
}
int main() {
  char directory[] = "/tmp/diftray-theme-XXXXXX";
  if (!mkdtemp(directory)) return EXIT_FAILURE;
  int result = EXIT_SUCCESS;
  try {
    ThemeEngine engine;
    ThemeProperties properties;
    check(engine.parse_css("/* theme */ :root { --accent: #abc; border-color: var(--accent); border-size: 0; command-bar-height: 2px; }", properties),
          "comments and CSS variables failed");
    check(properties.colors.at("border-color") == "#abc" && properties.metrics.at("command-bar-height") == "2px",
          "resolved properties classified incorrectly");
    check(engine.parse_css(":root { border-color: var(--missing, #123); }", properties), "variable fallback failed");
    for (const auto *css : {":root { border-color; }", ":root { border-color:; }", ":root { border-size: 1.2.3px; }",
                          ":root { border-size: -1px; }", "/* unterminated", ":root", "{ color: #123; }",
                          ":root { --a: var(--b); --b: var(--a); border-color: var(--a); }"}) {
      properties.tokens["retained"] = "yes";
      check(!engine.parse_css(css, properties), "invalid CSS accepted");
      check(properties.tokens.at("retained") == "yes", "failed parse changed candidate theme");
    }
    const auto path = std::filesystem::path(directory) / "diftray.conf";
    auto write_config = [&](const char *text) { std::ofstream file(path); file << text; };
    write_config("general {\n font_size = 16\n border_size = 3\n}\n");
    setenv("DIFTRAYWM_CONFIG", path.c_str(), 1);
    setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
    setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent", 1);
    setenv("DIFTRAYWM_EXTENSION_PATH", "/nonexistent", 1);
    Compositor compositor;
    check(compositor.init(), "compositor initialization failed");
    check(compositor.apply_theme_css(":root { border-size: 0; background-color: #123; terminal-cursor-color: #abc; terminal-cursor-thickness: 1px; highlight-color: #def; }") == "theme applied",
          "shorthand colors and zero border were not applied");
    check(CompositorTestAccess::config(compositor).border_size == 0 &&
          CompositorTestAccess::style(compositor).background == 0xff112233 &&
          CompositorTestAccess::style(compositor).cursor == 0xffaabbcc &&
          CompositorTestAccess::style(compositor).highlight == 0xffddeeff,
          "CSS did not reach terminal renderer");
    write_config("general {\n font_size = 28\n border_size = 7\n theme = missing.css\n}\n");
    check(compositor.reload_config().starts_with("reload failed:"), "missing theme reload succeeded");
    check(CompositorTestAccess::config(compositor).font_size == 16 &&
          CompositorTestAccess::config(compositor).border_size == 0 &&
          CompositorTestAccess::style(compositor).cursor == 0xffaabbcc,
          "failed reload changed live configuration or terminal style");
    write_config("general {\n font_size = 28\n border_size = 7\n}\n");
    check(compositor.reload_config().starts_with("reloaded "), "valid reload failed");
    check(CompositorTestAccess::config(compositor).font_size == 28 &&
          CompositorTestAccess::config(compositor).border_size == 7,
          "valid reload did not commit configuration");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; result = EXIT_FAILURE;
  }
  std::filesystem::remove_all(directory);
  return result;
}
