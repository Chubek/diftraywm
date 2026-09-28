#include "config/Config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

static void check(bool condition, const std::string &message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
  char dir[] = "/tmp/diftray-config-XXXXXX";
  check(mkdtemp(dir), "mkdtemp failed");
  const auto path = [&](const char *name, const char *source) {
    auto p = std::filesystem::path(dir) / name;
    std::ofstream(p) << source;
    return p.string();
  };
  std::string error;
  for (const auto &p : {
      path("settings.conf", "# general configuration\ngeneral {\n\n # comment inside section\n font_size = 21\n font = Test Font\n border_color = #12345678\n}\nterminal {\n shell = /bin/bash\n}\n"),
      path("settings.toml", "[general]\nfont_size = 21\nfont = 'Test Font'\nborder_color = '#12345678'\n[terminal]\nshell = '/bin/bash'\n"),
      path("settings.yaml", "general:\n  font_size: 21\n  font: Test Font\n  border_color: '#12345678'\nterminal:\n  shell: /bin/bash\n")}) {
    CompositorConfig c;
    check(load_compositor_config(p, c, error), error);
    check(c.font_size == 21 && c.font == "Test Font" && c.shell == "/bin/bash" && c.border_color[0] == 18.f/255, "format mismatch");
  }
  for (const auto &p : {
      path("bad.conf", "general {\n font = Changed\n font_size = bad\n}\n"),
      path("bad.toml", "[general]\nfont = 'Changed'\nfont_size = 2\n"),
      path("bad.yaml", "general:\n  font: Changed\n  font_size: 2\n"),
      path("unknown.yaml", "general:\n  typo: 2\n"),
      path("unknown.toml", "[general]\ntypo = 2\n"),
      path("unknown.conf", "general {\n typo = 2\n}\n"),
      path("duplicate.yaml", "general:\n  font: one\n  font: two\n"),
      path("duplicate.toml", "[general]\nfont = 'one'\n[terminal]\nfont = 'two'\n"),
      path("empty.yaml", "terminal:\n  shell: ''\n"),
      path("array.yaml", "general:\n  font: [one, two]\n"),
      path("array.toml", "[general]\nfont = ['one', 'two']\n"),
      std::string(dir) + "/missing.yaml"}) {
    CompositorConfig c;
    check(!load_compositor_config(p, c, error), "accepted invalid config: " + p);
    check(!error.empty() && c.font == "monospace" && c.font_size == 14, "failed load changed config");
  }
  std::filesystem::remove_all(dir);
}
