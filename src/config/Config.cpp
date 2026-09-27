#include "config/Config.hpp"

#include <tao/pegtl.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>

namespace {
namespace pegtl = tao::pegtl;

struct space : pegtl::star<pegtl::blank> {};
struct identifier : pegtl::plus<pegtl::sor<pegtl::alnum, pegtl::one<'_', '-'>>> {};
struct value : pegtl::plus<pegtl::not_one<'\n', '\r', '}'>> {};
struct assignment
    : pegtl::seq<space, identifier, space, pegtl::one<'='>, space, value,
                 pegtl::opt<pegtl::eolf>> {};
struct blank_line : pegtl::seq<space, pegtl::eol> {};
struct section
    : pegtl::seq<space, identifier, space, pegtl::one<'{'>, pegtl::eolf,
                 pegtl::star<assignment>, space, pegtl::one<'}'>,
                 pegtl::opt<pegtl::eolf>> {};
struct grammar : pegtl::must<pegtl::star<pegtl::sor<section, blank_line>>, pegtl::eof> {};

std::string trim(std::string text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  const auto end = text.find_last_not_of(" \t\r\n");
  return begin == std::string::npos ? std::string{} : text.substr(begin, end - begin + 1);
}

bool parse_int(std::string_view value, int &out) {
  const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
  return result.ec == std::errc() && result.ptr == value.data() + value.size();
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

bool load_compositor_config(const std::string &path, CompositorConfig &config,
                            std::string &error) {
  std::ifstream input(path);
  if (!input) {
    return true;
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  const std::string source = contents.str();
  try {
    pegtl::memory_input parser_input(source, path);
    pegtl::parse<grammar>(parser_input);
  } catch (const pegtl::parse_error &exception) {
    error = exception.what();
    return false;
  }

  std::istringstream lines(source);
  for (std::string line; std::getline(lines, line);) {
    line = trim(std::move(line));
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = trim(line.substr(0, equals));
    const std::string value = trim(line.substr(equals + 1));
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
    } else if (key == "shell") {
      config.shell = value;
    } else if (key == "font") {
      config.font = value;
    } else if (key == "font_size") {
      if (!parse_int(value, config.font_size) || config.font_size < 8) {
        error = "font_size must be an integer >= 8";
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
    }
  }
  return true;
}
