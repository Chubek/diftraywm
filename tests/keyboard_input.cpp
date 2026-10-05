#include "compositor/Compositor.hpp"
#include "input/TextInput.hpp"
#include "nterm/NTerm.hpp"
#include "views/Cell.hpp"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

struct CompositorTestAccess {
  static void event_loop(Compositor &c) { c.display_.reset(wl_display_create()); }
  static Cell *cell(Compositor &c) { return c.active_cell(); }
  static void bar(Compositor &c) { c.open_command_bar(CommandScope::CELL, ""); }
  static bool repeating(Compositor &c) { return c.repeating_key_.code != 0; }
  static void event(Compositor &c, uint32_t sym, uint32_t unicode, uint32_t code,
                    uint32_t state = WL_KEYBOARD_KEY_STATE_PRESSED, uint32_t mods = 0) {
    c.terminal_key_received(&c, sym, mods, state, unicode, 0, code);
  }
  static void close_bar(Compositor &c) { c.close_command_bar(); }
  static bool help_match(Compositor &c, uint32_t unicode) { return c.help_key_matches("q", 0, unicode); }
  static void search(Compositor &c, bool open) { c.help_search_open_ = open; c.cancel_key_repeat(); }
  static std::string search_text(Compositor &c) { return c.help_search_input_; }
};
struct NTermTestAccess {
  static void writer(NTerm &term, int fd) { term.stop(); term.master_fd_ = fd; }
};
static void check(bool ok, const char *message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void pump(Compositor &c) {
  check(wl_event_loop_dispatch(wl_display_get_event_loop(c.display()), 50) == 0,
        "event loop dispatch failed");
}
int main() {
  char temporary[] = "/tmp/diftray-key-input-XXXXXX";
  const auto made = mkdtemp(temporary);
  check(made, "temporary directory failed");
  const std::filesystem::path dir(made);
  std::ofstream(dir / "diftray.conf") << "general {\n keymap = keymap.ini\n}\n";
  std::ofstream(dir / "keymap.ini") << "[diftray]\nrepeat_delay = 1\nrepeat_rate = 100\n"
      "[meta]\nprefix = <C-q>\n[default]\n<C-y> = Diftray(workspace 4)\n";
  setenv("DIFTRAYWM_CONFIG", (dir / "diftray.conf").c_str(), 1);
  setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  setenv("DIFTRAYWM_EXTENSION_PATH", "/nonexistent", 1);
  setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent", 1);
  Compositor c;
  check(c.init(), "compositor initialization failed");
  CompositorTestAccess::event(c, XKB_KEY_y, 0, KEY_Y,
      WL_KEYBOARD_KEY_STATE_PRESSED, WLR_MODIFIER_CTRL);
  check(c.current_workspace() == 4 && !CompositorTestAccess::repeating(c),
        "raw Linux code did not match its INI binding or repeated an action");
  CompositorTestAccess::event(c, XKB_KEY_q, 0, KEY_Q,
      WL_KEYBOARD_KEY_STATE_PRESSED, WLR_MODIFIER_CTRL);
  CompositorTestAccess::event(c, XKB_KEY_3, '3', KEY_3);
  check(c.current_workspace() == 3, "raw Linux code did not arm the Meta prefix");
  CompositorTestAccess::event_loop(c);
  check(c.display(), "Wayland event loop initialization failed");
  check(CompositorTestAccess::help_match(c, 'q') &&
        !CompositorTestAccess::help_match(c, 0x171), "Unicode aliased an ASCII pager binding");
  CompositorTestAccess::search(c, true);
  CompositorTestAccess::event(c, XKB_KEY_NoSymbol, 0x3bb, KEY_A);
  CompositorTestAccess::event(c, XKB_KEY_NoSymbol, 0x1f642, KEY_B);
  CompositorTestAccess::event(c, XKB_KEY_BackSpace, 0, KEY_BACKSPACE);
  check(CompositorTestAccess::search_text(c) == "λ", "help search split or dropped Unicode");
  CompositorTestAccess::search(c, false);

  CompositorTestAccess::bar(c);
  // Non-ASCII input and backspace must preserve valid UTF-8.
  CompositorTestAccess::event(c, XKB_KEY_NoSymbol, 0x3bb, KEY_A);
  CompositorTestAccess::event(c, XKB_KEY_NoSymbol, 0x1f642, KEY_B);
  check(c.command_bar().input_buffer == "λ🙂", "command bar dropped Unicode");
  CompositorTestAccess::event(c, XKB_KEY_BackSpace, 0, KEY_BACKSPACE);
  check(c.command_bar().input_buffer == "λ", "backspace split a UTF-8 character");
  CompositorTestAccess::event(c, XKB_KEY_BackSpace, 0, KEY_BACKSPACE,
      WL_KEYBOARD_KEY_STATE_RELEASED);
  c.command_bar().input_buffer.clear();
  CompositorTestAccess::event(c, XKB_KEY_x, 'x', KEY_X);
  for (int i = 0; i < 10 && c.command_bar().input_buffer.size() == 1; ++i) pump(c);
  check(c.command_bar().input_buffer.size() > 1, "held editor key did not repeat");
  CompositorTestAccess::event(c, XKB_KEY_x, 'x', KEY_X, WL_KEYBOARD_KEY_STATE_RELEASED);
  const auto count = c.command_bar().input_buffer.size();
  pump(c);
  check(c.command_bar().input_buffer.size() == count, "editor key repeated after release");
  CompositorTestAccess::close_bar(c);

  int pipes[2];
  check(pipe2(pipes, O_CLOEXEC | O_NONBLOCK) == 0, "pipe failed");
  auto *term = CompositorTestAccess::cell(c)->nterm();
  NTermTestAccess::writer(*term, pipes[1]);
  CompositorTestAccess::event(c, XKB_KEY_a, 'a', KEY_A);
  check(CompositorTestAccess::repeating(c), "terminal key did not arm repeat");
  char bytes[128];
  std::string received;
  for (int attempt = 0; attempt < 10 && received.size() < 2; ++attempt) {
    pump(c);
    const auto count = read(pipes[0], bytes, sizeof(bytes));
    if (count > 0) received.append(bytes, static_cast<std::size_t>(count));
  }
  check(received.size() >= 2 && received.find_first_not_of('a') == std::string::npos,
        "held terminal key did not repeat");
  // A workspace change cannot redirect a held key into its new terminal.
  c.switch_workspace(4);
  pump(c);
  check(!CompositorTestAccess::repeating(c), "workspace change left repeat armed");
  term->stop();
  close(pipes[0]);

  CompositorTestAccess::bar(c);
  CompositorTestAccess::event(c, XKB_KEY_x, 'x', KEY_X);
  std::ofstream(dir / "keymap.ini") << "[diftray]\nrepeat_rate = 0\n";
  c.command_bar().dispatch("keymap reload");
  check(!CompositorTestAccess::repeating(c), "keymap reload did not stop a held key");
  check(c.keymap_info().find("repeat: 0 Hz") != std::string::npos,
        "repeat-only keymap was reported as unloaded");
  c.command_bar().input_buffer.clear();
  CompositorTestAccess::event(c, XKB_KEY_z, 'z', KEY_Z);
  pump(c);
  check(c.command_bar().input_buffer == "z" && !CompositorTestAccess::repeating(c),
        "repeat_rate = 0 did not disable repeat");

  Keymap parsed;
  std::string error;
  check(parse_keymap("[diftray]\nrepeat_rate = 0\nrepeat_delay = 30\n", "repeat.ini", parsed, error) &&
        parsed.repeat_rate == 0 && parsed.repeat_delay == 30, "repeat-only INI failed");
  for (const auto *bad : {"repeat_rate = -1", "repeat_rate = 101", "repeat_delay = 5001",
                          "repeat_delay = 1.5", "repeat_rate = many"}) {
    check(!parse_keymap(std::string("[diftray]\n") + bad + "\n", "bad.ini", parsed, error) &&
          error.find(":2:") != std::string::npos, "invalid repeat setting lacked a line error");
  }
  std::string text(16383, 'x');
  check(!text_input::append(text, 0x1f642) && text.size() == 16383,
        "editor size limit split UTF-8 input");
  check(!text_input::append(text, 0xd800), "editor accepted a Unicode surrogate");
  std::filesystem::remove_all(dir);
}
