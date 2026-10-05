#include "keymap/Keymap.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace {

constexpr std::size_t kMaxKeymapBytes = 1024 * 1024;

std::string lower(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return out;
}

std::string trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return {};
  const auto end = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(begin, end - begin + 1));
}

// Strips one layer of matching quotes so a value may contain spaces.
std::string unquote(std::string_view text) {
  if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
      text.back() == text.front()) {
    return std::string(text.substr(1, text.size() - 2));
  }
  return std::string(text);
}

// Splits "Exec(kitty -m tmux)" into "Exec" and "kitty -m tmux". The argument may
// itself contain parentheses, so only the outermost pair is stripped.
bool split_call(std::string_view text, std::string &name, std::string &argument,
                std::string &error) {
  const std::size_t open = text.find('(');
  if (open == std::string_view::npos || text.back() != ')') {
    error = "expected Name(argument)";
    return false;
  }
  name = std::string(trim(text.substr(0, open)));
  argument = std::string(trim(text.substr(open + 1, text.size() - open - 2)));
  return true;
}

// Every key the INI may name, as an explicit table. It is explicit rather
// than derived on purpose: an unknown name must be a hard error, and a derived
// table that guessed wrong would silently bind a different key. Note in
// particular that Linux key codes are qwerty order, not alphabetical, so
// KEY_G is 34 and not KEY_A + 6.
const std::pair<const char *, uint32_t> kNamedKeys[] = {
    // Function keys come first so str() reports them by name. They cannot be
    // treated as a contiguous range: F1..F10 are consecutive, but F11 and F12
    // are not adjacent to them on every kernel header.
    {"f1", KEY_F1},               {"f2", KEY_F2},
    {"f3", KEY_F3},               {"f4", KEY_F4},
    {"f5", KEY_F5},               {"f6", KEY_F6},
    {"f7", KEY_F7},               {"f8", KEY_F8},
    {"f9", KEY_F9},               {"f10", KEY_F10},
    {"f11", KEY_F11},             {"f12", KEY_F12},
    // Printable keys, in the order the physical keyboard numbers them.
    {"1", KEY_1},                 {"2", KEY_2},
    {"3", KEY_3},                 {"4", KEY_4},
    {"5", KEY_5},                 {"6", KEY_6},
    {"7", KEY_7},                 {"8", KEY_8},
    {"9", KEY_9},                 {"0", KEY_0},
    {"minus", KEY_MINUS},         {"equal", KEY_EQUAL},
    {"q", KEY_Q},                 {"w", KEY_W},
    {"e", KEY_E},                 {"r", KEY_R},
    {"t", KEY_T},                 {"y", KEY_Y},
    {"u", KEY_U},                 {"i", KEY_I},
    {"o", KEY_O},                 {"p", KEY_P},
    {"leftbrace", KEY_LEFTBRACE}, {"rightbrace", KEY_RIGHTBRACE},
    {"backslash", KEY_BACKSLASH}, {"a", KEY_A},
    {"s", KEY_S},                 {"d", KEY_D},
    {"f", KEY_F},                 {"g", KEY_G},
    {"h", KEY_H},                 {"j", KEY_J},
    {"k", KEY_K},                 {"l", KEY_L},
    {"semicolon", KEY_SEMICOLON}, {"apostrophe", KEY_APOSTROPHE},
    {"z", KEY_Z},                 {"x", KEY_X},
    {"c", KEY_C},                 {"v", KEY_V},
    {"b", KEY_B},                 {"n", KEY_N},
    {"m", KEY_M},                 {"comma", KEY_COMMA},
    {"dot", KEY_DOT},             {"slash", KEY_SLASH},
    {"space", KEY_SPACE},         {"grave", KEY_GRAVE},
    // Non-printing keys.
    {"enter", KEY_ENTER},         {"return", KEY_ENTER},
    {"tab", KEY_TAB},             {"backspace", KEY_BACKSPACE},
    {"esc", KEY_ESC},             {"escape", KEY_ESC},
    {"del", KEY_DELETE},          {"delete", KEY_DELETE},
    {"ins", KEY_INSERT},          {"insert", KEY_INSERT},
    {"home", KEY_HOME},           {"end", KEY_END},
    {"pageup", KEY_PAGEUP},       {"pagedown", KEY_PAGEDOWN},
    {"up", KEY_UP},               {"down", KEY_DOWN},
    {"left", KEY_LEFT},           {"right", KEY_RIGHT},
    {"menu", KEY_COMPOSE},        {"capslock", KEY_CAPSLOCK},
    {"numlock", KEY_NUMLOCK},     {"scrolllock", KEY_SCROLLLOCK},
    {"prtsc", KEY_SYSRQ},         {"pause", KEY_PAUSE},
    {"leftshift", KEY_LEFTSHIFT}, {"rightshift", KEY_RIGHTSHIFT},
    {"leftctrl", KEY_LEFTCTRL},   {"rightctrl", KEY_RIGHTCTRL},
    {"leftalt", KEY_LEFTALT},     {"rightalt", KEY_RIGHTALT},
    // The kernel calls the Super/Logo keys KEY_LEFTMETA and KEY_RIGHTMETA.
    // They are spelled logo here because that is what the key is labelled on
    // the keyboard, and G- is already this modifier elsewhere in the file.
    {"leftlogo", KEY_LEFTMETA},   {"rightlogo", KEY_RIGHTMETA},
    // Aliases. The table is searched in order, so a canonical name listed above
    // is what str() reports; these exist only so the alias also parses.
    {"-", KEY_MINUS},             {"=", KEY_EQUAL},
    {"[", KEY_LEFTBRACE},         {"{", KEY_LEFTBRACE},
    {"]", KEY_RIGHTBRACE},        {"}", KEY_RIGHTBRACE},
    {"\\", KEY_BACKSLASH},        {"|", KEY_BACKSLASH},
    {";", KEY_SEMICOLON},         {":", KEY_SEMICOLON},
    {"'", KEY_APOSTROPHE},        {"\"", KEY_APOSTROPHE},
    {",", KEY_COMMA},             {"<", KEY_COMMA},
    {".", KEY_DOT},               {">", KEY_DOT},
    {"/", KEY_SLASH},             {"?", KEY_SLASH},
    {"`", KEY_GRAVE},             {"~", KEY_GRAVE},
    {" ", KEY_SPACE},
};

