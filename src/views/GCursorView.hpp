#pragma once

#include "views/View.hpp"

#include <optional>
#include <string>
#include <string_view>

struct wlr_xdg_toplevel;
class Compositor;

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

private:
  std::string word_id_;
  bool docked_ = false;
  std::optional<int> quick_restore_slot_;
  wlr_xdg_toplevel *toplevel_ = nullptr;
  Compositor *compositor_ = nullptr;
};
