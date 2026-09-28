#include "views/NCursorView.hpp"

#include <algorithm>

NCursorView::NCursorView() = default;
NCursorView::~NCursorView() = default;

void NCursorView::layout() {
  if (cell_stacks_.empty()) {
    return;
  }
  // Stacks tile the output side by side (columns); cells within a stack tile
  // vertically (rows). A single stack keeps the historical full-width layout.
  const std::size_t stack_count = cell_stacks_.size();
  const int column_width =
      std::max(1, output_box_.width / static_cast<int>(stack_count));
  for (std::size_t stack_index = 0; stack_index < stack_count; ++stack_index) {
    auto &stack = cell_stacks_[stack_index];
    const int x = output_box_.x + static_cast<int>(stack_index) * column_width;
    const int width = stack_index + 1 == stack_count
                          ? output_box_.width - static_cast<int>(stack_index) * column_width
                          : column_width;
    const int cell_height = stack.cells.empty()
                                ? output_box_.height
                                : std::max(1, output_box_.height /
                                                  static_cast<int>(stack.cells.size()));
    int cell_y = output_box_.y;
    for (auto *cell : stack.cells) {
      if (!cell) {
        continue;
      }
      wlr_box box{x, cell_y, width, cell_height};
      cell->set_box(box);
      cell_y += cell_height;
    }
  }
}

void NCursorView::render(wlr_render_pass *) {}
void NCursorView::focus() {}
void NCursorView::handle_key(wlr_keyboard_key_event *) {}
ViewType NCursorView::type() const { return ViewType::NCURSOR; }
std::vector<NCursorView::CellStack> &NCursorView::cell_stacks() { return cell_stacks_; }
const std::vector<NCursorView::CellStack> &NCursorView::cell_stacks() const { return cell_stacks_; }
CursorArea &NCursorView::cursor_area() { return cursor_area_; }
const CursorArea &NCursorView::cursor_area() const { return cursor_area_; }
bool NCursorView::cell_select_mode() const { return cell_select_mode_; }
int NCursorView::selected_cell_index() const { return selected_cell_index_; }
void NCursorView::set_output_box(const wlr_box &box) { output_box_ = box; }

void NCursorView::ensure_stack() {
  if (cell_stacks_.empty()) {
    cell_stacks_.push_back({});
    selected_stack_index_ = 0;
    selected_cell_index_ = 0;
  }
  if (selected_stack_index_ >= cell_stacks_.size()) {
    selected_stack_index_ = 0;
  }
}

Cell *NCursorView::active_cell() const {
  if (cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size()) {
    return nullptr;
  }
  const auto &stack = cell_stacks_[selected_stack_index_];
  if (stack.cells.empty() || stack.active_index >= stack.cells.size()) {
    return nullptr;
  }
  return stack.cells[stack.active_index];
}

void NCursorView::set_cell_select_mode(bool enabled) { cell_select_mode_ = enabled; }

