#include "views/TCursorView.hpp"

#include "views/Cell.hpp"
#include <wlr/util/box.h>

TCursorView::TCursorView(Cell *cell) : cell_(cell) {
  if (cell_) {
    previous_state_ = static_cast<PreviousState>(cell_->state());
    previous_box_ = cell_->box();
    cell_->set_state(CellState::TCURSOR);
  }
}

TCursorView::~TCursorView() {
  if (cell_) {
    cell_->set_state(static_cast<CellState>(previous_state_));
    cell_->set_box(previous_box_);
  }
}

void TCursorView::layout() {}
void TCursorView::render(wlr_render_pass *) {}
void TCursorView::focus() {}
void TCursorView::handle_key(wlr_keyboard_key_event *) {}
ViewType TCursorView::type() const { return ViewType::TCURSOR; }
