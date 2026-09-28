#pragma once
// Keymap model and INI reader.
//
// Every key-related aspect of DiftrayWM -- the Meta prefix chord, the built-in
// bindings, the help pager keys, and the optional system-wide evdev remap -- is
// described by an INI file in the style used by keyd. The compositor's own
// config file only names the file:
//
//     [general]
//     keymap = keymap.ini
//
// The file is parsed into this model and validated strictly: an unknown key, an
// unknown action or a reference to a missing profile is an error rather than a
// silently ignored line, because a typo in a keymap would otherwise look like
// a dead key.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>


// Modifier bits, matching the Linux input event encoding so the same chord can
// be matched both by the compositor (XKB modifiers) and by the evdev backend.
enum KeyMods : uint32_t {
  kModNone = 0,
  kModShift = 1u << 0,
  kModCtrl = 1u << 1,
  kModAlt = 1u << 2,
  kModMeta = 1u << 3,
  kModLogo = 1u << 4,
};

// A key plus modifiers, written `<C-S-q>` or `<C-f8>` in the INI file.
struct KeyChord {
  uint32_t code = 0;  // Linux evdev key code (KEY_*)
  uint32_t mods = kModNone;

  bool operator<(const KeyChord &other) const {
    if (code != other.code) return code < other.code;
    return mods < other.mods;
  }
  bool operator==(const KeyChord &other) const {
    return code == other.code && mods == other.mods;
  }
  bool empty() const { return code == 0; }
  // Canonical spelling, e.g. "C-S-q" or "F8".
  std::string str() const;
};

// What a chord does when pressed.
struct Action {
  enum class Kind {
    none,
    exec,     // Exec(command)  -- run a program
    typeout,  // Typeout(text) -- write text to the focused terminal
    trigger,  // Trigger(profile) -- switch profile
    diftray,  // Diftray(command) -- dispatch a Command Bar command
    remap,    // Remap(<chord>) -- emit a different chord downstream
    ignore,   // Ignore() -- swallow the key
  };
  Kind kind = Kind::none;
  std::string text;    // exec command / typeout text / diftray command
  std::string profile; // trigger target
  KeyChord chord;      // remap target
  std::string str() const;
  static Action parse(std::string_view text, std::string &error);
};

// A named set of chord -> action bindings.
struct Profile {
  std::string name;
  std::map<KeyChord, Action> bindings;
};

// A keyboard identified by USB vendor and product, as "320f:5080".
struct DeviceId {
  uint16_t vendor = 0;
  uint16_t product = 0;
  std::string str() const;
  bool operator<(const DeviceId &other) const {
    if (vendor != other.vendor) return vendor < other.vendor;
    return product < other.product;
  }
  bool operator==(const DeviceId &other) const {
    return vendor == other.vendor && product == other.product;
  }
};

// A parsed keymap.ini.
//
// Two prefixes live here, and they are different things:
//
//   [init] prefix  the keyd-style remapping layer. Pressing it runs [init]
//                  action, normally Trigger() into another profile, and the
//                  keypress is swallowed. It is a device-level concept: it
//                  changes which chords the *machine* answers to, so the
//                  system-wide backend honours it too.
//   [meta] prefix  the compositor's Meta prefix: press it, then the next key
//                  behaves as if Meta were held. It is a session-level
//                  concept, and only the compositor has one.
//
// They are separate settings because they answer different questions, and
// giving them the same chord would be ambiguous: the layer switch would
// consume the key before the Meta prefix ever saw it.
struct Keymap {
  // [devices] -- keyboards the system-wide remapper should intercept. Empty
  // means "every keyboard", which is the safe default for a machine that should
  // not be left half remapped.
  std::map<DeviceId, std::string> devices;  // id -> role ("keyboard", "mouse", ...)

  // [init] -- the remapping layer's chord, and what it does.
  KeyChord prefix;
  Action prefix_action;

  // [meta] -- the compositor's Meta prefix. Unset leaves the compositor on its
  // own fallbacks rather than on nothing.
  KeyChord meta_prefix;
  bool meta_prefix_set = false;

  // Profiles, keyed by name. `default` is active before the prefix is pressed.
  std::map<std::string, Profile> profiles;
  std::string default_profile = "default";

  // [diftray] -- compositor-level key configuration that is not a binding.
  // These carry the historical pager bindings, so a keymap that only remaps
  // chords still behaves like the built-in one.
  std::string help_key_close = "q";
  std::string help_key_search = "/";
  std::string help_key_next = "n";
  std::string help_key_previous = "?";
  std::string help_key_page_down = "space";
  std::string help_key_page_up = "b";
  std::string help_key_line_down = "j";
  std::string help_key_line_up = "k";
  // True once [diftray] has assigned at least one of them, so a file that only
  // sets help keys is not mistaken for an empty one.
  bool help_key_set = false;

  // When true the keymap file asks for the privileged evdev backend as well.
  // The compositor still applies the keymap itself either way; see
  // EvdevRemapper for why that lives in a separate process.
  bool system_wide = false;
  std::string profile_path;

  const Profile *current() const;
  // A profile by name, or nullptr with an explanation.
  const Profile *profile_named(const std::string &name, std::string &error) const;
  // The action a chord performs, or nullptr when it is unbound.
  const Action *lookup(const std::string &profile, const KeyChord &chord) const;
  bool empty() const { return profiles.empty() && !prefix.code; }
  std::string summary() const;
  // The Meta prefix chord, for the compositor's own matching. False when the
  // keymap does not set one, in which case the compositor keeps its fallback.
  bool meta_prefix_chord(KeyChord &out) const {
    if (!meta_prefix_set) return false;
    out = meta_prefix;
    return true;
  }
  // Every binding, flattened, as "profile: <chord> = action". Used by
  // `keymap show` and the help pages.
  std::string describe_bindings() const;
};

// Parses an INI source. `path` is only used in error messages.
bool parse_keymap(std::string_view source, const std::string &path, Keymap &out,
                  std::string &error);
// Reads and parses a file, refusing anything larger than 1 MiB.
bool load_keymap(const std::string &path, Keymap &out, std::string &error);

// Exposed for tests and for error messages: "C-q" or "<C-q>".
bool parse_key_chord(std::string_view text, KeyChord &out, std::string &error);
// "320f:5080"
bool parse_device_id(std::string_view text, DeviceId &out, std::string &error);
// Looks up a Linux key name ("q", "f8", "leftbrace") or a single character.
bool key_code_from_name(std::string_view name, uint32_t &out, std::string &error);

