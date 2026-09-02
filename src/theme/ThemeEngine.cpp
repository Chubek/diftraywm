#include "theme/ThemeEngine.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#include <utility>

namespace {
std::string trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(begin, end - begin + 1));
}

bool is_hex_color(std::string_view value) {
  if (value.empty() || value.front() != '#') {
    return false;
  }
  const auto hex = value.substr(1);
  if (!(hex.size() == 3 || hex.size() == 4 || hex.size() == 6 || hex.size() == 8)) {
    return false;
  }
  return std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool is_length_value(std::string_view value) {
  const auto unit_pos = value.find_first_not_of("0123456789.+-");
  if (unit_pos == std::string_view::npos) {
    return false;
  }
  double parsed = 0.0;
  const auto number = value.substr(0, unit_pos);
  const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
  if (result.ec != std::errc()) {
    return false;
  }
  const auto unit = value.substr(unit_pos);
  return unit == "px" || unit == "em" || unit == "rem" || unit == "%" || unit == "vh" || unit == "vw";
}
}

bool ThemeEngine::parse_css(std::string_view css, ThemeProperties &out) {
  last_error_.clear();
  out = {};

  std::string key;
  std::string value;
  bool in_value = false;
  bool in_block = false;

  auto commit = [&]() -> bool {
    const auto trimmed_key = trim(key);
    const auto trimmed_value = trim(value);
    key.clear();
    value.clear();
    in_value = false;
    if (trimmed_key.empty() || trimmed_value.empty()) {
      return true;
    }
    const auto valid_key = std::all_of(trimmed_key.begin(), trimmed_key.end(), [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
    });
    if (!valid_key) {
      last_error_ = "invalid theme key: " + trimmed_key;
      return false;
    }
    const bool looks_like_color = trimmed_key.find("color") != std::string::npos || trimmed_value.front() == '#';
    const bool looks_like_metric = trimmed_key.find("radius") != std::string::npos ||
                                   trimmed_key.find("size") != std::string::npos ||
                                   trimmed_key.find("spacing") != std::string::npos;
    if (looks_like_color && !is_hex_color(trimmed_value)) {
      last_error_ = "invalid color value for " + trimmed_key;
      return false;
    }
    if (looks_like_metric && !(is_length_value(trimmed_value) || trimmed_value == "0")) {
      last_error_ = "invalid metric value for " + trimmed_key;
      return false;
    }
    out.tokens[trimmed_key] = trimmed_value;
    if (looks_like_color) {
      out.colors[trimmed_key] = trimmed_value;
    } else if (looks_like_metric) {
      out.metrics[trimmed_key] = trimmed_value;
    } else {
      out.animations[trimmed_key] = trimmed_value;
    }
    return true;
  };

  for (char ch : css) {
    if (ch == '{') {
      in_block = true;
      continue;
    }
    if (ch == '}') {
      if (!commit()) {
        return false;
      }
      in_block = false;
      continue;
    }
    if (ch == ';') {
      if (!commit()) {
        return false;
      }
      continue;
    }
    if (!in_value && ch == ':') {
      in_value = true;
      continue;
    }
    if (in_value) {
      value.push_back(ch);
    } else if (in_block || !std::isspace(static_cast<unsigned char>(ch))) {
      key.push_back(ch);
    }
  }

  if (!commit()) {
    return false;
  }
  out.tokens["css"] = std::string(css);
  return true;
}

const ThemeProperties &ThemeEngine::active() const { return active_; }
void ThemeEngine::swap_active(ThemeProperties properties) { active_ = std::move(properties); }
const std::string &ThemeEngine::last_error() const { return last_error_; }
