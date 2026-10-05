#include "compositor/Compositor.hpp"
#include "config/Config.hpp"
#include "config/ConfigProgram.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"

#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

constexpr uint32_t kPressed = WL_KEYBOARD_KEY_STATE_PRESSED;
constexpr uint32_t kLogo = WLR_MODIFIER_LOGO;
constexpr uint32_t kCtrl = WLR_MODIFIER_CTRL;

// The compositor matches chords by key code, because the keymap INI spells a
// chord as an evdev code plus modifier bits, matching wlroots events. Note that XKB_KEY_q is the
// *keysym* 0x71, not the key code, so the code has to come from
// input-event-codes. The tests run without a seat, so it must be passed in.
constexpr uint32_t q_keycode = KEY_Q;
constexpr uint32_t x_keycode = KEY_X;

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

std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> out;
  std::string current;
  for (char ch : text) {
    if (ch == '\n') {
      out.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  out.push_back(current);
  return out;
}

}  // namespace

struct CompositorTestAccess {
  static wlr_box cell_box(Compositor &c) { return c.active_cell()->box(); }
  static bool bar_open(Compositor &c) { return c.command_bar_open_; }
  static bool prefix_armed(Compositor &c) { return c.meta_prefix_pending_; }
  static std::size_t panes(Compositor &c) { return c.ncursor_view()->pane_count(); }
  static std::size_t stacks(Compositor &c) { return c.ncursor_view()->cell_stacks().size(); }
  static std::string active_id(Compositor &c) { return c.active_cell()->id(); }
  static std::size_t ncursor_count(Compositor &c) {
    std::size_t count = 0;
    for (const auto &line : split_lines(c.list_views()))
      if (!line.empty()) ++count;
    return count;
  }
};

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  Compositor compositor;
  init_compositor(compositor, (root / "diftray.conf").string());

  // 1. Opening `:` must not resize the terminal: the command bar is an
  // overlay, so the prompt and half-typed input stay where they are.
  {
    check(compositor.handle_key(XKB_KEY_colon, 0, kPressed, ':'),
          "colon not handled");
    check(CompositorTestAccess::bar_open(compositor), "command bar did not open");
    const wlr_box opened = CompositorTestAccess::cell_box(compositor);
    check(opened.height > 0, "cell has no geometry");
    check(compositor.handle_key(XKB_KEY_Escape, 0, kPressed, 0),
          "escape not handled");
    check(!CompositorTestAccess::bar_open(compositor), "command bar did not close");
    check(CompositorTestAccess::cell_box(compositor).height == opened.height,
          "closing the command bar resized the terminal");
    check(compositor.handle_key(XKB_KEY_colon, 0, kPressed, ':'),
          "colon not handled again");
    check(CompositorTestAccess::cell_box(compositor).height == opened.height,
          "opening `:` resized the terminal");
    check(compositor.handle_key(XKB_KEY_Escape, 0, kPressed, 0),
          "escape not handled again");
  }

  // 2. Builtin multiplexer commands.
  check(dispatch(compositor, "mux list").rfind("* ", 0) == 0, "mux list initial");
  check(CompositorTestAccess::panes(compositor) == 1, "initial pane count");
  check(dispatch(compositor, "mux split horizontal").find("split cell ") == 0,
        "mux split horizontal");
  check(CompositorTestAccess::panes(compositor) == 2, "horizontal split panes");
  check(CompositorTestAccess::stacks(compositor) == 1, "horizontal split stacks");
  check(dispatch(compositor, "mux split vertical").find("split cell ") == 0,
        "mux split vertical");
  check(CompositorTestAccess::stacks(compositor) == 2, "vertical split stacks");
  check(CompositorTestAccess::panes(compositor) == 3, "vertical split panes");
  check(dispatch(compositor, "mux focus left").find("focused cell ") == 0,
        "mux focus left");
  check(dispatch(compositor, "mux focus right").find("focused cell ") == 0,
        "mux focus right");
  check(dispatch(compositor, "mux focus down").find("no pane below") == 0,
        "mux focus down from single-cell stack");
  check(dispatch(compositor, "mux focus left").find("focused cell ") == 0,
        "mux focus left again");
  check(dispatch(compositor, "mux focus down").find("focused cell ") == 0,
        "mux focus down within stack");
  check(dispatch(compositor, "mux focus up").find("focused cell ") == 0,
        "mux focus up within stack");
  const std::string cycling = CompositorTestAccess::active_id(compositor);
  check(dispatch(compositor, "mux focus next").find("focused cell ") == 0,
        "mux focus next");
  check(dispatch(compositor, "mux focus prev").find("focused cell ") == 0,
        "mux focus prev");
  check(CompositorTestAccess::active_id(compositor) == cycling, "next/prev cycle");
  check(dispatch(compositor, "mux split").find("requires horizontal or vertical") !=
            std::string::npos,
        "mux split usage");
  check(dispatch(compositor, "mux focus sideways").find("requires next") !=
            std::string::npos,
        "mux focus usage");
  check(dispatch(compositor, "mux dance").find("mux supports:") == 0, "mux usage");
  {
    const auto list = dispatch(compositor, "mux list");
    std::size_t lines = 0;
    for (char ch : list) lines += (ch == '\n');
    check(lines + 1 == CompositorTestAccess::panes(compositor) &&
              list.find("* ") != std::string::npos,
          "mux list panes");
  }

  // Multiplexer shortcuts.
  {
    const std::size_t panes = CompositorTestAccess::panes(compositor);
    check(compositor.handle_key(XKB_KEY_s, kLogo, kPressed, 's'), "Meta+S");
    check(CompositorTestAccess::panes(compositor) == panes + 1, "Meta+S split");
    check(compositor.handle_key(XKB_KEY_v, kLogo, kPressed, 'v'), "Meta+V");
    check(CompositorTestAccess::stacks(compositor) >= 2, "Meta+V stack");
    const std::string focused = CompositorTestAccess::active_id(compositor);
    check(compositor.handle_key(XKB_KEY_o, kLogo, kPressed, 'o'), "Meta+O");
    check(compositor.handle_key(XKB_KEY_p, kLogo, kPressed, 'p'), "Meta+P");
    check(CompositorTestAccess::active_id(compositor) == focused, "Meta+O/P cycle");
    check(compositor.handle_key(XKB_KEY_Up, kLogo | kCtrl, kPressed, 0),
          "Meta+Ctrl+Up");
  }

  // Zoom toggle and kill.
  check(dispatch(compositor, "mux zoom").find("promoted cell ") == 0, "mux zoom in");
  check(dispatch(compositor, "mux zoom") == "restored tcursor", "mux zoom out");
  check(compositor.handle_key(XKB_KEY_z, kLogo, kPressed, 'z'), "Meta+Z");
  check(dispatch(compositor, "mux zoom") == "restored tcursor", "Meta+Z zoom");
  while (CompositorTestAccess::panes(compositor) > 1) {
    check(dispatch(compositor, "mux kill") == "removed cell", "mux kill");
  }
  check(dispatch(compositor, "mux kill") ==
            "kill failed: cannot remove the last cell",
        "mux kill guard");

  // 3. The Meta prefix, which now comes from the keymap INI, and Composite().
  check(dispatch(compositor, "config eval Composite(\"Meta\", \"Shift\")") ==
            "Meta+Shift",
        "Composite() join");
  check(dispatch(compositor, "config eval Composite(\"Ctrl\", \"Q\")") == "Ctrl+Q",
        "Composite() chord spelling");
  // The shipped keymap.ini sets [init] prefix = <C-q>, and the keymap is the
  // authority for the prefix chord, so the default chord still arms Meta.
  {
    const std::size_t tabs = CompositorTestAccess::ncursor_count(compositor);
    check(compositor.handle_key(XKB_KEY_q, kCtrl, kPressed, 'q', q_keycode), "prefix chord");
    check(CompositorTestAccess::prefix_armed(compositor), "prefix not armed");
    check(compositor.handle_key(XKB_KEY_n, 0, kPressed, 0), "prefixed key");
    check(!CompositorTestAccess::prefix_armed(compositor), "prefix not consumed");
    check(CompositorTestAccess::ncursor_count(compositor) == tabs + 1,
          "Ctrl+Q N did not act as Meta+N");
    check(compositor.handle_key(XKB_KEY_n, kLogo, kPressed, 0), "Meta+N");
    check(CompositorTestAccess::ncursor_count(compositor) == tabs + 2,
          "Logo Meta stopped working");
  }
  {
    const std::size_t tabs = CompositorTestAccess::ncursor_count(compositor);
    check(compositor.handle_key(XKB_KEY_q, kCtrl, kPressed, 'q', q_keycode), "prefix re-arm");
    check(compositor.handle_key(XKB_KEY_Escape, 0, kPressed, 0), "prefix escape");
    check(!CompositorTestAccess::prefix_armed(compositor), "prefix not cancelled");
    check(compositor.handle_key(XKB_KEY_n, 0, kPressed, 0), "plain n");
    check(CompositorTestAccess::ncursor_count(compositor) == tabs,
          "cancelled prefix still acted as Meta");
  }
  {
    std::string error;
    check(!ConfigProgram::compile("let Composite = 1;", "reserved", error),
          "Composite not reserved");
  }

  // An explicit Ctrl+Q binding wins over the default prefix.
  {
    char dir[] = "/tmp/diftray-mux-XXXXXX";
    check(mkdtemp(dir) != nullptr, "mkdtemp failed");
    const auto path = std::filesystem::path(dir) / "settings.conf";
    {
      std::ofstream out(path);
      out << "program {\n"
             "  bind(\"Ctrl\", keysym(\"q\"), \"workspace 5\");\n"
             "}\n";
    }
    Compositor other;
    init_compositor(other, path.string());
    check(other.handle_key(XKB_KEY_q, kCtrl, kPressed, 'q', q_keycode), "bound Ctrl+Q");
    check(other.current_workspace() == 5, "explicit Ctrl+Q binding ignored");
    check(!CompositorTestAccess::prefix_armed(other), "binding armed prefix");
    std::filesystem::remove_all(dir);
  }

  // A custom `prefix` variable redefines the chord via Composite().
  {
    char dir[] = "/tmp/diftray-prefix-XXXXXX";
    check(mkdtemp(dir) != nullptr, "mkdtemp failed");
    const auto path = std::filesystem::path(dir) / "settings.conf";
    {
      std::ofstream out(path);
      out << "program {\n"
             "  let prefix = Composite(\"Ctrl\", \"X\");\n"
             "}\n";
    }
    Compositor other;
    init_compositor(other, path.string());
    check(other.handle_key(XKB_KEY_q, kCtrl, kPressed, 'q', q_keycode), "old chord");
    check(!CompositorTestAccess::prefix_armed(other), "old chord still armed");
    const std::size_t tabs = CompositorTestAccess::ncursor_count(other);
    check(other.handle_key(XKB_KEY_x, kCtrl, kPressed, 'x', x_keycode), "new chord");
    check(CompositorTestAccess::prefix_armed(other), "custom prefix not armed");
    check(other.handle_key(XKB_KEY_n, 0, kPressed, 0), "custom prefixed key");
    check(CompositorTestAccess::ncursor_count(other) == tabs + 1,
          "custom prefix did not act as Meta");
    std::filesystem::remove_all(dir);
  }

  // 4. Theming commands. The shipped diftray.conf preloads themes/default.css.
  check(dispatch(compositor, "theme") == "theme supports: load <path>, show",
        "theme usage");
  {
    const auto shown = dispatch(compositor, "theme show");
    check(shown.find("border-color: #59a6ff") != std::string::npos &&
              shown.find("css:") == std::string::npos,
          "theme show contents: " + shown);
  }
  {
    char dir[] = "/tmp/diftray-theme-XXXXXX";
    check(mkdtemp(dir) != nullptr, "mkdtemp failed");
    const auto path = std::filesystem::path(dir) / "bare.conf";
    {
      std::ofstream out(path);
      out << "general {\n}\n";
    }
    Compositor bare;
    init_compositor(bare, path.string());
    auto show = dispatch(bare, "theme show");
    check(show == "no theme properties", "theme show empty: " + show);
    std::filesystem::remove_all(dir);
  }
  check(dispatch(compositor, "theme load /nonexistent-theme.css").find(
              "cannot open theme") == 0,
        "theme load missing");
  check(dispatch(compositor, "theme load " + (root / "themes" / "default.css").string()) ==
            "theme applied",
        "theme load file");
  check(dispatch(compositor, "set theme :root { border-size: 2px; }") ==
            "theme applied",
        "set theme css");

  std::cout << "mux, prefix, theme checks passed\n";
  return EXIT_SUCCESS;
}
