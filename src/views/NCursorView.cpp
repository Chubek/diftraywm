#include "views/NCursorView.hpp"

#include <algorithm>

NCursorView::NCursorView() = default;
NCursorView::~NCursorView() = default;

void NCursorView::layout() {
  if (cell_stacks_.empty()) {
    return;
  }
  const int band_height = cell_stacks_.empty() ? 0 : output_box_.height / static_cast<int>(cell_stacks_.size());
  int y = output_box_.y;
  for (auto &stack : cell_stacks_) {
    int cell_y = y;
    const int cell_height = stack.cells.empty() ? band_height : band_height / static_cast<int>(stack.cells.size());
    for (auto *cell : stack.cells) {
      if (!cell) {
        continue;
      }
      wlr_box box{output_box_.x, cell_y, output_box_.width, cell_height};
      cell->set_box(box);
      cell_y += cell_height;
    }
    y += band_height;
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
  const auto pos = stack.cells.begin() + std::min<std::ptrdiff_t>(insert_at, static_cast<std::ptrdiff_t>(stack.cells.size()));
  stack.cells.insert(pos, cell);
  stack.active_index = static_cast<std::size_t>(std::distance(stack.cells.begin(), pos));
  selected_cell_index_ = static_cast<int>(stack.active_index);
  return true;
}

bool NCursorView::remove_cell(Cell *cell) {
  if (!cell) {
    return false;
  }
  for (auto &stack : cell_stacks_) {
    const auto it = std::find(stack.cells.begin(), stack.cells.end(), cell);
    if (it != stack.cells.end()) {
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
      return true;
    }
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

bool NCursorView::select_next_cell() { return move_selected_cell(1); }

bool NCursorView::select_previous_cell() { return move_selected_cell(-1); }

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