uint32_t function_key(unsigned int number) {
  // F1..F10 are contiguous, F11 and F12 are not.
  if (number >= 1 && number <= 10) return KEY_F1 + (number - 1);
  if (number == 11) return KEY_F11;
  if (number == 12) return KEY_F12;
  return 0;
}

}  // namespace

std::string KeyChord::str() const {
  std::string out;
  if (mods & kModCtrl) out += "C-";
  if (mods & kModAlt) out += "A-";
  if (mods & kModShift) out += "S-";
  if (mods & kModMeta) out += "M-";
  if (mods & kModLogo) out += "G-";
  // kNamedKeys lists the canonical spelling before any alias, so searching it
  // in order always reports the name a user would write. This is the only way
  // to name a function key, because F1..F10, F11 and F12 are not one range.
  for (const auto &[name, candidate] : kNamedKeys) {
    if (candidate == code) {
      out += name;
      return out;
    }
  }
  out += "code" + std::to_string(code);
  return out;
}

std::string Action::str() const {
  switch (kind) {
    case Kind::exec: return "Exec(" + text + ")";
    case Kind::typeout: return "Typeout(" + text + ")";
    case Kind::trigger: return "Trigger(" + profile + ")";
    case Kind::diftray: return "Diftray(" + text + ")";
    case Kind::remap: return "Remap(<" + chord.str() + ">)";
    case Kind::ignore: return "Ignore()";
    case Kind::none: break;
  }
  return {};
}

Action Action::parse(std::string_view text, std::string &error) {
  error.clear();
  Action action;
  const std::string body = trim(text);
  std::string name, argument;
  if (!split_call(body, name, argument, error)) {
    return action;
  }
  const std::string lower_name = lower(name);
  if (lower_name == "exec") {
    if (argument.empty()) { error = "Exec requires a command"; return {}; }
    action.kind = Kind::exec;
  } else if (lower_name == "typeout") {
    if (argument.empty()) { error = "Typeout requires text"; return {}; }
    action.kind = Kind::typeout;
  } else if (lower_name == "trigger") {
    if (argument.empty()) { error = "Trigger requires a profile name"; return {}; }
    action.kind = Kind::trigger;
    action.profile = argument;
  } else if (lower_name == "diftray") {
    if (argument.empty()) { error = "Diftray requires a Command Bar command"; return {}; }
    action.kind = Kind::diftray;
  } else if (lower_name == "remap") {
    KeyChord chord;
    // split_call() has already consumed the closing parenthesis, so an
    // argument written "<C-q>" arrives here as "<C-q". Restore the bracket
    // rather than making every chord carry its own closing one.
    std::string spelling = argument;
    if (!spelling.empty() && spelling.front() == '<' && spelling.back() != '>') {
      spelling.push_back('>');
    }
    if (!parse_key_chord(spelling, chord, error)) {
      error = "Remap: " + error;
      return {};
    }
    action.kind = Kind::remap;
    action.chord = chord;
  } else if (lower_name == "ignore") {
    action.kind = Kind::ignore;
  } else {
    error = "unknown action: " + name;
    return {};
  }
  action.text = argument;
  return action;
}

