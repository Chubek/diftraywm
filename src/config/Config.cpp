#include "config/Config.hpp"

#include <tao/pegtl.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>
#include <filesystem>
#include <map>
#include <memory>
#include <cmath>
#include <toml.h>
#include "config/YamlConfig.h"

namespace {
namespace pegtl = tao::pegtl;

struct space : pegtl::star<pegtl::blank> {};
struct identifier : pegtl::plus<pegtl::sor<pegtl::alnum, pegtl::one<'_', '-'>>> {};
struct value : pegtl::plus<pegtl::not_one<'\n', '\r', '}'>> {};
struct assignment
    : pegtl::seq<space, identifier, space, pegtl::one<'='>, space, value,
                 pegtl::opt<pegtl::eolf>> {};
struct blank_line : pegtl::seq<space, pegtl::eol> {};
struct comment_line : pegtl::seq<space, pegtl::one<'#'>, pegtl::until<pegtl::eolf>> {};
struct section
    : pegtl::seq<space, identifier, space, pegtl::one<'{'>, pegtl::eolf,
                 pegtl::star<pegtl::sor<assignment, blank_line, comment_line>>, space, pegtl::one<'}'>,
                 pegtl::opt<pegtl::eolf>> {};
struct program_string : pegtl::seq<pegtl::one<'"'>,
    pegtl::star<pegtl::sor<pegtl::seq<pegtl::one<'\\'>, pegtl::any>, pegtl::not_one<'"'>>>,
    pegtl::one<'"'>> {};
struct program_comment : pegtl::seq<pegtl::one<'#'>, pegtl::until<pegtl::eolf>> {};
struct program_block : pegtl::seq<space, TAO_PEGTL_STRING("program"), space, pegtl::one<'{'>,
    pegtl::star<pegtl::sor<program_string, program_comment, pegtl::not_one<'}'>>>,
    pegtl::one<'}'>, pegtl::opt<pegtl::eolf>> {};
struct ProgramSource { std::string text; size_t offset = 0, length = 0; bool found = false; };
template<class Rule> struct config_action : pegtl::nothing<Rule> {};
template<> struct config_action<program_block> {
  template<class Input> static void apply(const Input &input, ProgramSource &out) {
    if (out.found) throw std::runtime_error("duplicate program block");
    const auto text = input.string();
    const auto begin = text.find('{');
    out.text = text.substr(begin + 1, text.rfind('}') - begin - 1);
    out.offset = input.position().byte; out.length = text.size(); out.found = true;
  }
};
struct grammar : pegtl::must<pegtl::star<pegtl::sor<program_block, section, blank_line, comment_line>>, pegtl::eof> {};

std::string trim(std::string text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  const auto end = text.find_last_not_of(" \t\r\n");
  return begin == std::string::npos ? std::string{} : text.substr(begin, end - begin + 1);
}

bool parse_int(std::string_view value, int &out) {
  const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
  return result.ec == std::errc() && result.ptr == value.data() + value.size();
}

// The DSL and YAML deliver every setting as text and TOML booleans are
// normalised above, so booleans are spelled the same way in all three formats.
bool parse_bool(std::string_view value, bool &out) {
  if (value == "true" || value == "yes" || value == "on" || value == "1") {
    out = true;
    return true;
  }
  if (value == "false" || value == "no" || value == "off" || value == "0") {
    out = false;
    return true;
  }
  return false;
}

bool parse_color(std::string_view value, float out[4]) {
  if (value.size() != 7 && value.size() != 9) {
    return false;
  }
  if (value.front() != '#') {
    return false;
  }
  unsigned int channels[4] = {0, 0, 0, 255};
  for (std::size_t index = 0; index < (value.size() - 1) / 2; ++index) {
    const auto component = value.substr(1 + index * 2, 2);
    const auto result = std::from_chars(component.data(), component.data() + component.size(),
                                        channels[index], 16);
    if (result.ec != std::errc() || result.ptr != component.data() + component.size()) {
      return false;
    }
  }
  for (std::size_t index = 0; index < 4; ++index) {
    out[index] = static_cast<float>(channels[index]) / 255.0f;
  }
  return true;
}
}

