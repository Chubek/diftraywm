#pragma once

#include "views/Cell.hpp"
#include "views/CursorArea.hpp"
#include "views/View.hpp"

#include <vector>

#include <wlr/util/box.h>

class NCursorView : public View {
public:
  struct CellStack {
    std::vector<Cell *> cells;
    std::size_t active_index = 0;
  };

  NCursorView();
  ~NCursorView() override;

  void layout() override;
  void render(wlr_render_pass *pass) override;
  void focus() override;
  void handle_key(wlr_keyboard_key_event *event) override;
  ViewType type() const override;

  std::vector<CellStack> &cell_stacks();
  const std::vector<CellStack> &cell_stacks() const;
  CursorArea &cursor_area();
  const CursorArea &cursor_area() const;
  bool cell_select_mode() const;
  int selected_cell_index() const;
  void set_output_box(const wlr_box &box);
  Cell *active_cell() const;
  void set_cell_select_mode(bool enabled);
  void ensure_stack();
  bool insert_cell(Cell *cell, bool above);
  bool remove_cell(Cell *cell);
  bool move_selected_cell(int delta);
  bool select_next_cell();
  bool select_previous_cell();
  bool select_cell(Cell *cell);

private:
  std::vector<CellStack> cell_stacks_;
  CursorArea cursor_area_;
  bool cell_select_mode_ = false;
  int selected_cell_index_ = 0;
  std::size_t selected_stack_index_ = 0;
  wlr_box output_box_{};
};
