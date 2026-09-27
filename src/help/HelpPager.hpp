#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// A small, roff-aware pager for the compositor's bundled help pages.  It owns
// no Wayland objects: the compositor decides where its rendered text appears.
class HelpPager {
public:
  explicit HelpPager(std::string search_path = {});

  void set_search_path(std::string search_path);
  bool open(const std::string &topic, std::string &message);
  bool find(const std::string &pattern, std::string &message);
  bool next_match(std::string &message);
  bool previous_match(std::string &message);
  bool scroll_lines(int delta, std::size_t page_rows, std::string &message);
  bool scroll_pages(int delta, std::size_t page_rows, std::string &message);
  bool set_bookmark(const std::string &name, std::string &message);
  bool open_bookmark(const std::string &name, std::string &message);

  std::string render(std::size_t columns, std::size_t rows) const;
  bool has_page() const;
  const std::string &page_name() const;
  const std::string &search_pattern() const;
  std::size_t match_count() const;

private:
  struct Match {
    std::size_t line = 0;
  };

  bool load_page(const std::string &topic, std::string &message);
  void move_to_line(std::size_t line, std::size_t page_rows);
  std::size_t maximum_offset(std::size_t page_rows) const;
  std::vector<std::string> format_roff(const std::string &source) const;

  std::string search_path_;
  std::string page_name_;
  std::vector<std::string> lines_;
  std::size_t offset_ = 0;
  std::string search_pattern_;
  std::vector<Match> matches_;
  std::size_t selected_match_ = 0;
  std::map<std::string, std::string> bookmarks_;
};