bool parse_monitor_settings(const std::map<std::string, std::string> &settings,
                            MonitorConfig &monitor, std::string &error) {
  auto candidate = monitor;
  if (settings.count("x") != settings.count("y")) {
    error = "monitor position requires both x and y"; return false;
  }
  for (const auto &[key, value] : settings) {
    if (key == "name") candidate.name = value;
    else if (key == "rotation") {
      if (!parse_int(value, candidate.rotation) || candidate.rotation < 0 ||
          candidate.rotation > 270 || candidate.rotation % 90 != 0) {
        error = "monitor rotation must be 0, 90, 180 or 270"; return false;
      }
    } else if (key == "scale") {
      const auto result = std::from_chars(value.data(), value.data() + value.size(), candidate.scale);
      if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
          !std::isfinite(candidate.scale) || candidate.scale < 0.5f || candidate.scale > 4.f) {
        error = "monitor scale must be a finite number from 0.5 to 4"; return false;
      }
    } else if (key == "x" || key == "y") {
      int &coordinate = key == "x" ? candidate.x : candidate.y;
      if (!parse_int(value, coordinate) || coordinate < -100000 || coordinate > 100000) {
        error = "monitor coordinates must be integers from -100000 to 100000"; return false;
      }
      candidate.positioned = true;
    } else { error = "unknown monitor setting: " + key; return false; }
  }
  if (candidate.name.empty() || candidate.name.size() > 256 ||
      candidate.name.find_first_of("\r\n\t") != std::string::npos) {
    error = "monitor name is required and must be at most 256 characters"; return false;
  }
  monitor = std::move(candidate);
  return true;
}

