// The compositor's half of the keymap subsystem: a keymap INI actually applied
// to real key events.
//
// The INI parser and the evdev mapping are covered without a compositor in
// tests/keymap.cpp. What that cannot cover is the wiring -- that a profile
// binding reaches the Command Bar, that Ignore() swallows a key before the
// built-in bindings see it, that the [init] chord switches profiles, and that a
// binding from the configuration program still wins over a profile. That is
// what this file is for.
//
// The compositor matches chords by XKB key code, because the INI spells a chord
// as an evdev code plus modifier bits. These tests run without a seat, so the
// key code has to be passed in, and XKB_KEY_q is the keysym 0x71 rather than the
// code, so the code comes from input-event-codes.

#include "compositor/Compositor.hpp"
#include "keymap/Keymap.hpp"

#include <linux/input-event-codes.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

constexpr uint32_t kPressed = WL_KEYBOARD_KEY_STATE_PRESSED;
constexpr uint32_t kLogo = WLR_MODIFIER_LOGO;
constexpr uint32_t kCtrl = WLR_MODIFIER_CTRL;
constexpr uint32_t kXkbOffset = 8;
constexpr uint32_t q_code = KEY_Q + kXkbOffset;
constexpr uint32_t g_code = KEY_G + kXkbOffset;
constexpr uint32_t n_code = KEY_N + kXkbOffset;
constexpr uint32_t x_code = KEY_X + kXkbOffset;
constexpr uint32_t a_code = KEY_A + kXkbOffset;
constexpr uint32_t y_code = KEY_Y + kXkbOffset;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string &message) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::string dispatch(Compositor &c, const std::string &command) {
  c.command_bar().dispatch(command);
  return c.command_bar().status_line();
}

// Writes a config and a keymap into a fresh directory and returns the config
// path. Each case needs its own pair so a keymap cannot leak into the next.
struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path config;

  Fixture() {
    char tmpl[] = "/tmp/diftray-keymap-XXXXXX";
    const char *made = ::mkdtemp(tmpl);
    check(made != nullptr, "mkdtemp failed");
    dir = made ? made : "/tmp/diftray-keymap-fallback";
    std::filesystem::create_directories(dir);
    config = dir / "diftray.conf";
    std::ofstream(config) << "general {\n  keymap = keymap.ini\n}\n"
                             "terminal {\n  shell = libshell\n}\n";
  }
  ~Fixture() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  void keymap(const std::string &body) {
    std::ofstream(dir / "keymap.ini") << body;
  }
  void init(Compositor &c) {
    ::setenv("DIFTRAYWM_CONFIG", config.string().c_str(), 1);
    ::setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
    ::setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent", 1);
    ::setenv("DIFTRAYWM_EXTENSION_PATH", "/nonexistent", 1);
    check(c.init(), "init failed: " + c.status_line());
  }
};

// 1. A profile can redefine a built-in binding, and Ignore() can disable one.
void test_profile_overrides_builtin() {
  Fixture fixture;
  fixture.keymap(
      "[meta]\nprefix = <C-q>\n"
      "[default]\n"
      "<C-n> = Diftray(workspace 4)\n"     // would otherwise open an NCursor
      "<C-x> = Ignore()\n");               // would otherwise type
  Compositor c;
  fixture.init(c);
  check(c.current_workspace() == 1, "starts on workspace 1");

  // Diftray() dispatches a real Command Bar command.
  c.handle_key(XKB_KEY_n, kCtrl, kPressed, 0, n_code);
  check(c.current_workspace() == 4,
        "a Diftray() binding did not reach the Command Bar, workspace is " +
            std::to_string(c.current_workspace()));

  // Ignore() swallows the key. Ctrl+x has no built-in binding, so the check is
  // that it did not type into the cell: a swallowed key leaves the terminal
  // untouched, and handle_key still reports the event as handled.
  check(c.handle_key(XKB_KEY_x, kCtrl, kPressed, 'x', x_code),
        "an Ignore() binding did not consume the key");
}

