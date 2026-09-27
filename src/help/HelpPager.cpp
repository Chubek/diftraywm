#include "help/HelpPager.hpp"

#include <oniguruma.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>

namespace {
std::once_flag oniguruma_initialization;

void initialize_oniguruma() {
  std::call_once(oniguruma_initialization, [] {
    OnigEncoding encodings[] = {ONIG_ENCODING_UTF8};
    onig_initialize(encodings, 1);
  });
}

std::string trim(std::string text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  const auto end = text.find_last_not_of(" \t\r\n");
  return begin == std::string::npos ? std::string{} : text.substr(begin, end - begin + 1);
}

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

std::string unquote(std::string text) {
  text = trim(std::move(text));
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    return text.substr(1, text.size() - 2);
  }
  return text;
}

std::string roff_text(std::string text) {
  for (std::size_t pos = 0; (pos = text.find("\\f", pos)) != std::string::npos;) {
    if (pos + 2 < text.size()) {
      text.erase(pos, 3);
    } else {
      text.erase(pos, 2);
    }
  }
  for (std::size_t pos = 0; (pos = text.find("\\-", pos)) != std::string::npos;) {
    text.replace(pos, 2, "-");
  }
  for (std::size_t pos = 0; (pos = text.find("\\&", pos)) != std::string::npos;) {
    text.erase(pos, 2);
  }
  return unquote(std::move(text));
}

std::vector<std::string> wrap_line(const std::string &line, std::size_t columns) {
  const std::size_t width = std::max<std::size_t>(1, columns);
  if (line.empty()) return {""};
  std::vector<std::string> wrapped;
  std::istringstream words(line);
  std::string word;
  std::string current;
  while (words >> word) {
    if (word.size() > width) {
      if (!current.empty()) {
        wrapped.push_back(std::move(current));
        current.clear();
      }
      for (std::size_t pos = 0; pos < word.size(); pos += width) {
        wrapped.push_back(word.substr(pos, width));
      }
      continue;
    }
    if (current.empty()) {
      current = word;
    } else if (current.size() + 1 + word.size() <= width) {
      current += ' ';
      current += word;
    } else {
      wrapped.push_back(std::move(current));
      current = word;
    }
  }
  if (!current.empty()) wrapped.push_back(std::move(current));
  return wrapped.empty() ? std::vector<std::string>{""} : wrapped;
}

std::string join_path(std::string_view path, const std::string &page) {
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = path.find(':', start);
    const auto piece = path.substr(start, end == std::string_view::npos ? path.size() - start
                                                                         : end - start);
    if (!piece.empty()) {
      const auto candidate = std::filesystem::path(piece) / (page + ".1");
      if (std::filesystem::is_regular_file(candidate)) return candidate.string();
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return {};
}
}

HelpPager::HelpPager(std::string search_path) : search_path_(std::move(search_path)) {}

void HelpPager::set_search_path(std::string search_path) {
  search_path_ = std::move(search_path);
}

bool HelpPager::has_page() const { return !page_name_.empty(); }
const std::string &HelpPager::page_name() const { return page_name_; }
const std::string &HelpPager::search_pattern() const { return search_pattern_; }
std::size_t HelpPager::match_count() const { return matches_.size(); }

bool HelpPager::open(const std::string &topic, std::string &message) {
  return load_page(topic.empty() ? "help-index" : topic, message);
}

