#include "compositor/Compositor.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"
#include "notelet/Notelet.hpp"
#include "nterm/NTerm.hpp"
#include "compositor/WaylandRuntime.h"

#include <wayland-server-core.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>

struct CompositorTestAccess {
  static bool animation_started(Compositor &c) {
    // Restart after backend initialization, which can outlast a short opening
    // animation on a busy machine. Sample deterministically on real scene nodes.
    c.reset_cell_animations();
    c.animate_cell(c.active_cell());
    if (!c.animations_.active()) return false;
    c.animations_.tick(0.05);
    for (const auto &cell : c.cells_) {
      const float opacity = diftray_cell_surface_opacity(cell->surface());
      if (opacity > 0 && opacity < 1) return true;
    }
    return false;
  }
  static bool animation_finished(Compositor &c) {
    if (c.animations_.active()) return false;
    for (const auto &cell : c.cells_)
      if (diftray_cell_surface_opacity(cell->surface()) != 1) return false;
    return true;
  }
  static bool check(Compositor &c) {
    if (c.outputs_.size() != 3) return false;
    for (auto &[name, output] : c.outputs_) {
      auto *view = output.ncursors[1];
      if (!view || !view->active_cell() || !view->active_cell()->surface()) return false;
      const auto box = view->active_cell()->box();
      if (box.x != output.geometry.x || box.width != output.geometry.width) return false;
    }
    return true;
  }
  static bool rotations(Compositor &c) {
    const auto one = c.outputs_.at("HEADLESS-1").geometry;
    const auto two = c.outputs_.at("HEADLESS-2").geometry;
    const auto three = c.outputs_.at("HEADLESS-3").geometry;
    if (one.rotation != 90 || one.width != 720 || one.height != 1280 || one.x != -720 ||
        two.rotation != 0 || two.scale != 2 || two.width != 640 || two.height != 360 ||
        three.rotation != 180) return false;
    c.focus_output("HEADLESS-2");
    auto *focused = c.active_cell();
    auto *rotated = c.outputs_.at("HEADLESS-1").ncursors[1]->active_cell();
    for (int angle : {0, 90, 180, 270}) {
      if (!c.command_bar().dispatch("output rotate HEADLESS-1 " + std::to_string(angle)) ||
          c.command_bar().status_line() != "configured output HEADLESS-1") return false;
      const auto &g = c.outputs_.at("HEADLESS-1").geometry;
      if (g.rotation != angle || g.width != (angle % 180 ? 720 : 1280) ||
          g.height != (angle % 180 ? 1280 : 720) || rotated->box().width != g.width ||
          c.active_cell() != focused || c.current_output() != "HEADLESS-2") return false;
    }
    c.command_bar().dispatch("output rotate HEADLESS-1 45");
    if (c.outputs_.at("HEADLESS-1").geometry.rotation != 270 ||
        c.command_bar().status_line().find("must be") == std::string::npos) return false;
    c.command_bar().dispatch("output scale HEADLESS-1 1.25");
    if (c.outputs_.at("HEADLESS-1").geometry.width != 576 || c.outputs_.at("HEADLESS-1").geometry.height != 1024) return false;
    c.command_bar().dispatch("output scale HEADLESS-1 2");
    const auto scaled = c.outputs_.at("HEADLESS-1").geometry;
    if (scaled.width != 360 || scaled.height != 640) return false;
    c.command_bar().dispatch("output position HEADLESS-1 -360 30");
    if (c.outputs_.at("HEADLESS-1").geometry.x != -360 || rotated->box().y <= 30) return false;
    c.command_bar().dispatch("output position HEADLESS-1 auto");
    if (c.monitor_config("HEADLESS-1").positioned) return false;
    // Backend API rejects invalid requests without changing committed state.
    diftray_output_config bad{45, 1.f, false, 0, 0};
    if (diftray_wayland_runtime_configure_output(c.wayland_runtime_, "HEADLESS-1", &bad)) return false;
    return c.outputs_.at("HEADLESS-1").geometry.rotation == 270 && check(c);
  }
  static bool notelet_updated(Compositor &c) {
    return c.notelet_cells_.begin()->second->frame().find("rotation=180 scale=2") != std::string::npos;
  }
  static bool notelets_done(Compositor &c) {
    if (c.notelet_cells_.size() != 1) return false;
    const auto &entry = *c.notelet_cells_.begin();
    return !entry.second->busy() && !entry.second->frame().empty() &&
           !entry.first->nterm()->running();
  }
};

struct Run {
  Compositor *compositor;
  wl_event_source *timer;
  int ticks = 0;
  bool passed = false;
  bool rotated_notelet = false;
};
static int inspect(void *userdata) {
  auto *run = static_cast<Run *>(userdata);
  if (++run->ticks == 1) {
    if (!CompositorTestAccess::check(*run->compositor) || !CompositorTestAccess::rotations(*run->compositor)) {
      run->compositor->stop(); return 0;
    }
    run->compositor->open_notelet("desktop");
  } else if (CompositorTestAccess::notelets_done(*run->compositor)) {
    if (!run->rotated_notelet) {
      run->compositor->command_bar().dispatch("output rotate HEADLESS-2 180");
      run->rotated_notelet = true;
    } else if (CompositorTestAccess::animation_finished(*run->compositor)) {
      run->passed = CompositorTestAccess::notelet_updated(*run->compositor);
      run->compositor->stop(); return 0;
    }
  }
  if (run->ticks > 100) { run->compositor->stop(); return 0; }
  wl_event_source_timer_update(run->timer, 20);
  return 0;
}
int main() {
  char directory[] = "/tmp/diftray-headless-XXXXXX";
  if (!mkdtemp(directory)) return EXIT_FAILURE;
  setenv("XDG_RUNTIME_DIR", directory, 1);
  setenv("WLR_BACKENDS", "headless", 1);
  setenv("WLR_HEADLESS_OUTPUTS", "3", 1);
  setenv("WLR_RENDERER", "pixman", 1);
  unsetenv("DIFTRAYWM_LOGIC_ONLY");
  const auto config = std::filesystem::path(directory) / "diftray.toml";
  const auto theme = std::filesystem::path(__FILE__).parent_path().parent_path() / "themes/default.css";
  std::ofstream(config) << "[general]\ntheme = '" << theme.string() << "'\n"
    "[[monitors]]\nname = '*'\nrotation = 180\n"
    "[[monitors]]\nname = 'HEADLESS-1'\nrotation = 90\nx = -720\ny = 0\n"
    "[[monitors]]\nname = 'HEADLESS-2'\nscale = 2.0\nx = 0\ny = 0\n";
  setenv("DIFTRAYWM_CONFIG", config.c_str(), 1);
  bool passed = false;
  {
    Compositor c;
    if (c.init() && CompositorTestAccess::animation_started(c)) {
      Run run{&c, nullptr};
      run.timer = wl_event_loop_add_timer(wl_display_get_event_loop(c.display()), inspect, &run);
      wl_event_source_timer_update(run.timer, 20);
      const int result = c.run();
      wl_event_source_remove(run.timer);
      passed = result == 0 && run.passed;
    }
    if (!passed) std::cerr << "headless integration failed: " << c.status_line() << '\n';
  }
  std::filesystem::remove_all(directory);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
