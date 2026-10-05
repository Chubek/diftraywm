#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <array>
#include <vector>

struct AnimationKeyframe {
  double offset = 0;
  double opacity = 1;
};

struct AnimationSpec {
  double duration = 0;
  double delay = 0;
  std::array<double, 4> curve = {0.25, 0.1, 0.25, 1};
  bool linear = false;
  bool forwards = false;
  bool backwards = false;
  std::vector<AnimationKeyframe> frames;
};

struct ThemeProperties {
  std::unordered_map<std::string, std::string> tokens;
  std::unordered_map<std::string, std::string> colors;
  std::unordered_map<std::string, std::string> metrics;
  std::unordered_map<std::string, std::string> animations;
  std::unordered_map<std::string, std::vector<AnimationKeyframe>> keyframes;
  AnimationSpec cell_animation;
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