bool HelpPager::load_page(const std::string &topic, std::string &message) {
  const std::string normalized = lowercase(topic);
  if (normalized.empty() || normalized.find_first_not_of(
                                "abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
    message = "help: invalid page name";
    return false;
  }
  const std::string path = join_path(search_path_, normalized);
  if (path.empty()) {
    message = "help: no page named " + normalized;
    return false;
  }
  std::ifstream input(path);
  std::ostringstream source;
  source << input.rdbuf();
  page_name_ = normalized;
  lines_ = format_roff(source.str());
  offset_ = 0;
  search_pattern_.clear();
  matches_.clear();
  selected_match_ = 0;
  message = "help: " + page_name_;
  return true;
}

std::vector<std::string> HelpPager::format_roff(const std::string &source) const {
  std::vector<std::string> output;
  std::istringstream input(source);
  for (std::string line; std::getline(input, line);) {
    if (line.rfind(".\\\"", 0) == 0) continue;
    if (line.empty()) {
      if (output.empty() || !output.back().empty()) output.emplace_back();
      continue;
    }
    if (line.front() != '.') {
      output.push_back(roff_text(line));
      continue;
    }
    const auto first_space = line.find_first_of(" \t", 1);
    const std::string macro = line.substr(1, first_space == std::string::npos
                                                  ? std::string::npos
                                                  : first_space - 1);
    const std::string argument = first_space == std::string::npos
                                     ? std::string{}
                                     : roff_text(line.substr(first_space + 1));
    if (macro == "TH") {
      std::istringstream words(argument);
      std::string title;
      std::string section;
      words >> title >> section;
      output.push_back(title + (section.empty() ? "" : "(" + section + ")"));
      output.push_back(std::string(std::max<std::size_t>(title.size() + section.size() + 2, 8), '='));
    } else if (macro == "SH") {
      if (!output.empty() && !output.back().empty()) output.emplace_back();
      output.push_back(lowercase(argument));
      std::transform(output.back().begin(), output.back().end(), output.back().begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
      });
    } else if (macro == "SS") {
      if (!output.empty() && !output.back().empty()) output.emplace_back();
      output.push_back(argument);
    } else if (macro == "PP" || macro == "P" || macro == "LP" || macro == "br") {
      if (output.empty() || !output.back().empty()) output.emplace_back();
    } else if (macro == "B" || macro == "I" || macro == "BI" || macro == "IB" ||
               macro == "BR" || macro == "RB" || macro == "IR" || macro == "RI") {
      output.push_back(argument);
    } else if (macro == "TP" || macro == "IP" || macro == "RS" || macro == "RE" ||
               macro == "nf" || macro == "fi") {
      continue;
    }
  }
  while (!output.empty() && output.back().empty()) output.pop_back();
  return output;
}

bool HelpPager::find(const std::string &pattern, std::string &message) {
  if (!has_page()) {
    message = "help: open a page before searching";
    return false;
  }
  if (pattern.empty()) {
    message = "help: search pattern is empty";
    return false;
  }
  initialize_oniguruma();
  OnigRegex regex = nullptr;
  OnigErrorInfo error_info {};
  const auto *begin = reinterpret_cast<const OnigUChar *>(pattern.data());
  const auto *end = begin + pattern.size();
  const int compile_status = onig_new(&regex, begin, end, ONIG_OPTION_NONE,
                                      ONIG_ENCODING_UTF8, ONIG_SYNTAX_DEFAULT, &error_info);
  if (compile_status != ONIG_NORMAL) {
    OnigUChar error[ONIG_MAX_ERROR_MESSAGE_LEN] {};
    onig_error_code_to_str(error, compile_status, &error_info);
    message = "help: invalid regex: " + std::string(reinterpret_cast<char *>(error));
    return false;
  }
  OnigRegion *region = onig_region_new();
  std::vector<Match> found;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    const auto *line_begin = reinterpret_cast<const OnigUChar *>(lines_[index].data());
    const auto *line_end = line_begin + lines_[index].size();
    const int search_status = onig_search(regex, line_begin, line_end, line_begin, line_end,
                                          region, ONIG_OPTION_NONE);
    if (search_status >= 0) {
      found.push_back({index});
    } else if (search_status != ONIG_MISMATCH) {
      OnigUChar error[ONIG_MAX_ERROR_MESSAGE_LEN] {};
      onig_error_code_to_str(error, search_status);
      onig_region_free(region, 1);
      onig_free(regex);
      message = "help: regex search failed: " + std::string(reinterpret_cast<char *>(error));
      return false;
    }
  }
  onig_region_free(region, 1);
  onig_free(regex);
  search_pattern_ = pattern;
  matches_ = std::move(found);
  selected_match_ = 0;
  if (matches_.empty()) {
    message = "help: pattern not found: " + pattern;
    return true;
  }
  move_to_line(matches_.front().line, 1);
  message = "help: " + std::to_string(matches_.size()) + " matching lines";
  return true;
}