namespace {
using Settings = std::map<std::string, std::string>;
bool apply(const Settings &settings, CompositorConfig &config, std::string &error) {
  for (const auto &[key, value] : settings) {
    if (key == "border_size") {
      if (!parse_int(value, config.border_size) || config.border_size < 1) {
        error = "border_size must be a positive integer";
        return false;
      }
    } else if (key == "command_bar_height") {
      if (!parse_int(value, config.command_bar_height) || config.command_bar_height < 1) {
        error = "command_bar_height must be a positive integer";
        return false;
      }
    } else if (key == "status_bar_height") {
      if (!parse_int(value, config.status_bar_height) || config.status_bar_height < 1) {
        error = "status_bar_height must be a positive integer";
        return false;
      }
    } else if (key == "launcher_bar_height") {
      if (!parse_int(value, config.launcher_bar_height) || config.launcher_bar_height < 1) {
        error = "launcher_bar_height must be a positive integer";
        return false;
      }
    } else if (key == "border_color") {
      if (!parse_color(value, config.border_color)) {
        error = "border_color must be #RRGGBB or #RRGGBBAA";
        return false;
      }
    } else if (key == "background_color") {
      if (!parse_color(value, config.background_color)) {
        error = "background_color must be #RRGGBB or #RRGGBBAA";
        return false;
      }
    } else if (key == "command_bar_color") {
      if (!parse_color(value, config.command_bar_color)) {
        error = "command_bar_color must be #RRGGBB or #RRGGBBAA";
        return false;
      }
    } else if (key == "launcher_bar_color") {
      if (!parse_color(value, config.launcher_bar_color)) {
        error = "launcher_bar_color must be #RRGGBB or #RRGGBBAA";
        return false;
      }
    } else if (key == "launcher_locked") {
      if (!parse_bool(value, config.launcher_locked)) {
        error = "launcher_locked must be true or false";
        return false;
      }
    } else if (key == "shell") {
      if (value.empty()) { error = "shell cannot be empty"; return false; }
      config.shell = value;
    } else if (key == "font") {
      if (value.empty()) { error = "font cannot be empty"; return false; }
      config.font = value;
    } else if (key == "font_size") {
      if (!parse_int(value, config.font_size) || config.font_size < 8) {
        error = "font_size must be an integer >= 8";
        return false;
      }
    } else if (key == "font_ligatures") {
      if (!parse_bool(value, config.font_ligatures)) {
        error = "font_ligatures must be true or false";
        return false;
      }
    } else if (key == "ncursor_mode" || key == "gcursor_mode") {
      if (value != "stack" && value != "tab") {
        error = key + " must be stack or tab";
        return false;
      }
      if (key == "ncursor_mode") {
        config.ncursor_mode = value;
      } else {
        config.gcursor_mode = value;
      }
    } else if (key == "word_pool") {
      config.word_pool = value;
    } else if (key == "theme") {
      config.theme = value;
    } else if (key == "help_path") {
      config.help_path = value;
    } else if (key == "keymap") {
      config.keymap = value;
    } else {
      error = "unknown setting: " + key;
      return false;
    }
  }
  return true;
}
bool add(Settings &settings, const std::string &key, std::string value, std::string &error) {
  if (!settings.emplace(key, std::move(value)).second) {
    error = "duplicate setting: " + key;
    return false;
  }
  return true;
}
bool parse_toml(std::string source, Settings &settings, std::vector<Settings> &monitors, std::string &program, std::string &error) {
  char diagnostic[512]{};
  std::unique_ptr<toml_table_t, decltype(&toml_free)> root(
      toml_parse(source.data(), diagnostic, sizeof(diagnostic)), toml_free);
  if (!root) { error = diagnostic; return false; }
  for (int section_index = 0; const char *section = toml_key_in(root.get(), section_index); ++section_index) {
    if (std::string_view(section) == "program") {
      auto value = toml_string_in(root.get(), section);
      if (!value.ok) { error = "program must be a string"; return false; }
      program = value.u.s; std::free(value.u.s); continue;
    }
    if (std::string_view(section) == "monitors") {
      auto *array = toml_array_in(root.get(), section);
      if (!array || toml_array_nelem(array) > 64) { error = "monitors must be an array of at most 64 tables"; return false; }
      for (int i = 0; i < toml_array_nelem(array); ++i) {
        auto *table = toml_table_at(array, i);
        if (!table) { error = "monitor must be a table"; return false; }
        Settings fields;
        for (int j = 0; const char *key = toml_key_in(table, j); ++j) {
          auto value = toml_string_in(table, key);
          std::string text;
          if (value.ok) { text = value.u.s; std::free(value.u.s); }
          else if ((value = toml_int_in(table, key)).ok) text = std::to_string(value.u.i);
          else if (std::string_view(key) == "scale" && (value = toml_double_in(table, key)).ok) {
            char number[64];
            const auto result = std::to_chars(number, number + sizeof(number), value.u.d);
            if (result.ec != std::errc()) { error = "invalid monitor scale"; return false; }
            text.assign(number, result.ptr);
          } else { error = "invalid monitor value: " + std::string(key); return false; }
          fields.emplace(key, std::move(text));
        }
        monitors.push_back(std::move(fields));
      }
      continue;
    }
    auto *table = toml_table_in(root.get(), section);
    if (!table || (std::string_view(section) != "general" && std::string_view(section) != "terminal")) {
      error = "expected general or terminal table: " + std::string(section); return false;
    }
    for (int index = 0; const char *key = toml_key_in(table, index); ++index) {
      auto value = toml_string_in(table, key);
      std::string text;
      if (value.ok) { text = value.u.s; std::free(value.u.s); }
      else {
        value = toml_int_in(table, key);
        if (!value.ok) {
          // Booleans arrive unquoted in TOML, so they need their own read
          // before the field can be rejected as an unsupported type.
          const auto flag = toml_bool_in(table, key);
          if (flag.ok) { text = flag.u.b ? "true" : "false"; }
          else { error = "expected string, integer or boolean: " + std::string(key); return false; }
        } else {
          text = std::to_string(value.u.i);
        }
      }
      if (!add(settings, key, std::move(text), error)) return false;
    }
  }
  return true;
}
bool parse_yaml(const std::string &source, Settings &settings, std::vector<Settings> &monitors, std::string &program, std::string &error) {
  DiftrayYaml *document = nullptr;
  char diagnostic[512]{};
  if (!diftray_yaml_load(source.data(), source.size(), &document, diagnostic, sizeof(diagnostic))) {
    error = diagnostic; return false;
  }
  std::unique_ptr<DiftrayYaml, decltype(&diftray_yaml_free)> owned(document, diftray_yaml_free);
  if (!document) return true;
  if (document->program) program = document->program;
  for (unsigned i = 0; i < document->monitors_count; ++i) {
    const auto &monitor = document->monitors[i];
    Settings fields;
    for (const auto &[key, value] : {std::pair{"name", monitor.name}, {"rotation", monitor.rotation},
                                  {"scale", monitor.scale}, {"x", monitor.x}, {"y", monitor.y}})
      if (value) fields.emplace(key, value);
    monitors.push_back(std::move(fields));
  }
  for (auto *section : {document->general, document->terminal}) {
    if (!section) continue;
#define CONFIG_FIELD(key) if (section->key && !add(settings, #key, section->key, error)) return false;
#include "config/ConfigFields.def"
#undef CONFIG_FIELD
  }
  return true;
}
}