// 2. The [init] chord switches profiles, and only that profile's bindings apply.
void test_profile_switching() {
  Fixture fixture;
  fixture.keymap(
      "[init]\nprefix = <C-g>\naction = Trigger(alt)\n"
      "[default]\n<C-y> = Diftray(workspace 2)\n"
      "[alt]\n<C-y> = Diftray(workspace 7)\n<C-n> = Ignore()\n");
  Compositor c;
  fixture.init(c);
  check(c.current_workspace() == 1, "starts on workspace 1");

  // In `default`, <C-y> goes to workspace 2.
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 2, "default profile binding did not fire");

  // The [init] chord is swallowed and switches to `alt`. It must not also
  // trigger the built-in Ctrl+G bindings or reach the terminal.
  c.handle_key(XKB_KEY_g, kCtrl, kPressed, 'g', g_code);
  check(c.keymap().profiles.size() == 2, "both profiles were parsed");
  const std::string shown = c.keymap_info();
  check(shown.find("active profile: alt") != std::string::npos,
        "the [init] chord did not switch profile: " + shown);

  // In `alt`, the same chord means something else.
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 7,
        "the alt profile binding did not fire, workspace is " +
            std::to_string(c.current_workspace()));

  // And an Ignore() in `alt` beats the built-in Ctrl+N.
  c.handle_key(XKB_KEY_n, kCtrl, kPressed, 0, n_code);
  check(c.current_workspace() == 7,
        "an Ignore() binding in the alt profile let Ctrl+N through");

  // keymap reset returns to default without needing a bound exit.
  c.keymap_reset_profile();
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 2, "keymap reset did not restore default");
}

// 3. A broken keymap is reported but does not stop the session or the keys.
void test_broken_keymap_is_not_fatal() {
  Fixture fixture;
  fixture.keymap("[default]\n<C-n> = ThisIsNotAnAction()\n");
  Compositor c;
  fixture.init(c);
  check(!c.keymap_error().empty(),
        "a broken keymap was accepted silently");
  const std::string shown = c.keymap_info();
  check(shown.find("built-in keys in force") != std::string::npos,
        "a broken keymap did not report that the built-in keys are in force: " +
            shown);
  // The built-in Meta+N still works, which is the point: a typo in a keymap
  // must not leave the user without a way to open a view.
  c.handle_key(XKB_KEY_n, kLogo, kPressed, 0, n_code);
  check(c.keymap_error().empty() == false,
        "the keymap error was lost");
}

// 4. A keymap that names a profile which does not exist is refused outright.
void test_keymap_validation() {
  Fixture fixture;
  fixture.keymap("[default]\n<C-n> = Trigger(nowhere)\n");
  Compositor c;
  fixture.init(c);
  check(!c.keymap_error().empty(), "a Trigger to a missing profile was accepted");
  check(c.keymap_error().find("unknown profile") != std::string::npos,
        "the error names the missing profile: " + c.keymap_error());
}

// 5. `keymap check` validates a file without adopting it, and `keymap reload`
// adopts one.
void test_check_and_reload() {
  Fixture fixture;
  fixture.keymap("[default]\n<C-y> = Diftray(workspace 3)\n");
  Compositor c;
  fixture.init(c);
  check(c.keymap_error().empty(), "the fixture keymap did not load");
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 3, "the loaded binding did not fire");

  // A bad candidate is reported without disturbing what is loaded.
  const auto bad = fixture.dir / "bad.ini";
  std::ofstream(bad) << "[default]\n<C-y> = Nope()\n";
  const std::string checked = dispatch(c, "keymap check " + bad.string());
  check(checked.find("keymap check failed") != std::string::npos,
        "keymap check accepted a broken file: " + checked);
  // The still-loaded binding sends us to workspace 3 again, which is how we
  // know `check` did not adopt the candidate.
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 3,
        "keymap check changed the loaded keymap, workspace is " +
            std::to_string(c.current_workspace()));

  // A good candidate is validated, and reload then adopts it.
  const auto good = fixture.dir / "good.ini";
  std::ofstream(good) << "[default]\n<C-y> = Diftray(workspace 6)\n";
  const std::string valid = dispatch(c, "keymap check " + good.string());
  check(valid.find("is valid") != std::string::npos,
        "keymap check rejected a good file: " + valid);

  fixture.keymap("[default]\n<C-y> = Diftray(workspace 8)\n");
  const std::string reloaded = dispatch(c, "keymap reload");
  check(reloaded.find("reloaded") != std::string::npos,
        "keymap reload failed: " + reloaded);
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 8, "the reloaded binding did not fire");

  fixture.keymap("[default]\n<C-y> = Nope()\n");
  const std::string failed = dispatch(c, "keymap reload");
  check(failed.find("failed") != std::string::npos, "invalid reload was accepted");
  c.switch_workspace(2);
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 8, "failed reload discarded the working keymap");
  check(!c.keymap_error().empty(), "failed reload lost its diagnostic");
}