bool key_code_from_name(std::string_view name, uint32_t &out, std::string &error) {
  const std::string key = lower(trim(name));
  if (key.empty()) {
    error = "empty key name";
    return false;
  }
  // F1..F12 are a computed range, so they are checked before the table, which
  // holds the rest. The table is searched first-match, so the canonical
  // spellings it lists are what str() reports back.
  if (key.size() >= 2 && key[0] == 'f') {
    bool digits = true;
    for (std::size_t i = 1; i < key.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(key[i]))) { digits = false; break; }
    }
    if (digits) {
      const auto number =
          static_cast<unsigned int>(std::strtoul(key.c_str() + 1, nullptr, 10));
      if (const uint32_t code = function_key(number); code != 0) {
        out = code;
        return true;
      }
    }
  }
  for (const auto &[candidate, code] : kNamedKeys) {
    if (key == candidate) {
      out = code;
      return true;
    }
  }
  error = "unknown key: " + key;
  return false;
}

bool parse_key_chord(std::string_view text, KeyChord &out, std::string &error) {
  error.clear();
  std::string body = trim(text);
  if (body.size() >= 2 && body.front() == '<' && body.back() == '>') {
    body = trim(body.substr(1, body.size() - 2));
  }
  if (body.empty()) {
    error = "empty key chord";
    return false;
  }
  out = KeyChord{};
  // Consume modifier prefixes left to right. A modifier is a single character
  // followed by '-' or '+', so only a body of three or more can start with one.
  for (;;) {
    if (body.size() > 2) {
      if (body[1] == '-' || body[1] == '+') {
        const std::string token = lower(body.substr(0, 1));
        uint32_t bit = 0;
        if (token == "c") bit = kModCtrl;
        else if (token == "a") bit = kModAlt;
        else if (token == "s") bit = kModShift;
        else if (token == "m") bit = kModMeta;
        else if (token == "g") bit = kModLogo;
        if (bit != 0) {
          if (out.mods & bit) {
            error = "duplicate modifier in chord: " + std::string(text);
            return false;
          }
          out.mods |= bit;
          body = body.substr(2);
          continue;
        }
      }
    }
    break;
  }
  return key_code_from_name(body, out.code, error);
}

bool parse_device_id(std::string_view text, DeviceId &out, std::string &error) {
  error.clear();
  const std::string body = trim(text);
  const auto colon = body.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= body.size()) {
    error = "expected VENDOR:PRODUCT";
    return false;
  }
  const auto parse = [](const std::string &part, uint16_t &value) {
    if (part.empty() || part.size() > 4) return false;
    for (const char ch : part) {
      if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    const auto parsed = std::strtoul(part.c_str(), nullptr, 16);
    if (parsed > 0xffff) return false;
    value = static_cast<uint16_t>(parsed);
    return true;
  };
  if (!parse(body.substr(0, colon), out.vendor) ||
      !parse(body.substr(colon + 1), out.product)) {
    error = "expected a hexadecimal VENDOR:PRODUCT pair, got: " + body;
    return false;
  }
  return true;
}

std::string DeviceId::str() const {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04x:%04x", vendor, product);
  return buffer;
}

const Profile *Keymap::profile_named(const std::string &name, std::string &error) const {
  error.clear();
  const auto it = profiles.find(name);
  if (it == profiles.end()) {
    error = "unknown profile: " + name;
    return nullptr;
  }
  return &it->second;
}

const Profile *Keymap::current() const {
  const auto it = profiles.find(default_profile);
  return it == profiles.end() ? nullptr : &it->second;
}

const Action *Keymap::lookup(const std::string &profile, const KeyChord &chord) const {
  const auto it = profiles.find(profile);
  if (it == profiles.end()) return nullptr;
  const auto binding = it->second.bindings.find(chord);
  return binding == it->second.bindings.end() ? nullptr : &binding->second;
}

std::string Keymap::describe_bindings() const {
  std::ostringstream out;
  for (const auto &[name, profile] : profiles) {
    out << "[" << name << "]";
    if (profile.bindings.empty()) {
      out << " (no bindings)\n";
      continue;
    }
    for (const auto &[chord, action] : profile.bindings) {
      out << "\n  <" << chord.str() << "> = " << action.str();
    }
    out << "\n";
  }
  return out.str();
}

