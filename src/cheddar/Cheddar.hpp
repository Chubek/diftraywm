#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace diftray::cheddar {

struct Piece { bool added = false; std::size_t offset = 0, length = 0; };

class PieceTable {
public:
  PieceTable() = default;
  explicit PieceTable(std::string text);
  void reset(std::string text);
  std::size_t size() const;
  std::string str() const;
  std::string line(std::size_t index) const;
  std::size_t line_count() const;
  void insert(std::size_t position, std::string_view text);
  void erase(std::size_t position, std::size_t length);
  bool undo();
  bool redo();
private:
  std::string original_, added_;
  std::vector<Piece> pieces_;
  std::vector<std::string> undo_text_, redo_text_;
  std::string materialize() const;
  void load_without_history(std::string text);
  void split(std::size_t position);
  void remember();
};

struct Highlight { std::size_t start = 0, length = 0; std::string kind; };
struct Completion { std::string label, insert_text, detail; };

class LspClient {
public:
  using Send = std::function<void(std::string_view)>;
  explicit LspClient(Send send = {}): send_(std::move(send)) {}
  void set_sender(Send send) { send_ = std::move(send); }
  std::uint64_t request(std::string method, std::string params);
  void feed(std::string_view bytes);
  std::optional<std::string> take_response(std::uint64_t id);
private:
  Send send_;
  std::uint64_t next_id_ = 1;
  std::string input_;
  std::map<std::uint64_t, std::string> responses_;
};

struct LanguageProfile {
  std::string name;
  std::vector<std::string> keywords, builtins, commands;
  std::string line_comment;
  std::function<std::vector<Highlight>(std::string_view)> highlight;
  std::function<std::size_t(std::string_view, std::size_t)> indent;
  std::function<std::vector<Completion>(std::string_view, std::size_t)> complete;
};

LanguageProfile shell_profile();
LanguageProfile profile_for_path(const std::filesystem::path &path);

class Editor {
public:
  bool open(const std::filesystem::path &path, std::string *error = nullptr);
  bool save(std::string *error = nullptr);
  void close();
  bool active() const { return active_; }
  bool dispatch_mode() const { return dispatch_mode_; }
  const std::filesystem::path &path() const { return path_; }
  PieceTable &buffer() { return buffer_; }
  const PieceTable &buffer() const { return buffer_; }
  const LanguageProfile &profile() const { return profile_; }
  std::vector<Highlight> highlights(std::size_t line) const;
  std::vector<Completion> complete(std::size_t line, std::size_t column) const;
  void set_dispatch_mode(bool enabled) { dispatch_mode_ = enabled; }
  void map_key(std::string key, std::string action);
  void shortcut(std::string key, std::string command);
  void abbreviation(std::string from, std::string to);
  std::optional<std::string> key_action(std::string_view key) const;
  std::optional<std::string> expand(std::string_view word) const;
  std::string diff() const;
private:
  bool active_ = false, dispatch_mode_ = false;
  std::filesystem::path path_;
  PieceTable buffer_, saved_;
  LanguageProfile profile_ = shell_profile();
  std::map<std::string, std::string, std::less<>> keys_, shortcuts_, abbreviations_;
};

} // namespace diftray::cheddar
