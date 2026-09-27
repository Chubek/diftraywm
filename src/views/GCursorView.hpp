#pragma once

#include "views/View.hpp"

#include <optional>
#include <string>

struct wlr_xdg_toplevel;
class Cell;
class Compositor;
class NCursorView;

class GCursorView : public View {
public:
  explicit GCursorView(Compositor *compositor = nullptr, std::string word_id = {});
  ~GCursorView() override;

  void layout() override;
  void render(wlr_render_pass *pass) override;
  void focus() override;
  void handle_key(wlr_keyboard_key_event *event) override;
  ViewType type() const override;

  const std::string &word_id() const;
  bool docked() const;
  std::optional<int> quick_restore_slot() const;
  void set_quick_restore_slot(std::optional<int> slot);
  void set_word_id(std::string word_id);
  void restore();
  void dock();
  void set_toplevel(wlr_xdg_toplevel *toplevel);
  wlr_xdg_toplevel *toplevel() const;
  void set_owner_cell(Cell *cell);
  Cell *owner_cell() const;
  void set_owner_ncursor(NCursorView *view);
  NCursorView *owner_ncursor() const;
  int workspace() const { return workspace_; }
  void set_workspace(int workspace) { workspace_ = workspace; }

private:
  std::string word_id_;
  bool docked_ = false;
  std::optional<int> quick_restore_slot_;
  wlr_xdg_toplevel *toplevel_ = nullptr;
  Compositor *compositor_ = nullptr;
  Cell *owner_cell_ = nullptr;
  NCursorView *owner_ncursor_ = nullptr;
  int workspace_ = 1;
};
