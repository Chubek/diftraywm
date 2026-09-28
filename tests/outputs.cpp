#include "compositor/Compositor.hpp"
#include "views/Cell.hpp"
#include "nterm/NTerm.hpp"
#include "views/NCursorView.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

struct CompositorTestAccess {
  static Cell *cell(Compositor &c) { return c.active_cell(); }
  static NCursorView *view(Compositor &c) { return c.active_ncursor_; }
  static size_t tabs(Compositor &c) { return c.ncursors_on_workspace(c.current_workspace_).size(); }
};

static void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
int main() {
  try {
    const auto config = std::filesystem::path(__FILE__).parent_path().parent_path() / "diftray.conf";
    setenv("DIFTRAYWM_CONFIG", config.c_str(), 1);
    setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
    Compositor c;
    check(c.init(), "initialization");
    auto *original = CompositorTestAccess::cell(c);
    c.synchronize_outputs({{"left", -1920, 0, 1920, 1080}, {"right", 0, 100, 1280, 720}});
    check(c.current_output() == "left", "initial focus migration");
    check(CompositorTestAccess::cell(c) == original, "primary cell retained");
    check(original->box().x == -1920 && original->box().width == 1920, "left geometry");
    check(c.focus_output("right") == "output right", "right focus");
    auto *right = CompositorTestAccess::cell(c);
    check(right != original && right->box().x == 0 && right->box().y > 100 && right->box().width == 1280,
          "distinct right cell and geometry");
    c.spawn_ncursor();
    check(CompositorTestAccess::tabs(c) == 2, "second right tab");
    c.focus_output("left");
    check(CompositorTestAccess::tabs(c) == 1 && CompositorTestAccess::cell(c) == original, "tabs local to monitor");
    c.promote_active_cell_to_tcursor();
    check(original->box().width == 1920, "tcursor uses own monitor");
    check(right->box().width == 1280, "tcursor leaves other monitor intact");
    c.restore_tcursor();
    c.switch_workspace(2);
    auto *workspace2 = CompositorTestAccess::cell(c);
    check(workspace2 != original, "new workspace cell");
    c.focus_output("right");
    check(CompositorTestAccess::tabs(c) == 1, "workspace populated on each monitor");
    c.switch_workspace(1);
    check(CompositorTestAccess::tabs(c) == 2, "workspace tabs restored");
    c.focus_output("left");
    check(CompositorTestAccess::cell(c) == original, "workspace focus restored per monitor");
    check(c.move_to_output("right").find("moved ncursor") == 0, "move command");
    check(CompositorTestAccess::cell(c) != original, "replacement on vacated output");
    c.focus_output("right");
    check(CompositorTestAccess::cell(c) == original && CompositorTestAccess::tabs(c) == 3, "moved tab selected");
    const auto target = CompositorTestAccess::view(c)->id();
    auto *moving_cell = CompositorTestAccess::cell(c);
    c.focus_output("left");
    const auto source = CompositorTestAccess::view(c)->id();
    check(c.move_cell(moving_cell->id(), source).find("moved cell") == 0, "cross-monitor cell move");
    check(CompositorTestAccess::cell(c) == moving_cell, "moved cell selected at destination");
    check(c.move_cell(moving_cell->id(), target).find("moved cell") == 0, "return cell");
    c.focus_output("right");
    c.synchronize_outputs({{"right", 0, 0, 1600, 900}});
    check(c.current_output() == "right" && CompositorTestAccess::tabs(c) == 4, "unplug migrates all tabs");
    c.synchronize_outputs({});
    c.synchronize_outputs({{"new", 0, 0, 1024, 768}});
    check(c.current_output() == "new" && CompositorTestAccess::tabs(c) == 4, "replug after no monitors");
    check(c.focus_output("missing") == "output not found: missing", "invalid output rejected");
    c.command_bar().dispatch("output list");
    check(c.command_bar().status_line().find("1024x768") != std::string::npos, "output list command");
    auto *individual = CompositorTestAccess::cell(c);
    check(c.set_shell_override("/bin/sh", CommandScope::CELL).find("set cell shell") == 0, "cell shell override");
    c.spawn_cell(false);
    auto *inherited = CompositorTestAccess::cell(c);
    check(c.set_shell_override("/bin/false", CommandScope::NCURSOR_GLOBAL).find("set global shell") == 0, "global shell override");
    check(individual->nterm()->shell_path() == "/bin/sh" && inherited->nterm()->shell_path() == "/bin/false",
          "individual shell survives global changes");
    c.spawn_cell(false);
    check(CompositorTestAccess::cell(c)->nterm()->shell_path() == "/bin/false", "new cell inherits ncursor shell");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n'; return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