std::string Keymap::summary() const {
  std::ostringstream out;
  out << "keymap: " << (profile_path.empty() ? "(none)" : profile_path) << "\n";
  out << "profiles: " << profiles.size() << "\n";
  if (!prefix.code) {
    out << "layer prefix: (none)\n";
  } else {
    out << "layer prefix: <" << prefix.str() << ">";
    if (prefix_action.kind == Action::Kind::trigger) {
      out << " -> " << prefix_action.profile;
    }
    out << "\n";
  }
  out << "meta prefix: "
      << (meta_prefix_set ? "<" + meta_prefix.str() + ">" : std::string("(compositor fallback)"))
      << "\n";
  out << "devices: "
      << (devices.empty() ? std::string("(all keyboards)") : std::to_string(devices.size()))
      << "\n";
  out << "system-wide: " << (system_wide ? "requested" : "no") << "\n";
  out << "repeat: " << repeat_rate << " Hz, delay: " << repeat_delay << " ms\n";
  return out.str();
}

namespace {

bool parse_help_key(const std::string &value, const char *name, std::string &out,
                    std::string &error) {
  // Help keys keep their historical spelling: a single character, or one of the
  // friendly names the pager already understands. A character is stored as
  // written, because help_key_matches() compares it against the character the
  // user actually typed and folding case here would break an uppercase binding.
  const std::string trimmed = trim(value);
  const std::string folded = lower(trimmed);
  static const std::set<std::string> named{"space", "pagedown", "page-down",
                                           "pageup", "page-up", "up", "down"};
  if (trimmed.empty()) {
    error = std::string(name) + " is empty";
    return false;
  }
  if (named.count(folded) == 0 && trimmed.size() != 1) {
    error = std::string(name) + " must be a single character or one of: " +
            "space, pagedown, pageup, up, down";
    return false;
  }
  out = trimmed;
  return true;
}

}  // namespace