void HelpPager::move_to_line(std::size_t line, std::size_t page_rows) {
  offset_ = std::min(line, maximum_offset(page_rows));
}

std::size_t HelpPager::maximum_offset(std::size_t page_rows) const {
  (void)page_rows;
  return lines_.empty() ? 0 : lines_.size() - 1;
}

bool HelpPager::next_match(std::string &message) {
  if (matches_.empty()) {
    message = "help: no active search results";
    return false;
  }
  selected_match_ = (selected_match_ + 1) % matches_.size();
  move_to_line(matches_[selected_match_].line, 1);
  message = "help: match " + std::to_string(selected_match_ + 1) + "/" +
            std::to_string(matches_.size());
  return true;
}

bool HelpPager::previous_match(std::string &message) {
  if (matches_.empty()) {
    message = "help: no active search results";
    return false;
  }
  selected_match_ = (selected_match_ + matches_.size() - 1) % matches_.size();
  move_to_line(matches_[selected_match_].line, 1);
  message = "help: match " + std::to_string(selected_match_ + 1) + "/" +
            std::to_string(matches_.size());
  return true;
}

bool HelpPager::scroll_lines(int delta, std::size_t page_rows, std::string &message) {
  if (!has_page()) return false;
  const auto max_offset = maximum_offset(page_rows);
  const auto next = static_cast<long long>(offset_) + delta;
  offset_ = static_cast<std::size_t>(std::clamp<long long>(next, 0, max_offset));
  message = "help: " + page_name_ + " line " + std::to_string(offset_ + 1);
  return true;
}

bool HelpPager::scroll_pages(int delta, std::size_t page_rows, std::string &message) {
  return scroll_lines(delta * static_cast<int>(std::max<std::size_t>(1, page_rows)),
                      page_rows, message);
}

bool HelpPager::set_bookmark(const std::string &name, std::string &message) {
  if (!has_page()) {
    message = "help: open a page before bookmarking it";
    return false;
  }
  if (name.empty()) {
    message = "help: bookmark name is required";
    return false;
  }
  bookmarks_[name] = page_name_;
  message = "help: bookmark " + name + " -> " + page_name_;
  return true;
}

bool HelpPager::open_bookmark(const std::string &name, std::string &message) {
  const auto bookmark = bookmarks_.find(name);
  if (bookmark == bookmarks_.end()) {
    message = "help: no bookmark named " + name;
    return false;
  }
  return load_page(bookmark->second, message);
}

std::string HelpPager::render(std::size_t columns, std::size_t rows) const {
  if (!has_page()) return {};
  const std::size_t body_rows = rows > 1 ? rows - 1 : 1;
  const std::size_t offset = std::min(offset_, maximum_offset(body_rows));
  std::vector<std::string> visible;
  visible.reserve(body_rows);
  for (std::size_t line = offset; line < lines_.size() && visible.size() < body_rows; ++line) {
    const auto wrapped = wrap_line(lines_[line], columns);
    for (const auto &part : wrapped) {
      if (visible.size() == body_rows) break;
      visible.push_back(part);
    }
  }
  std::ostringstream output;
  output << "\x1b[2J\x1b[H";
  for (std::size_t row = 0; row < body_rows; ++row) {
    if (row < visible.size()) output << visible[row];
    output << "\x1b[K";
    if (row + 1 < rows) output << '\n';
  }
  if (rows > 1) {
    output << "-- " << page_name_ << "  " << (offset + 1) << "/"
           << std::max<std::size_t>(1, lines_.size());
    if (!search_pattern_.empty()) output << "  /" << search_pattern_ << " (" << matches_.size() << ')';
    output << "  q: close  /: search  n/?: results\x1b[K";
  }
  return output.str();
}
