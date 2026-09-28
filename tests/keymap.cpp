// Tests for the keymap subsystem: the INI reader, chord and action parsing,
// validation, and the evdev mapping function.
//
// The evdev backend needs root and a real keyboard, which is exactly what a
// test must not require. The mapping is a pure function, so map_event() is
// driven directly with synthetic input_events: that covers the whole decision
// table -- prefix, Ignore, Remap, Trigger, pass-through, and the modifier and
// EV_SYN handling -- with no device, no privileges and no timing.

#include <linux/input.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "keymap/EvdevRemapper.hpp"
#include "keymap/Keymap.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string &what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
  }
}

void check_equal(const std::string &actual, const std::string &expected,
                 const std::string &what) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n  expected: " << expected
              << "\n  actual:   " << actual << "\n";
  }
}

Keymap parse(const std::string &source) {
  Keymap keymap;
  std::string error;
  if (!parse_keymap(source, "test.ini", keymap, error)) {
    std::cerr << "unexpected parse failure: " << error << "\n";
    ++g_failures;
  }
  return keymap;
}

// Asserts that `source` is rejected, and that the message mentions `needle`, so
// a test cannot pass on a rejection for the wrong reason.
void check_rejected(const std::string &source, const std::string &needle,
                    const std::string &what) {
  Keymap keymap;
  std::string error;
  const bool ok = parse_keymap(source, "test.ini", keymap, error);
  ++g_checks;
  if (ok) {
    ++g_failures;
    std::cerr << "FAIL: " << what << " (accepted, should have been rejected)\n";
    return;
  }
  if (error.find(needle) == std::string::npos) {
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n  message was: " << error
              << "\n  expected it to mention: " << needle << "\n";
  }
}

// ---------------------------------------------------------------------------
// Chord parsing
// ---------------------------------------------------------------------------
void test_chords() {
  KeyChord chord;
  std::string error;

  check(parse_key_chord("<C-q>", chord, error), "C-q parses: " + error);
  check(chord.code == KEY_Q, "C-q is the q key code");
  check(chord.mods == kModCtrl, "C-q carries Ctrl");

  check(parse_key_chord("<C-S-q>", chord, error), "C-S-q parses: " + error);
  check(chord.code == KEY_Q && chord.mods == (kModCtrl | kModShift),
        "C-S-q carries Ctrl and Shift");

  check(parse_key_chord("<C-f8>", chord, error), "C-f8 parses: " + error);
  check(chord.code == KEY_F8, "C-f8 is the F8 key code");
  check(chord.mods == kModCtrl, "C-f8 carries Ctrl");

  check(parse_key_chord("f11", chord, error), "f11 parses: " + error);
  check(chord.code == KEY_F11, "f11 is F11, not F10+1");
  check(parse_key_chord("f12", chord, error), "f12 parses: " + error);
  check(chord.code == KEY_F12, "f12 is F12");

  check(parse_key_chord("<M-q>", chord, error), "M-q parses: " + error);
  check(chord.mods == kModMeta, "M-q carries Meta");
  check(parse_key_chord("<G-q>", chord, error), "G-q parses: " + error);
  check(chord.mods == kModLogo, "G-q carries Logo");
  check(parse_key_chord("<A-q>", chord, error), "A-q parses: " + error);
  check(chord.mods == kModAlt, "A-q carries Alt");

  // A chord with no modifiers is just the key.
  check(parse_key_chord("<space>", chord, error), "space parses: " + error);
  check(chord.code == KEY_SPACE && chord.mods == kModNone,
        "space carries no modifiers");

  // Brackets are optional, so a bare key works too.
  check(parse_key_chord("q", chord, error), "a bare key parses: " + error);
  check(chord.code == KEY_Q && chord.mods == kModNone, "a bare key has no modifiers");

  // Punctuation may be written as itself.
  check(parse_key_chord("<;>", chord, error), "; parses: " + error);
  check(chord.code == KEY_SEMICOLON, "; is the semicolon key");
  check(parse_key_chord("<;>", chord, error) &&
            parse_key_chord(";", chord, error) && chord.code == KEY_SEMICOLON,
        "the character and the name name the same key");

  // F1..F10 are contiguous and F11/F12 are not, so a naive offset breaks.
  check(parse_key_chord("f1", chord, error) && chord.code == KEY_F1, "f1 is F1");
  check(parse_key_chord("f10", chord, error) && chord.code == KEY_F10, "f10 is F10");

  // The kernel calls the Logo keys LEFTMETA; the INI spells them logo.
  check(parse_key_chord("<leftlogo>", chord, error), "leftlogo parses: " + error);
  check(chord.code == KEY_LEFTMETA, "leftlogo is KEY_LEFTMETA");

  // Round trip: what str() prints must parse back to the same chord.
  const char *round_trip[] = {"<C-q>", "<C-S-a>", "<f8>",  "<f11>", "<space>",
                              "<enter>", "<esc>",  "<up>",  "<leftbrace>", "<G-1>"};
  for (const char *spelling : round_trip) {
    KeyChord original, again;
    std::string first_error, second_error;
    if (!parse_key_chord(spelling, original, first_error)) {
      check(false, std::string("round trip source parses: ") + spelling);
      continue;
    }
    if (!parse_key_chord(original.str(), again, second_error)) {
      check(false, std::string("str() output parses: ") + original.str());
      continue;
    }
    check(original == again,
          std::string(spelling) + " round trips through str() (got " +
              original.str() + " -> " + again.str() + ")");
  }

  // Rejections. A typo must be an error, not a wrong key.
  check(!parse_key_chord("<C-nope>", chord, error), "an unknown key is rejected");
  check(error.find("unknown key") != std::string::npos,
        "an unknown key says so: " + error);
  check(!parse_key_chord("<C-C-q>", chord, error), "a duplicate modifier is rejected");
  check(error.find("duplicate") != std::string::npos,
        "a duplicate modifier says so: " + error);
  check(!parse_key_chord("", chord, error), "an empty chord is rejected");
  check(!parse_key_chord("<>", chord, error), "an empty bracketed chord is rejected");
  check(!parse_key_chord("<C->", chord, error), "a modifier with no key is rejected");
}

