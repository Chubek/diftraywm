#pragma once

#include "views/View.hpp"

class Cell;

#include <wlr/util/box.h>

class TCursorView : public View {
public:
  explicit TCursorView(Cell *cell);
  ~TCursorView() override;

  void layout() override;
  void render(wlr_render_pass *pass) override;
  void focus() override;
  void handle_key(wlr_keyboard_key_event *event) override;
  ViewType type() const override;

private:
  Cell *cell_ = nullptr;
  enum class PreviousState {
    NORMAL,
    SELECTED,
    TCURSOR,
  };
  PreviousState previous_state_ = PreviousState::NORMAL;
  wlr_box previous_box_{};
};