bool parse_keymap(std::string_view source, const std::string &path, Keymap &out,
                  std::string &error) {
  error.clear();
  out = Keymap{};
  out.profile_path = path;

  std::istringstream input{std::string(source)};
  std::string line;
  std::string section;
  int line_number = 0;
  bool default_profile_named = false;

  const auto fail = [&](const std::string &message) {
    error = path + ":" + std::to_string(line_number) + ": " + message;
    return false;
  };

  while (std::getline(input, line)) {
    ++line_number;
    // Strip a trailing comment, but not one inside a quoted value.
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
      const char ch = line[i];
      if (ch == '"' || ch == '\'') {
        in_quotes = !in_quotes;
      } else if ((ch == '#' || ch == ';') && !in_quotes) {
        line.resize(i);
        break;
      }
    }
    const std::string text = trim(line);
    if (text.empty()) continue;

    if (text.front() == '[') {
      if (text.back() != ']') return fail("unterminated section header");
      section = lower(trim(text.substr(1, text.size() - 2)));
      if (section.empty()) return fail("empty section name");
      continue;
    }

    const auto equals = text.find('=');
    if (equals == std::string::npos) {
      return fail("expected key = value, got: " + text);
    }
    const std::string key = lower(trim(text.substr(0, equals)));
    const std::string value = unquote(trim(text.substr(equals + 1)));
    if (key.empty()) return fail("empty key name");

    if (section == "devices") {
      DeviceId id;
      if (!parse_device_id(value, id, error)) {
        return fail(error);
      }
      if (!out.devices.emplace(id, key).second) {
        return fail("duplicate device: " + id.str());
      }
      continue;
    }
    if (section == "meta") {
      // The compositor's Meta prefix. Separate from [init] prefix, which is the
      // device-level remapping layer: see the comment on Keymap.
      if (key != "prefix") {
        return fail("unknown [meta] setting: " + key);
      }
      if (!parse_key_chord(value, out.meta_prefix, error)) {
        return fail("prefix: " + error);
      }
      out.meta_prefix_set = true;
      continue;
    }
    if (section == "init") {
      if (key == "prefix") {
        if (!parse_key_chord(value, out.prefix, error)) return fail("prefix: " + error);
      } else if (key == "action") {
        Action action = Action::parse(value, error);
        if (!error.empty()) return fail("action: " + error);
        if (action.kind != Action::Kind::trigger) {
          return fail("init action must be Trigger(profile)");
        }
        out.prefix_action = action;
      } else if (key == "default_profile") {
        out.default_profile = value;
        default_profile_named = true;
      } else if (key == "system_wide") {
        if (value != "true" && value != "false") {
          return fail("system_wide must be true or false");
        }
        out.system_wide = value == "true";
      } else {
        return fail("unknown [init] setting: " + key);
      }
      continue;
    }
    if (section == "diftray") {
      if (key == "repeat_rate" || key == "repeat_delay") {
        int parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        const int maximum = key == "repeat_rate" ? 100 : 5000;
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            parsed < 0 || parsed > maximum)
          return fail(key + " must be an integer between 0 and " + std::to_string(maximum));
        (key == "repeat_rate" ? out.repeat_rate : out.repeat_delay) = parsed;
        out.help_key_set = true;
        continue;
      }
      std::string *target = nullptr;
      if (key == "help_key_close") target = &out.help_key_close;
      else if (key == "help_key_search") target = &out.help_key_search;
      else if (key == "help_key_next") target = &out.help_key_next;
      else if (key == "help_key_previous") target = &out.help_key_previous;
      else if (key == "help_key_page_down") target = &out.help_key_page_down;
      else if (key == "help_key_page_up") target = &out.help_key_page_up;
      else if (key == "help_key_line_down") target = &out.help_key_line_down;
      else if (key == "help_key_line_up") target = &out.help_key_line_up;
      else {
        return fail("unknown [diftray] setting: " + key);
      }
      std::string message;
      if (!parse_help_key(value, key.c_str(), *target, message)) {
        return fail(message);
      }
      out.help_key_set = true;
      continue;
    }
    // Everything else is a profile section.
    if (section.empty()) {
      return fail("setting outside of any section: " + key);
    }
    Profile &profile = out.profiles[section];
    if (profile.name.empty()) profile.name = section;
    KeyChord chord;
    if (!parse_key_chord(key, chord, error)) {
      return fail("[" + section + "] key: " + error);
    }
    Action action = Action::parse(value, error);
    if (!error.empty()) {
      return fail("[" + section + "] " + error);
    }
    if (!profile.bindings.emplace(chord, action).second) {
      return fail("[" + section + "] duplicate binding for <" + chord.str() + ">");
    }
  }

  // An absent `default` profile is fine: in keyd an undefined profile means "no
  // bindings", which is what makes a profile-1-only keymap valid. Only a
  // default_profile that names something missing is an error. Checked before
  // the emptiness rule so the message names the actual mistake.
  if (default_profile_named && out.profiles.count(out.default_profile) == 0) {
    error = path + ": default_profile names an unknown profile: " + out.default_profile;
    return false;
  }
  // A file that configures only the device list, or only the help keys, is
  // legitimate: the system-wide backend works fine with no bindings. Only a
  // file that says nothing at all is rejected, because that is almost always a
  // wrong path rather than an intent.
  if (out.profiles.empty() && !out.prefix.code && out.devices.empty() &&
      !out.help_key_set) {
    error = path + ": keymap defines no profiles, no prefix and no devices";
    return false;
  }
  // Every Trigger() must name a profile that exists.
  for (const auto &[name, profile] : out.profiles) {
    for (const auto &[chord, action] : profile.bindings) {
      if (action.kind != Action::Kind::trigger) continue;
      if (!out.profiles.count(action.profile)) {
        error = path + ": [" + name + "] <" + chord.str() + "> triggers unknown profile: " +
                action.profile;
        return false;
      }
    }
  }
  if (out.prefix_action.kind == Action::Kind::trigger &&
      !out.profiles.count(out.prefix_action.profile)) {
    error = path + ": [init] prefix triggers unknown profile: " + out.prefix_action.profile;
    return false;
  }
  if (out.prefix.code && out.prefix_action.kind != Action::Kind::trigger) {
    error = path + ": [init] needs a prefix and an action = Trigger(profile)";
    return false;
  }
  return true;
}

bool load_keymap(const std::string &path, Keymap &out, std::string &error) {
  error.clear();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    error = "cannot read keymap: " + (ec ? ec.message() : path);
    return false;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = "cannot stat keymap: " + ec.message();
    return false;
  }
  if (size > kMaxKeymapBytes) {
    error = "keymap exceeds 1 MiB: " + path;
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open keymap: " + path;
    return false;
  }
  std::string source;
  source.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    input.read(source.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
      error = "short read on keymap: " + path;
      return false;
    }
  }
  if (source.find('\0') != std::string::npos) {
    error = "keymap contains NUL: " + path;
    return false;
  }
  return parse_keymap(source, path, out, error);
}