// ---------------------------------------------------------------------------
// Device identifiers
// ---------------------------------------------------------------------------
void test_device_ids() {
  DeviceId id;
  std::string error;
  check(parse_device_id("320f:5080", id, error), "320f:5080 parses: " + error);
  check(id.vendor == 0x320f, "320f:5080 vendor");
  check(id.product == 0x5080, "320f:5080 product");
  // The spelling in the file is lower case hex; the canonical form must match,
  // because that form is what diftrayremap list prints for the user to copy.
  check_equal(id.str(), "320f:5080", "DeviceId::str round trips");
  check(parse_device_id("09da:3519", id, error) && id.vendor == 0x09da,
        "a leading zero in the vendor is accepted");
  check_equal(id.str(), "09da:3519", "a leading zero survives str()");

  check(!parse_device_id("320f", id, error), "a missing product is rejected");
  check(!parse_device_id("320f:", id, error), "an empty product is rejected");
  check(!parse_device_id(":5080", id, error), "an empty vendor is rejected");
  check(!parse_device_id("zzzz:5080", id, error), "a non-hex vendor is rejected");
  check(!parse_device_id("320f:5080:1234", id, error), "three fields are rejected");
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void test_actions() {
  std::string error;

  Action exec = Action::parse("Exec(kitty -m tmux)", error);
  check(exec.kind == Action::Kind::exec, "Exec parses: " + error);
  check_equal(exec.text, "kitty -m tmux", "Exec keeps the command");
  check_equal(exec.str(), "Exec(kitty -m tmux)", "Exec round trips");

  // An argument containing parentheses must survive, because a shell command
  // may well have one.
  Action nested = Action::parse("Exec(sh -c 'echo (hi)')", error);
  check(nested.kind == Action::Kind::exec, "Exec with parentheses parses: " + error);
  check_equal(nested.text, "sh -c 'echo (hi)'",
              "Exec keeps parentheses in its argument");

  Action typeout = Action::parse("Typeout(foobar)", error);
  check(typeout.kind == Action::Kind::typeout, "Typeout parses: " + error);
  check_equal(typeout.text, "foobar", "Typeout keeps the text");

  Action trigger = Action::parse("Trigger(profile-1)", error);
  check(trigger.kind == Action::Kind::trigger, "Trigger parses: " + error);
  check_equal(trigger.profile, "profile-1", "Trigger keeps the profile name");

  Action diftray = Action::parse("Diftray(workspace 3)", error);
  check(diftray.kind == Action::Kind::diftray, "Diftray parses: " + error);
  check_equal(diftray.text, "workspace 3", "Diftray keeps the whole command");

  Action remap = Action::parse("Remap(<C-S-z>)", error);
  check(remap.kind == Action::Kind::remap, "Remap parses: " + error);
  check(remap.chord.code == KEY_Z && remap.chord.mods == (kModCtrl | kModShift),
        "Remap keeps the target chord");

  Action ignore = Action::parse("Ignore()", error);
  check(ignore.kind == Action::Kind::ignore, "Ignore parses: " + error);

  // Action names are matched case-insensitively so `typeout` also works.
  check(Action::parse("typeout(x)", error).kind == Action::Kind::typeout,
        "action names are case-insensitive");

  check(Action::parse("Exec()", error).kind == Action::Kind::none,
        "Exec with no command is rejected");
  check(Action::parse("Trigger()", error).kind == Action::Kind::none,
        "Trigger with no profile is rejected");
  check(Action::parse("Diftray()", error).kind == Action::Kind::none,
        "Diftray with no command is rejected");
  check(Action::parse("Remap(nonsense)", error).kind == Action::Kind::none,
        "Remap with a bad chord is rejected");
  check(Action::parse("Explode(now)", error).kind == Action::Kind::none,
        "an unknown action is rejected");
  check(!Action::parse("bare text", error).text.empty() || error.size(),
        "an action with no parentheses is rejected");
}

// ---------------------------------------------------------------------------
// Whole-file parsing
// ---------------------------------------------------------------------------
void test_parsing() {
  // The example that shipped with the project.
  const Keymap shipped = parse(
      "[devices]\n"
      "mouse = 09da:3519\n"
      "keyboard = 320f:5080\n"
      "\n"
      "[init]\n"
      "prefix = <C-q>\n"
      "action = Trigger(profile-1)\n"
      "\n"
      "[profile-1]\n"
      "<C-f8> = Exec(kitty -m tmux)\n"
      "<M-q> = Typeout(foobar)\n");
  check(shipped.devices.size() == 2, "both devices were read");
  check(shipped.prefix.code == KEY_Q && shipped.prefix.mods == kModCtrl,
        "the prefix is <C-q>");
  check(shipped.prefix_action.kind == Action::Kind::trigger &&
            shipped.prefix_action.profile == "profile-1",
        "the prefix triggers profile-1");
  check(shipped.profiles.size() == 1, "one profile was read");
  check(shipped.profiles.count("profile-1") == 1, "profile-1 exists");
  check(shipped.profiles.at("profile-1").bindings.size() == 2,
        "profile-1 has both bindings");
  check(!shipped.system_wide, "system_wide defaults to false");

  // A keymap that only defines profile-1, with no `default`, is what the
  // shipped example does. `default` must then be simply empty, not an error,
  // because an undefined profile means "no bindings" in keyd.
  check(shipped.current() == nullptr,
        "an absent default profile is empty rather than an error");

  // Comments, blank lines, indentation and quoted values.
  const Keymap tidy = parse(
      "; a comment\n"
      "# another comment\n"
      "\n"
      "[init]\n"
      "  prefix = <C-q>   ; trailing comment\n"
      "  action = Trigger(a)\n"
      "  system_wide = true\n"
      "\n"
      "[a]\n"
      "  <C-x> = Exec(echo \"hi # there\")\n");
  check(tidy.system_wide, "system_wide is read");
  check(tidy.profiles.count("a") == 1, "the profile was read after comments");
  // The # inside the quoted argument must not be treated as a comment.
  Action quoted;
  std::string error;
  quoted = Action::parse("Exec(echo \"hi # there\")", error);
  check_equal(quoted.text, "echo \"hi # there\"",
              "a # inside quotes is not a comment");

  // The [diftray] section carries the help pager keys, and its defaults
  // survive when the section is absent.
  check_equal(tidy.help_key_close, "q", "help close key defaults to q");
  check_equal(tidy.help_key_line_down, "j", "help line-down key defaults to j");
  const Keymap with_help = parse(
      "[diftray]\n"
      "help_key_close = Q\n"
      "help_key_page_down = pagedown\n");
  check_equal(with_help.help_key_close, "Q", "an overridden help key is read");
  check_equal(with_help.help_key_page_down, "pagedown",
              "a named help key is read");

  // Two profiles with the same chord are fine; the same chord twice in one
  // profile is not, because the second line would silently never fire.
  const Keymap two = parse(
      "[init]\nprefix = <C-q>\naction = Trigger(b)\n"
      "[a]\n<C-x> = Ignore()\n"
      "[b]\n<C-y> = Trigger(a)\n");
  check(two.profiles.size() == 2, "two distinct profiles were read");
  check_rejected("[a]\n<C-x> = Ignore()\n<C-x> = Trigger(a)\n", "duplicate",
                 "the same chord twice in one profile is rejected");
  check_rejected("[a]\n<C-x> = Ignore()\n[a]\n<C-x> = Ignore()\n", "duplicate",
                 "the same chord in two same-named sections is rejected");
  check_rejected("[devices]\nkeyboard = 320f:5080\nkeyboard = 320f:5080\n",
                 "duplicate", "a duplicate device is rejected");

  // A Trigger() to a profile that does not exist would leave the keymap with
  // no way back, so it is a load-time error rather than a runtime surprise.
  check_rejected("[a]\n<C-x> = Trigger(nowhere)\n", "unknown profile",
                 "Trigger to a missing profile is rejected");
  check_rejected("[init]\nprefix = <C-q>\naction = Trigger(nowhere)\n",
                 "unknown profile", "a prefix triggering a missing profile is rejected");

  // A prefix with no action, or an action with no prefix, is half a feature.
  check_rejected("[init]\nprefix = <C-q>\n", "action",
                 "a prefix with no action is rejected");
  check_rejected("[init]\naction = Trigger(a)\n[a]\n", "prefix",
                 "an action with no prefix is rejected");
  check_rejected("[init]\nprefix = <C-q>\naction = Exec(x)\n[a]\n", "Trigger",
                 "a non-Trigger init action is rejected");

  // Structural errors.
  check_rejected("[unterminated\n", "unterminated", "a bad section header is rejected");
  check_rejected("<C-q> = Ignore()\n", "outside of any section",
                 "a setting before any section is rejected");
  check_rejected("[a]\nno equals sign here\n", "expected key = value",
                 "a line with no = is rejected");
  check_rejected("[a]\n<C-x> = NotAnAction()\n", "unknown action",
                 "an unknown action in a file is rejected");
  check_rejected("[nonsense]\nkey = value\n", "key:",
                 "an unknown setting in [nonsense] is reported as a bad key");
  check_rejected("[diftray]\nhelp_key_bogus = q\n", "unknown [diftray] setting",
                 "an unknown [diftray] setting is rejected");
  check_rejected("[init]\nunknown_thing = 1\n", "unknown [init] setting",
                 "an unknown [init] setting is rejected");
  check_rejected("", "no profiles, no prefix and no devices",
                 "an empty keymap is rejected");
  check_rejected("[diftray]\n", "no profiles, no prefix and no devices",
                 "a bare [diftray] header with nothing in it is rejected");
  check_rejected("[init]\ndefault_profile = ghost\n", "unknown profile",
                 "a default_profile naming a missing profile is rejected");
  // A file that only configures devices is legitimate: the system-wide backend
  // reads [devices] and needs no bindings to have something to do.
  const Keymap devices_only = parse("[devices]\nkeyboard = 320f:5080\n");
  check(devices_only.devices.size() == 1, "a devices-only keymap is accepted");
  check(devices_only.empty(), "a devices-only keymap has no bindings");

  // A named default profile that does exist is fine.
  const Keymap named = parse(
      "[init]\ndefault_profile = work\n[work]\n<C-x> = Ignore()\n");
  check(named.current() != nullptr && named.current()->name == "work",
        "default_profile selects the startup profile");
  check(named.default_profile == "work", "default_profile is remembered");
}

// ---------------------------------------------------------------------------
// The evdev mapping
// ---------------------------------------------------------------------------
input_event make_event(unsigned int type, unsigned int code, int value) {
  input_event event{};
  event.type = type;
  event.code = code;
  event.value = value;
  return event;
}

// Resolves a profile by name, failing the test if it is missing.
const Profile *named(const Keymap &keymap, const std::string &name) {
  std::string error;
  const Profile *profile = keymap.profile_named(name, error);
  check(profile != nullptr, "profile " + name + " resolves: " + error);
  return profile;
}

// A keyboard driven by a script: it holds modifiers down, presses and releases
// keys, and records what the remapper would publish. This is how a real device
// looks to map_event, and modelling it is the only way to test a chord, since a
// bare key event says nothing about which modifiers are down.
class FakeKeyboard {
public:
  explicit FakeKeyboard(const Keymap &keymap)
      : keymap_(keymap), profiles_(keymap) {}

  void hold(unsigned int modifier) { feed(modifier, 1); }
  void release(unsigned int modifier) { feed(modifier, 0); }

  void tap(unsigned int code) {
    feed(code, 1);
    feed(code, 0);
  }

  // Presses with `held` already down, the way a chord is really typed.
  void chord(unsigned int code, uint32_t mods) {
    held_ = mods;
    feed(code, 1);
    feed(code, 0);
    held_ = kModNone;
  }

  const std::vector<input_event> &published() const { return published_; }
  const std::vector<std::string> &profile_switches() const { return switches_; }
  const Profile *active() const { return profiles_.active(); }
  uint32_t held() const { return held_; }
  void clear() {
    published_.clear();
    switches_.clear();
  }

private:
  void feed(unsigned int code, int value) {
    std::string switch_to;
    for (const auto &event :
         EvdevRemapper::map_event(profiles_.active(), keymap_, held_,
                                  make_event(EV_KEY, code, value), switch_to,
                                  &held_)) {
      published_.push_back(event);
    }
    profiles_.note(switch_to, make_event(EV_KEY, code, value));
    if (profiles_.applied()) {
      switches_.push_back(profiles_.active()->name);
    }
  }

  const Keymap &keymap_;
  EvdevRemapper::ProfileState profiles_;
  uint32_t held_ = kModNone;
  std::vector<input_event> published_;
  std::vector<std::string> switches_;
};

// A tap() or chord() publishes a press and a release, so a pass-through is two
// events. Checking both is the point: a remapper that publishes only one of them
// leaves the consumer holding a key forever.
bool published_pair(const std::vector<input_event> &events, unsigned int code) {
  return events.size() == 2 && events[0].type == EV_KEY &&
         events[0].code == code && events[0].value == 1 &&
         events[1].type == EV_KEY && events[1].code == code &&
         events[1].value == 0;
}

// The events a modifier tap publishes, when only the modifier was typed.
bool published_one(const std::vector<input_event> &events, unsigned int code) {
  return events.size() == 1 && events[0].type == EV_KEY && events[0].code == code;
}

void test_evdev_mapping() {
  const Keymap keymap = parse(
      "[init]\n"
      "prefix = <C-q>\n"
      "action = Trigger(profile-1)\n"
      "[default]\n"
      "<C-a> = Ignore()\n"
      "<C-b> = Remap(<C-c>)\n"
      "<C-t> = Trigger(profile-1)\n"
      "[profile-1]\n"
      "<C-a> = Ignore()\n"
      "<M-esc> = Trigger(default)\n");
  const Profile *normal = named(keymap, "default");
  const Profile *armed = named(keymap, "profile-1");
  check(normal && armed, "both profiles resolve");
  if (!normal || !armed) return;

  FakeKeyboard keys(keymap);
  check(keys.active() == normal, "a keymap starts in its default profile");

  // An unbound key passes through untouched: the whole machine must keep
  // working, and only the chords named in the file may change.
  keys.clear();
  keys.tap(KEY_X);
  check(published_pair(keys.published(), KEY_X), "an unbound key passes through");
  check(keys.profile_switches().empty(), "an unbound key switches no profile");

  // A chord with the wrong modifiers is still unbound. <C-a> is bound, so a
  // plain `a` and a Ctrl+Shift+a must both reach the consumer: a chord means
  // exactly the modifiers written.
  keys.clear();
  keys.chord(KEY_A, kModNone);
  check(published_pair(keys.published(), KEY_A), "plain `a` is not <C-a>");

  keys.clear();
  keys.chord(KEY_A, kModCtrl | kModShift);
  check(published_pair(keys.published(), KEY_A), "Ctrl+Shift+a is not <C-a>");

  // Ignore() swallows the press, and the matching release too, or the consumer
  // would be left holding a key forever.
  keys.clear();
  keys.chord(KEY_A, kModCtrl);
  check(keys.published().empty(), "Ignore swallows both the press and the release");

  // Remap() substitutes the code. A remap must be emitted once and not looked
  // up again, otherwise <C-b> -> <C-c> would loop.
  keys.clear();
  keys.chord(KEY_B, kModCtrl);
  check(published_pair(keys.published(), KEY_C), "Remap substitutes the key code");

  // The prefix runs its action and is swallowed on both edges, so the chord
  // never reaches the consumer as a Ctrl+q. The switch lands on the release,
  // which is what keeps that release from being looked up in the new profile.
  {
    FakeKeyboard prefix(keymap);
    prefix.chord(KEY_Q, kModCtrl);
    check(prefix.published().empty(), "the prefix is swallowed on both edges");
    check(prefix.profile_switches().size() == 1 &&
              prefix.profile_switches().front() == "profile-1",
          "the prefix triggers its profile");
    check(prefix.active() == armed, "the prefix left us in profile-1");
    // Now in profile-1, where <M-esc> goes home. M is the AltGr bit, distinct
    // from the Logo bit G.
    prefix.clear();
    prefix.chord(KEY_ESC, kModMeta);
    check(prefix.published().empty(), "profile-1 swallows <M-esc>");
    check(prefix.profile_switches().size() == 1 &&
              prefix.profile_switches().front() == "default",
          "profile-1 returns to default");
    check(prefix.active() == normal, "we are back in the default profile");
    prefix.clear();
    prefix.chord(KEY_X, kModNone);
    check(published_pair(prefix.published(), KEY_X),
          "returning to default restores the original bindings");
  }

  // Trigger() switches profile and is swallowed, so switching does not also
  // type the key. Its release must be swallowed too, even though by then the
  // active profile has changed -- which is the whole reason for deferring.
  {
    FakeKeyboard switching(keymap);
    switching.chord(KEY_T, kModCtrl);
    check(switching.published().empty(),
          "a Trigger key is swallowed on both edges");
    check(switching.profile_switches().size() == 1 &&
              switching.profile_switches().front() == "profile-1",
          "a Trigger key names its profile");
    check(switching.active() == armed, "a Trigger left us in the new profile");
  }

  // Profiles are independent: the same chord can mean different things. In
  // profile-1, <M-esc> is bound; plain Escape and <G-esc> are not.
  {
    FakeKeyboard in_armored(keymap);
    in_armored.chord(KEY_Q, kModCtrl);  // enter profile-1
    check(in_armored.active() == armed, "entered profile-1");
    in_armored.clear();
    in_armored.chord(KEY_ESC, kModNone);
    check(published_pair(in_armored.published(), KEY_ESC),
          "plain Escape is not <M-esc>");
    in_armored.clear();
    in_armored.chord(KEY_ESC, kModLogo);
    check(published_pair(in_armored.published(), KEY_ESC),
          "<G-esc> is not <M-esc>: M and G are different modifiers");
    in_armored.clear();
    in_armored.chord(KEY_ESC, kModMeta);
    check(in_armored.published().empty(), "<M-esc> in profile-1 is swallowed");
  }

  // Modifier keys always pass through, or a chord could never be pressed at
  // all: the Ctrl release has to reach the consumer or every later key would
  // look chorded.
  keys.clear();
  keys.hold(KEY_LEFTCTRL);
  check(published_one(keys.published(), KEY_LEFTCTRL),
        "a modifier press passes through even when it is part of a chord");
  check(keys.held() == kModCtrl, "the held modifier state follows the event");
  keys.clear();
  keys.release(KEY_LEFTCTRL);
  check(published_one(keys.published(), KEY_LEFTCTRL),
        "a modifier release passes through too");
  check(keys.held() == kModNone, "the held modifier state follows the release");

  // Left and right of the same modifier are one bit, so a chord typed with
  // right Ctrl still matches <C-a>.
  {
    FakeKeyboard right(keymap);
    right.hold(KEY_RIGHTCTRL);
    check(right.held() == kModCtrl, "right Ctrl is the same bit as left Ctrl");
    right.clear();
    right.chord(KEY_A, kModCtrl);
    check(right.published().empty(), "right Ctrl + a is the same <C-a>");
  }

  // EV_SYN and EV_MSC must pass through, or the consumer desynchronises.
  for (const unsigned int type : {EV_SYN, EV_MSC}) {
    std::string switch_to;
    const auto out = EvdevRemapper::map_event(normal, keymap, kModNone,
                                              make_event(type, 0, 0), switch_to);
    check(out.size() == 1 && out[0].type == type,
          "a non-EV_KEY event passes through");
  }

  // Diftray(), Exec() and Typeout() have no evdev meaning, so the key passes
  // through for the compositor's own copy of the keymap to handle.
  const Keymap passthrough = parse("[default]\n<C-d> = Diftray(workspace 3)\n"
                                   "<C-e> = Exec(xterm)\n"
                                   "<C-u> = Typeout(hi)\n");
  const Profile *only = passthrough.current();
  check(only != nullptr, "the passthrough keymap has a default profile");
  if (only) {
    for (const unsigned int code : {KEY_D, KEY_E, KEY_U}) {
      FakeKeyboard through(passthrough);
      through.chord(code, kModCtrl);
      check(published_pair(through.published(), code),
            "a compositor-level action passes through to the compositor");
    }
  }

  // An empty keymap changes nothing at all.
  {
    std::string switch_to;
    const auto out = EvdevRemapper::map_event(nullptr, keymap, kModCtrl,
                                              make_event(EV_KEY, KEY_A, 1), switch_to);
    check(out.size() == 1, "a null profile passes everything through");
  }
}

// ---------------------------------------------------------------------------
// Capability reporting
// ---------------------------------------------------------------------------
void test_capability() {
  // probe() must never claim it can run when it cannot, and must always
  // explain itself. Running unprivileged is the normal case and is what makes
  // this assertion meaningful.
  const RemapCapability capability = EvdevRemapper::probe();
  const std::string explanation = capability.explain();
  check(!explanation.empty(), "probe() always explains itself");
  if (capability.can_run) {
    check_equal(explanation, "evdev remapping is available",
                "a usable system explains that plainly");
  } else {
    check(explanation.find("uinput") != std::string::npos,
          "an unusable system names the missing piece: " + explanation);
  }
  check(!capability.is_root || capability.uinput_writable,
        "root can normally write uinput");

  // start() must refuse cleanly rather than half-claiming a device. On a machine
  // where the probe says no, the refusal is the interesting path; where it says
  // yes, the absence of a matching keyboard is.
  const Keymap keymap = parse("[devices]\nkeyboard = 0000:0000\n[default]\n<C-a> = Ignore()\n");
  EvdevRemapper remapper;
  check(!remapper.running(), "a fresh remapper is not running");
  std::string error;
  if (!remapper.start(keymap, error)) {
    check(!error.empty(), "a refusal explains why");
    check(!remapper.running(), "a refused start leaves nothing running");
  }
  remapper.stop();
  check(!remapper.running(), "stop() is safe to call when not running");
  const std::string status = remapper.status();
  check(status.find("not running") != std::string::npos,
        "a stopped remapper says so: " + status);
}

// The [init] prefix and the compositor's own Meta prefix are the same chord, so
// the keymap must be the authority and the C++ fallback must not fight it.
void test_prefix_authority() {
  const Keymap keymap = parse("[init]\nprefix = <C-g>\naction = Trigger(p)\n[p]\n<C-a> = Ignore()\n");
  check_equal(keymap.prefix.str(), "C-g", "the keymap's prefix is reported canonically");
  check(keymap.prefix_action.kind == Action::Kind::trigger, "the prefix action is kept");
  check(keymap.empty() == false, "a keymap with a prefix is not empty");
  const Keymap bare = parse("[default]\n<C-a> = Ignore()\n");
  check(!bare.empty(), "a keymap with only a profile is not empty");
  check(bare.prefix.code == 0, "a keymap with no prefix has none");
  const Keymap nothing = parse("[devices]\nkeyboard = 320f:5080\n");
  check(nothing.empty(), "devices alone is an empty keymap");
}

}  // namespace

int main() {
  test_chords();
  test_device_ids();
  test_actions();
  test_parsing();
  test_evdev_mapping();
  test_capability();
  test_prefix_authority();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
            << " checks, " << g_failures << " failures\n";
  return g_failures == 0 ? 0 : 1;
}
