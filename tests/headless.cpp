#include "compositor/Compositor.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"
#include "notelet/Notelet.hpp"
#include "nterm/NTerm.hpp"

#include <wayland-server-core.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

struct CompositorTestAccess {
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
};
static int inspect(void *userdata) {
  auto *run = static_cast<Run *>(userdata);
  if (++run->ticks == 1) {
    if (!CompositorTestAccess::check(*run->compositor)) {
      run->compositor->stop(); return 0;
    }
    run->compositor->open_notelet("desktop");
  } else if (CompositorTestAccess::notelets_done(*run->compositor)) {
    run->passed = true;
    run->compositor->stop(); return 0;
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
  const auto config = std::filesystem::path(__FILE__).parent_path().parent_path() / "diftray.conf";
  setenv("DIFTRAYWM_CONFIG", config.c_str(), 1);
  bool passed = false;
  {
    Compositor c;
    if (c.init()) {
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
