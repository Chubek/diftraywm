#include "theme/ThemeEngine.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <functional>
#include <system_error>
#include <sstream>
#include <map>
#include <utility>

namespace {
std::string trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return {};
  return std::string(text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1));
}
bool is_hex_color(std::string_view value) {
  if (value.empty() || value.front() != '#') return false;
  const auto hex = value.substr(1);
  return (hex.size() == 3 || hex.size() == 4 || hex.size() == 6 || hex.size() == 8) &&
      std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}
bool is_length_value(std::string_view value) {
  const auto unit_pos = value.find_first_not_of("0123456789.+-");
  if (unit_pos == std::string_view::npos) return false;
  double parsed = 0.0;
  const auto number = value.substr(0, unit_pos);
  const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
  if (result.ec != std::errc() || result.ptr != number.data() + number.size() ||
      !std::isfinite(parsed) || parsed < 0) return false;
  const auto unit = value.substr(unit_pos);
  return unit == "px" || unit == "em" || unit == "rem" || unit == "%" || unit == "vh" || unit == "vw";
}
bool number(std::string_view text, double &out) {
  auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc() && result.ptr == text.data() + text.size() && std::isfinite(out);
}
bool seconds(const std::string &text, double &out) {
  const bool ms = text.ends_with("ms");
  if (!ms && !text.ends_with('s')) return false;
  if (!number(std::string_view(text).substr(0, text.size() - (ms ? 2 : 1)), out) || out < 0) return false;
  if (ms) out /= 1000;
  return out <= 60;
}
bool timing(const std::string &text, AnimationSpec &spec) {
  if (text == "linear") { spec.linear = true; return true; }
  if (text == "ease") spec.curve = {0.25, 0.1, 0.25, 1};
  else if (text == "ease-in") spec.curve = {0.42, 0, 1, 1};
  else if (text == "ease-out") spec.curve = {0, 0, 0.58, 1};
  else if (text == "ease-in-out") spec.curve = {0.42, 0, 0.58, 1};
  else return false;
  spec.linear = false;
  return true;
}
bool extract_keyframes(std::string &css, ThemeProperties &parsed, std::string &error) {
  unsigned depth = 0;
  for (std::size_t pos = 0; pos < css.size();) {
    if (depth == 0 && std::string_view(css).substr(pos, 10) == "@keyframes") {
      const auto open = css.find('{', pos + 10);
      if (open == std::string::npos) { error = "missing keyframes block"; return false; }
      const auto name = trim(std::string_view(css).substr(pos + 10, open - pos - 10));
      if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-' || ch == '_';
          })) { error = "invalid keyframes name"; return false; }
      std::map<double, double> stops;
      auto cursor = open + 1;
      for (;;) {
        cursor = css.find_first_not_of(" \t\r\n", cursor);
        if (cursor == std::string::npos) { error = "unclosed keyframes block"; return false; }
        if (css[cursor] == '}') break;
        const auto frame_open = css.find('{', cursor);
        const auto frame_close = css.find('}', cursor);
        if (frame_open == std::string::npos || frame_close == std::string::npos || frame_close < frame_open ||
            css.find('{', frame_open + 1) < frame_close) { error = "invalid keyframe block"; return false; }
        auto selectors = std::string_view(css).substr(cursor, frame_open - cursor);
        const auto declarations = std::string_view(css).substr(frame_open + 1, frame_close - frame_open - 1);
        double opacity = 1;
        bool has_opacity = false;
        for (std::size_t begin = 0; begin < declarations.size();) {
          auto end = declarations.find(';', begin);
          if (end == std::string_view::npos) end = declarations.size();
          const auto declaration = trim(declarations.substr(begin, end - begin));
          begin = end + 1;
          if (declaration.empty()) continue;
          const auto colon = declaration.find(':');
          if (colon == std::string::npos || trim(std::string_view(declaration).substr(0, colon)) != "opacity" ||
              !number(trim(std::string_view(declaration).substr(colon + 1)), opacity)) {
            error = "keyframes currently support only numeric opacity"; return false;
          }
          opacity = std::clamp(opacity, 0.0, 1.0);
          has_opacity = true;
        }
        if (!has_opacity) { error = "keyframe needs opacity"; return false; }
        for (std::size_t begin = 0; begin < selectors.size();) {
          auto end = selectors.find(',', begin);
          if (end == std::string_view::npos) end = selectors.size();
          const auto selector = trim(selectors.substr(begin, end - begin));
          begin = end + 1;
          double offset = 0;
          if (selector == "from") offset = 0;
          else if (selector == "to") offset = 1;
          else if (selector.ends_with('%') && number(std::string_view(selector).substr(0, selector.size() - 1), offset) &&
                   offset >= 0 && offset <= 100) offset /= 100;
          else { error = "invalid keyframe offset"; return false; }
          stops[offset] = opacity;
        }
        cursor = frame_close + 1;
      }
      if (stops.empty()) { error = "empty keyframes block"; return false; }
      stops.try_emplace(0, 1);
      stops.try_emplace(1, 1);
      auto &frames = parsed.keyframes[name];
      frames.clear();
      for (const auto &[offset, opacity] : stops) frames.push_back({offset, opacity});
      css.replace(pos, cursor + 1 - pos, " ");
    } else {
      if (css[pos] == '{') ++depth;
      else if (css[pos] == '}' && depth) --depth;
      ++pos;
    }
  }
  return true;
}
bool compile_animation(ThemeProperties &parsed, std::string &error) {
  AnimationSpec spec;
  std::string name = "none";
  if (auto found = parsed.tokens.find("animation"); found != parsed.tokens.end()) {
    std::istringstream words(found->second);
    std::string word;
    unsigned times = 0;
    bool has_name = false;
    while (words >> word) {
      double time = 0;
      if (seconds(word, time)) {
        if (times == 0) spec.duration = time;
        else if (times == 1) spec.delay = time;
        else { error = "too many animation times"; return false; }
        ++times;
      } else if (timing(word, spec)) {
        // The timing function is applied separately between every pair of stops.
      } else if (word == "forwards" || word == "backwards" || word == "both") {
        spec.forwards = word == "forwards" || word == "both";
        spec.backwards = word == "backwards" || word == "both";
      } else if (!has_name) { name = word; has_name = true; }
      else { error = "unsupported animation shorthand token: " + word; return false; }
    }
  }
  for (const auto &[key, token] : parsed.tokens) {
    if (key == "animation-name") name = token;
    if (key == "animation-duration" && !seconds(token, spec.duration)) { error = "invalid animation duration"; return false; }
    if (key == "animation-delay" && !seconds(token, spec.delay)) { error = "invalid animation delay"; return false; }
    if (key == "animation-timing-function" && !timing(token, spec)) { error = "unsupported animation timing function"; return false; }
    if (key == "animation-fill-mode") {
      if (token != "none" && token != "forwards" && token != "backwards" && token != "both") { error = "invalid animation fill mode"; return false; }
      spec.forwards = token == "forwards" || token == "both";
      spec.backwards = token == "backwards" || token == "both";
    }
    if ((key == "animation-iteration-count" && token != "1") ||
        (key == "animation-direction" && token != "normal") ||
        (key == "animation-play-state" && token != "running")) {
      error = "unsupported " + key; return false;
    }
  }
  if (name != "none") {
    const auto found = parsed.keyframes.find(name);
    if (found == parsed.keyframes.end()) { error = "unknown animation: " + name; return false; }
    spec.frames = found->second;
  }
  parsed.cell_animation = std::move(spec);
  return true;
}

}

