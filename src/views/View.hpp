#pragma once

struct wlr_render_pass;
struct wlr_keyboard_key_event;

enum class ViewType {
  NCURSOR,
  GCURSOR,
  TCURSOR,
};

class View {
public:
  virtual ~View() = default;
  virtual void layout() = 0;
  virtual void render(wlr_render_pass *pass) = 0;
  virtual void focus() = 0;
  virtual void handle_key(wlr_keyboard_key_event *event) = 0;
  virtual ViewType type() const = 0;
};