// 6. The help pager keys come from the keymap's [diftray] section.
void test_help_keys_from_keymap() {
  Fixture fixture;
  fixture.keymap("[default]\n<C-x> = Ignore()\n[diftray]\nhelp_key_close = Q\n");
  Compositor c;
  fixture.init(c);
  check(c.keymap().help_key_close == "Q",
        "the [diftray] help key was not read: " + c.keymap().help_key_close);
  // The defaults survive when the section is absent.
  Fixture plain;
  plain.keymap("[default]\n<C-x> = Ignore()\n");
  Compositor other;
  plain.init(other);
  check(other.keymap().help_key_close == "q",
        "the default help key was lost: " + other.keymap().help_key_close);
}

// 7. A bind() in the configuration program beats a keymap profile, because a
// binding the user wrote down deliberately is more specific than a profile.
void test_program_binding_beats_profile() {
  char tmpl[] = "/tmp/diftray-keymap-bind-XXXXXX";
  const char *made = ::mkdtemp(tmpl);
  check(made != nullptr, "mkdtemp failed");
  const std::filesystem::path dir(made ? made : "/tmp/diftray-keymap-bind-fallback");
  const auto config = dir / "diftray.conf";
  std::filesystem::create_directories(dir);
  std::ofstream(config)
      << "program {\n"
         "  bind(\"Ctrl\", keysym(\"y\"), \"workspace 5\");\n"
         "}\n";
  std::ofstream(dir / "keymap.ini")
      << "[default]\n<C-y> = Diftray(workspace 9)\n";
  Compositor c;
  ::setenv("DIFTRAYWM_CONFIG", config.string().c_str(), 1);
  ::setenv("DIFTRAYWM_LOGIC_ONLY", "1", 1);
  ::setenv("DIFTRAYWM_PLUGIN_PATH", "/nonexistent", 1);
  ::setenv("DIFTRAYWM_EXTENSION_PATH", "/nonexistent", 1);
  check(c.init(), "init failed: " + c.status_line());
  c.handle_key(XKB_KEY_y, kCtrl, kPressed, 0, y_code);
  check(c.current_workspace() == 5,
        "the program binding did not win over the keymap profile, workspace is " +
            std::to_string(c.current_workspace()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// 8. The shipped keymap.ini is valid and describes the compositor's own keys.
void test_shipped_keymap() {
  const auto shipped = std::filesystem::path(__FILE__).parent_path().parent_path() /
                       "keymap.ini";
  Keymap keymap;
  std::string error;
  check(load_keymap(shipped.string(), keymap, error),
        "the shipped keymap.ini does not load: " + error);
  if (!error.empty()) return;
  KeyChord meta;
  check(keymap.meta_prefix_chord(meta), "the shipped keymap does not set [meta] prefix");
  check(meta.mods == kModCtrl, "the shipped Meta prefix is not Ctrl+something");
  check(keymap.prefix.code != 0, "the shipped keymap does not set an [init] prefix");
  check(keymap.prefix.code != keymap.meta_prefix.code,
        "the shipped keymap gives [init] and [meta] the same chord, which is "
        "ambiguous");
  check(keymap.profiles.count("profile-1") == 1,
        "the shipped keymap has no profile-1");
  check(keymap.help_key_close == "q",
        "the shipped keymap's help keys are not the defaults");
  const std::string described = keymap.describe_bindings();
  check(described.find("Exec(kitty -m tmux)") != std::string::npos,
        "the shipped keymap lost its Exec example: " + described);
  check(described.find("Typeout(foobar)") != std::string::npos,
        "the shipped keymap lost its Typeout example");
}

}  // namespace

int main() {
  test_profile_overrides_builtin();
  test_profile_switching();
  test_broken_keymap_is_not_fatal();
  test_keymap_validation();
  test_check_and_reload();
  test_help_keys_from_keymap();
  test_program_binding_beats_profile();
  test_shipped_keymap();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
            << " checks, " << g_failures << " failures\n";
  return g_failures == 0 ? 0 : 1;
}
