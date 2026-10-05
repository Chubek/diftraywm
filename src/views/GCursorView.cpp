#include "views/GCursorView.hpp"

#include "compositor/Compositor.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace {
std::vector<std::string> load_words();

struct WordPool {
  std::mutex mutex;
  std::unordered_set<std::string> live_ids;
  std::vector<std::string> words;

  WordPool() : words(load_words()) {}
};

WordPool &pool() {
  static WordPool instance;
  return instance;
}

std::vector<std::string> load_words() {
  std::vector<std::string> words;
  const char *candidates[] = {
      std::getenv("DIFTRAYWM_WORD_POOL"),
      "/usr/share/dict/words",
      "/usr/share/dict/web2",
  };
  for (const char *candidate : candidates) {
    if (!candidate || !*candidate) {
      continue;
    }
    std::ifstream input(candidate);
    if (!input) {
      continue;
    }
    for (std::string word; input >> word;) {
      if (word.size() >= 3 && word.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-") == std::string::npos) {
        words.push_back(word);
      }
    }
    if (!words.empty()) {
      break;
    }
  }
  if (words.empty()) {
    words = {"atlas", "birch", "cipher", "dawn", "ember", "falcon", "glade"};
  }
  return words;
}

std::string pick_word(std::string desired) {
  auto &word_pool = pool();
  static std::mt19937 rng{std::random_device{}()};
  std::lock_guard<std::mutex> lock(word_pool.mutex);
  if (!desired.empty() && word_pool.live_ids.insert(desired).second) {
    return desired;
  }
  std::uniform_int_distribution<std::size_t> pick(0, word_pool.words.empty() ? 0 : word_pool.words.size() - 1);
  for (std::size_t attempt = 0; attempt < word_pool.words.size() * 2 + 1; ++attempt) {
    const auto &candidate = word_pool.words[pick(rng)];
    if (word_pool.live_ids.insert(candidate).second) {
      return candidate;
    }
  }
  // The dictionary can be shorter than the number of live windows.
  for (std::size_t suffix = 1;; ++suffix) {
    std::string candidate = "cursor-" + std::to_string(suffix);
    if (word_pool.live_ids.insert(candidate).second) {
      return candidate;
    }
  }
}

void release_word(const std::string &word) {
  auto &word_pool = pool();
  std::lock_guard<std::mutex> lock(word_pool.mutex);
  word_pool.live_ids.erase(word);
}
}

GCursorView::GCursorView(Compositor *compositor, std::string word_id)
    : word_id_(pick_word(std::move(word_id))), compositor_(compositor) {}

GCursorView::~GCursorView() { release_word(word_id_); }
void GCursorView::reload_word_pool() {
  auto words = load_words();
  auto &word_pool = pool();
  std::lock_guard<std::mutex> lock(word_pool.mutex);
  word_pool.words = std::move(words);
}
ViewType GCursorView::type() const { return ViewType::GCURSOR; }
const std::string &GCursorView::word_id() const { return word_id_; }
bool GCursorView::docked() const { return docked_; }
std::optional<int> GCursorView::quick_restore_slot() const { return quick_restore_slot_; }
void GCursorView::set_quick_restore_slot(std::optional<int> slot) { quick_restore_slot_ = slot; }
void GCursorView::set_word_id(std::string word_id) {
  release_word(word_id_);
  word_id_ = pick_word(std::move(word_id));
}
void GCursorView::restore() {
  docked_ = false;
  if (compositor_) {
    compositor_->set_active_view(this);
  }
}
void GCursorView::dock() { docked_ = true; }
void GCursorView::set_toplevel(wlr_xdg_toplevel *toplevel) { toplevel_ = toplevel; }
wlr_xdg_toplevel *GCursorView::toplevel() const { return toplevel_; }
void GCursorView::set_owner_cell(Cell *cell) { owner_cell_ = cell; }
Cell *GCursorView::owner_cell() const { return owner_cell_; }
void GCursorView::set_owner_ncursor(NCursorView *view) { owner_ncursor_ = view; }
NCursorView *GCursorView::owner_ncursor() const { return owner_ncursor_; }
