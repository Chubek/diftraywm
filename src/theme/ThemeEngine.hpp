#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

struct ThemeProperties {
  std::unordered_map<std::string, std::string> tokens;
  std::unordered_map<std::string, std::string> colors;
  std::unordered_map<std::string, std::string> metrics;
  std::unordered_map<std::string, std::string> animations;
};

class ThemeEngine {
public:
  bool parse_css(std::string_view css, ThemeProperties &out);
  const ThemeProperties &active() const;
  void swap_active(ThemeProperties properties);
  const std::string &last_error() const;

private:
  ThemeProperties active_;
  std::string last_error_;
};
