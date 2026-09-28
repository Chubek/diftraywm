#pragma once

#include "views/Cell.hpp"
#include "views/CursorArea.hpp"
#include "views/View.hpp"

#include <string>
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
  std::size_t pane_count() const;
  bool split_stack(Cell *cell);
  bool focus_next_cell();
  bool focus_prev_cell();
  bool focus_up_cell();
  bool focus_down_cell();
  bool focus_left_cell();
  bool focus_right_cell();
  const std::string &id() const { return id_; }
  void set_id(std::string id) { id_ = std::move(id); }
  const std::string &default_shell() const { return default_shell_; }
  void set_default_shell(std::string shell) { default_shell_ = std::move(shell); }
  int workspace() const { return workspace_; }
  void set_workspace(int workspace) { workspace_ = workspace; }

 private:
  std::vector<Cell *> panes_in_order() const;
  std::vector<CellStack> cell_stacks_;
  CursorArea cursor_area_;
  bool cell_select_mode_ = false;
  int selected_cell_index_ = 0;
  std::size_t selected_stack_index_ = 0;
  wlr_box output_box_{};
  std::string id_ = "ncursor";
  int workspace_ = 1;
  std::string default_shell_;
};