bool load_compositor_config(const std::string &path, CompositorConfig &config,
                            std::string &error) {
  error.clear();
  std::ifstream input(path, std::ios::binary);
  if (!input) { error = "cannot open configuration: " + path; return false; }
  std::string source;
  char buffer[4096];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    source.append(buffer, input.gcount());
    if (source.size() > 1024 * 1024) { error = "configuration exceeds 1 MiB"; return false; }
  }
  if (input.bad()) { error = "cannot read configuration: " + path; return false; }
  if (source.find('\0') != std::string::npos) { error = "configuration contains NUL"; return false; }
  Settings settings;
  std::vector<Settings> monitors;
  std::string program;
  const auto extension = std::filesystem::path(path).extension();
  if (extension == ".toml") {
    if (!parse_toml(source, settings, monitors, program, error)) return false;
  } else if (extension == ".yaml" || extension == ".yml") {
    if (!parse_yaml(source, settings, monitors, program, error)) return false;
  } else {
    try {
      pegtl::memory_input parser_input(source, path);
      ProgramSource extracted;
      pegtl::parse<grammar, config_action>(parser_input, extracted);
      program = std::move(extracted.text);
      for (size_t i = extracted.offset; i < extracted.offset + extracted.length; ++i)
        if (source[i] != '\n' && source[i] != '\r') source[i] = ' ';
    } catch (const std::exception &exception) { error = exception.what(); return false; }
    Settings *current = &settings;
    std::istringstream lines(source);
    for (std::string line; std::getline(lines, line);) {
      line = trim(std::move(line));
      if (line.empty() || line.front() == '#') continue;
      const auto brace = line.find('{');
      const auto equals = line.find('=');
      if (brace != std::string::npos && equals == std::string::npos) {
        const auto section = trim(line.substr(0, brace));
        if (section == "monitor") {
          if (monitors.size() >= 64) { error = "at most 64 monitors may be configured"; return false; }
          monitors.emplace_back(); current = &monitors.back();
        } else if (section == "general" || section == "terminal") current = &settings;
        else {
          error = "unknown section: " + section; return false;
        }
      }
      if (equals == std::string::npos) continue;
      if (!add(*current, trim(line.substr(0, equals)), trim(line.substr(equals + 1)), error)) return false;
    }
  }
  auto candidate = config;
  candidate.program = ConfigProgram::compile(program, path + ":program", error);
  if (!candidate.program) return false;
  auto expand = [&](Settings &fields) {
    for (auto &[key, value] : fields) {
      std::string expanded;
      if (!candidate.program->expand(value, expanded, error)) { error = path + ":" + key + ": " + error; return false; }
      value = std::move(expanded);
    }
    return true;
  };
  if (!expand(settings)) return false;
  for (auto &fields : monitors) if (!expand(fields)) return false;
  if (!apply(settings, candidate, error)) { error = path + ": " + error; return false; }
  candidate.monitors.clear();
  for (const auto &fields : monitors) {
    MonitorConfig monitor;
    if (!parse_monitor_settings(fields, monitor, error)) { error = path + ": " + error; return false; }
    if (std::any_of(candidate.monitors.begin(), candidate.monitors.end(), [&](const auto &m) { return m.name == monitor.name; })) {
      error = path + ": duplicate monitor: " + monitor.name; return false;
    }
    candidate.monitors.push_back(std::move(monitor));
  }
  config = std::move(candidate);
  return true;
}
