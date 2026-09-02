#include "views/Cell.hpp"

#include "nterm/NTerm.hpp"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_set>

namespace {
std::string make_cell_id() {
  static std::mt19937_64 rng{std::random_device{}()};
  static std::unordered_set<std::string> live_ids;
  for (int attempts = 0; attempts < 32; ++attempts) {
    std::ostringstream out;
    out << std::hex << rng();
    const auto id = out.str();
    if (live_ids.insert(id).second) {
      return id;
    }
  }
  return "cell";
}
}

Cell::Cell(std::string shell_override)
    : id_(make_cell_id()), nterm_(std::make_unique<NTerm>(std::move(shell_override))),
      shell_override_(nterm_->shell_path()) {}

Cell::~Cell() = default;

const std::string &Cell::id() const { return id_; }
const wlr_box &Cell::box() const { return box_; }
void Cell::set_box(const wlr_box &box) { box_ = box; }
CellState Cell::state() const { return state_; }
void Cell::set_state(CellState state) { state_ = state; }
NTerm *Cell::nterm() const { return nterm_.get(); }
CursorArea &Cell::cursor_area() { return cursor_area_; }
const CursorArea &Cell::cursor_area() const { return cursor_area_; }
void Cell::set_shell_override(std::string shell_override) {
  shell_override_ = std::move(shell_override);
  if (nterm_) {
    nterm_->set_shell_path(shell_override_);
  }
}
const std::string &Cell::shell_override() const { return shell_override_; }