bool NCursorView::insert_cell(Cell *cell, bool above) {
  if (!cell) {
    return false;
  }
  ensure_stack();
  auto &stack = cell_stacks_[selected_stack_index_];
  const std::size_t index = stack.cells.empty() ? 0 : std::min(stack.active_index, stack.cells.size() - 1);
  const auto insert_at = above ? static_cast<std::ptrdiff_t>(index) : static_cast<std::ptrdiff_t>(index + 1);
  const auto insertion_index = static_cast<std::size_t>(
      std::min<std::ptrdiff_t>(insert_at, static_cast<std::ptrdiff_t>(stack.cells.size())));
  stack.cells.insert(stack.cells.begin() + static_cast<std::ptrdiff_t>(insertion_index), cell);
  stack.active_index = insertion_index;
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::remove_cell(Cell *cell) {
  if (!cell) {
    return false;
  }
  for (std::size_t stack_index = 0; stack_index < cell_stacks_.size(); ++stack_index) {
    auto &stack = cell_stacks_[stack_index];
    const auto it = std::find(stack.cells.begin(), stack.cells.end(), cell);
    if (it == stack.cells.end()) {
      continue;
    }
    const auto index = static_cast<std::size_t>(std::distance(stack.cells.begin(), it));
    stack.cells.erase(it);
    if (stack.cells.empty()) {
      stack.active_index = 0;
    } else if (stack.active_index >= stack.cells.size()) {
      stack.active_index = stack.cells.size() - 1;
    } else if (index <= stack.active_index && stack.active_index > 0) {
      --stack.active_index;
    }
    selected_cell_index_ = static_cast<int>(stack.active_index);
    // Dropping the emptied split keeps focus on a live pane. The final stack
    // is kept even when empty so the view stays usable.
    if (stack.cells.empty() && cell_stacks_.size() > 1) {
      cell_stacks_.erase(cell_stacks_.begin() + static_cast<std::ptrdiff_t>(stack_index));
      if (selected_stack_index_ > stack_index) {
        --selected_stack_index_;
      } else if (selected_stack_index_ >= cell_stacks_.size()) {
        selected_stack_index_ = cell_stacks_.size() - 1;
      }
      const auto &current = cell_stacks_[selected_stack_index_];
      selected_cell_index_ =
          current.cells.empty() ? 0 : static_cast<int>(current.active_index);
    } else if (selected_stack_index_ < cell_stacks_.size() &&
               stack_index == selected_stack_index_) {
      selected_cell_index_ = static_cast<int>(stack.active_index);
    }
    return true;
  }
  return false;
}

bool NCursorView::move_selected_cell(int delta) {
  if (delta == 0 || cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size()) {
    return false;
  }
  auto &stack = cell_stacks_[selected_stack_index_];
  if (stack.cells.size() < 2 || stack.active_index >= stack.cells.size()) {
    return false;
  }
  const auto current = static_cast<std::ptrdiff_t>(stack.active_index);
  const auto next = current + delta;
  if (next < 0 || next >= static_cast<std::ptrdiff_t>(stack.cells.size())) {
    return false;
  }
  std::swap(stack.cells[static_cast<std::size_t>(current)], stack.cells[static_cast<std::size_t>(next)]);
  stack.active_index = static_cast<std::size_t>(next);
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::select_next_cell() {
  if (cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size()) {
    return false;
  }
  auto &stack = cell_stacks_[selected_stack_index_];
  if (stack.cells.empty() || stack.active_index + 1 >= stack.cells.size()) {
    return false;
  }
  ++stack.active_index;
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::select_previous_cell() {
  if (cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size()) {
    return false;
  }
  auto &stack = cell_stacks_[selected_stack_index_];
  if (stack.cells.empty() || stack.active_index == 0) {
    return false;
  }
  --stack.active_index;
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::select_cell(Cell *cell) {
  for (std::size_t stack_index = 0; stack_index < cell_stacks_.size(); ++stack_index) {
    auto &stack = cell_stacks_[stack_index];
    const auto it = std::find(stack.cells.begin(), stack.cells.end(), cell);
    if (it != stack.cells.end()) {
      selected_stack_index_ = stack_index;
      stack.active_index = static_cast<std::size_t>(std::distance(stack.cells.begin(), it));
      selected_cell_index_ = static_cast<int>(stack.active_index);
      return true;
    }
  }
  return false;
}

std::size_t NCursorView::pane_count() const {
  std::size_t count = 0;
  for (const auto &stack : cell_stacks_) count += stack.cells.size();
  return count;
}

bool NCursorView::split_stack(Cell *cell) {
  if (!cell) {
    return false;
  }
  ensure_stack();
  cell_stacks_.push_back(CellStack{{cell}, 0});
  selected_stack_index_ = cell_stacks_.size() - 1;
  selected_cell_index_ = 0;
  return true;
}

std::vector<Cell *> NCursorView::panes_in_order() const {
  std::vector<Cell *> panes;
  for (const auto &stack : cell_stacks_)
    panes.insert(panes.end(), stack.cells.begin(), stack.cells.end());
  return panes;
}

bool NCursorView::focus_next_cell() {
  const auto panes = panes_in_order();
  if (panes.empty()) {
    return false;
  }
  const auto it = std::find(panes.begin(), panes.end(), active_cell());
  const std::size_t next =
      it == panes.end() ? 0 : (static_cast<std::size_t>(std::distance(panes.begin(), it)) + 1) % panes.size();
  return select_cell(panes[next]);
}

bool NCursorView::focus_prev_cell() {
  const auto panes = panes_in_order();
  if (panes.empty()) {
    return false;
  }
  const auto it = std::find(panes.begin(), panes.end(), active_cell());
  const std::size_t index =
      it == panes.end() ? 0 : static_cast<std::size_t>(std::distance(panes.begin(), it));
  return select_cell(panes[(index + panes.size() - 1) % panes.size()]);
}

bool NCursorView::focus_up_cell() {
  if (cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size()) {
    return false;
  }
  auto &stack = cell_stacks_[selected_stack_index_];
  if (stack.cells.empty() || stack.active_index == 0) {
    return false;
  }
  --stack.active_index;
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::focus_down_cell() {
  if (cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size()) {
    return false;
  }
  auto &stack = cell_stacks_[selected_stack_index_];
  if (stack.cells.empty() || stack.active_index + 1 >= stack.cells.size()) {
    return false;
  }
  ++stack.active_index;
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::focus_left_cell() {
  if (cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size() ||
      selected_stack_index_ == 0) {
    return false;
  }
  const std::size_t row = cell_stacks_[selected_stack_index_].active_index;
  --selected_stack_index_;
  auto &stack = cell_stacks_[selected_stack_index_];
  stack.active_index = stack.cells.empty() ? 0 : std::min(row, stack.cells.size() - 1);
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::focus_right_cell() {
  if (cell_stacks_.empty() || selected_stack_index_ >= cell_stacks_.size() ||
      selected_stack_index_ + 1 >= cell_stacks_.size()) {
    return false;
  }
  const std::size_t row = cell_stacks_[selected_stack_index_].active_index;
  ++selected_stack_index_;
  auto &stack = cell_stacks_[selected_stack_index_];
  stack.active_index = stack.cells.empty() ? 0 : std::min(row, stack.cells.size() - 1);
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}
