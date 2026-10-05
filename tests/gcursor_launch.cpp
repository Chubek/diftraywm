#include "compositor/Compositor.hpp"
#include "views/GCursorView.hpp"
#include "views/NCursorView.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

struct CompositorTestAccess {
  static int launched_pid(Compositor &c) { return c.launched_pids_.back(); }
  static NCursorView *owner(Compositor &c) { return c.active_ncursor_; }
  static GCursorView *map(Compositor &c, int pid, wlr_xdg_toplevel *surface) {
    c.on_new_toplevel(surface, pid);
    return c.gcursors_.back().get();
  }
  static void destroy(Compositor &c, wlr_xdg_toplevel *surface) {
    c.on_toplevel_destroy(surface);
  }
  static bool focused(Compositor &c, GCursorView *view) { return c.active_view_ == view; }
};

static void check(bool ok, const char *message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  setenv("DIFTRAYWM_CONFIG", (root / "diftray.conf").c_str(), 1);
  setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  Compositor c;
  check(c.init(), "compositor initialization failed");
  auto *launch_owner = CompositorTestAccess::owner(c);
  check(c.launch_program("exec sleep 30").starts_with("launched"), "launch failed");
  const int pid = CompositorTestAccess::launched_pid(c);
  struct Child {
    int pid;
    ~Child() { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); }
  } child{pid};
  bool ready = false;
  for (int attempt = 0; attempt < 100 && !ready; ++attempt) {
    std::ifstream env("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(env)), {});
    ready = content.find("DIFTRAYWM_LAUNCH_OWNER=") != std::string::npos;
    if (!ready) usleep(10000);
  }
  check(ready, "launch ownership was not inherited by exec");
  c.switch_workspace(2);
  // Opaque identities are sufficient in logic-only mode; no wlroots object is accessed.
  int first, second;
  auto *surface = reinterpret_cast<wlr_xdg_toplevel *>(&first);
  auto *view = CompositorTestAccess::map(c, pid, surface);
  check(view->owner_ncursor() == launch_owner && !view->owner_cell(),
        "delayed launcher window acquired the current cell/workspace");
  check(view->workspace() == 1 && c.current_workspace() == 2 &&
        !CompositorTestAccess::focused(c, view), "background window stole focus");
  c.dock_cursor(view->word_id());
  check(view->docked(), "background docking failed");
  c.restore_cursor(view->word_id());
  check(!view->docked() && c.current_workspace() == 1 &&
        CompositorTestAccess::focused(c, view), "restore did not focus the original workspace");
  c.switch_workspace(3);
  auto *surface2 = reinterpret_cast<wlr_xdg_toplevel *>(&second);
  auto *another = CompositorTestAccess::map(c, pid, surface2);
  check(another->owner_ncursor() == launch_owner && another->workspace() == 1 &&
        c.current_workspace() == 3, "additional surface lost launch ownership");
  CompositorTestAccess::destroy(c, surface);
  CompositorTestAccess::destroy(c, surface2);
  check(c.list_cursor_ids() == "no cursors", "destroy left dangling cursor identities");
  const auto dictionary = std::filesystem::temp_directory_path() /
      ("diftray-dictionary-" + std::to_string(getpid()));
  std::ofstream(dictionary) << "amber\n";
  setenv("DIFTRAYWM_WORD_POOL", dictionary.c_str(), 1);
  GCursorView::reload_word_pool();
  GCursorView amber;
  check(amber.word_id() == "amber", "new word pool was not loaded");
  std::ofstream(dictionary) << "willow\n";
  GCursorView::reload_word_pool();
  GCursorView willow;
  GCursorView duplicate(nullptr, "amber");
  check(amber.word_id() == "amber" && willow.word_id() == "willow" &&
        duplicate.word_id() != "amber" && duplicate.word_id() != "willow",
        "dictionary reload lost existing IDs or uniqueness");
  std::filesystem::remove(dictionary);
}