bool ThemeEngine::parse_css(std::string_view css, ThemeProperties &out) {
  last_error_.clear();
  if (css.size() > 1024 * 1024) {
    last_error_ = "theme exceeds 1 MiB";
    return false;
  }
  // Build a candidate. A failed reload must not expose a partially parsed theme.
  ThemeProperties parsed;
  std::string clean;
  for (std::size_t i = 0; i < css.size();) {
    if (css.substr(i, 2) == "/*") {
      const auto end = css.find("*/", i + 2);
      if (end == std::string_view::npos) {
        last_error_ = "unclosed CSS comment";
        return false;
      }
      clean.push_back(' ');
      i = end + 2;
    } else {
      clean.push_back(css[i++]);
    }
  }
  if (!extract_keyframes(clean, parsed, last_error_)) return false;
  std::string key, value, selector;
  bool in_value = false, in_block = false;
  auto commit = [&]() {
    const auto name = trim(key);
    const auto token = trim(value);
    key.clear(); value.clear();
    const bool had_colon = std::exchange(in_value, false);
    if (name.empty() && token.empty() && !had_colon) return true;
    if (name.empty() || token.empty() || !had_colon) {
      last_error_ = "incomplete CSS declaration";
      return false;
    }
    if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) {
      return std::isalnum(ch) || ch == '-' || ch == '_';
    })) {
      last_error_ = "invalid theme key: " + name;
      return false;
    }
    parsed.tokens[name] = token;
    return true;
  };
  for (char ch : clean) {
    if (!in_block) {
      if (ch == '{') {
        if (trim(selector).empty()) { last_error_ = "missing CSS selector"; return false; }
        selector.clear(); in_block = true;
      } else if (ch == '}' || ch == ';') {
        last_error_ = "declaration outside a CSS block";
        return false;
      } else selector.push_back(ch);
    } else if (ch == '{') {
      last_error_ = "nested CSS blocks are unsupported";
      return false;
    } else if (ch == ';' || ch == '}') {
      if (!commit()) return false;
      if (ch == '}') in_block = false;
    } else if (!in_value && ch == ':') in_value = true;
    else (in_value ? value : key).push_back(ch);
  }
  if (in_block || !trim(selector).empty()) {
    last_error_ = in_block ? "unclosed CSS block" : "missing CSS block";
    return false;
  }

  // Resolve root custom properties, including forward references and fallbacks.
  std::function<bool(std::string &, unsigned)> resolve = [&](std::string &token, unsigned depth) {
    if (depth > 32) { last_error_ = "cyclic or excessively nested CSS variable"; return false; }
    for (auto pos = token.find("var("); pos != std::string::npos; pos = token.find("var(")) {
      std::size_t end = pos + 4;
      unsigned nesting = 1;
      for (; end < token.size() && nesting; ++end) {
        if (token[end] == '(') ++nesting;
        else if (token[end] == ')') --nesting;
      }
      if (nesting) { last_error_ = "unclosed CSS variable"; return false; }
      const auto body = token.substr(pos + 4, end - pos - 5);
      const auto comma = body.find(',');
      const auto name = trim(body.substr(0, comma));
      if (name.size() <= 2 || name.substr(0, 2) != "--") {
        last_error_ = "invalid CSS variable: " + name; return false;
      }
      const auto found = parsed.tokens.find(name);
      std::string replacement;
      if (found != parsed.tokens.end()) replacement = found->second;
      else if (comma != std::string::npos) replacement = trim(body.substr(comma + 1));
      else { last_error_ = "undefined CSS variable: " + name; return false; }
      if (!resolve(replacement, depth + 1)) return false;
      token.replace(pos, end - pos, replacement);
      if (token.size() > 64 * 1024) { last_error_ = "CSS variable expansion too large"; return false; }
    }
    return true;
  };
  for (auto &[name, token] : parsed.tokens) {
    if (!resolve(token, 0)) return false;
    if (name.starts_with("--")) continue;
    const bool color = name.find("color") != std::string::npos || token.front() == '#';
    const bool metric = name.find("radius") != std::string::npos || name.find("size") != std::string::npos ||
        name.find("spacing") != std::string::npos || name.find("height") != std::string::npos ||
        name.find("width") != std::string::npos;
    if (color && !is_hex_color(token)) {
      last_error_ = "invalid color value for " + name; return false;
    }
    if (metric && !is_length_value(token) && token != "0") {
      last_error_ = "invalid metric value for " + name; return false;
    }
    if (color) parsed.colors[name] = token;
    else if (metric) parsed.metrics[name] = token;
    else parsed.animations[name] = token;
  }
  if (!compile_animation(parsed, last_error_)) return false;
  parsed.tokens["css"] = std::string(css);
  out = std::move(parsed);
  return true;
}

const ThemeProperties &ThemeEngine::active() const { return active_; }
void ThemeEngine::swap_active(ThemeProperties properties) { active_ = std::move(properties); }
const std::string &ThemeEngine::last_error() const { return last_error_; }
