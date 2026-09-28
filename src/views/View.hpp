#pragma once

#include <string>
#include <utility>

struct wlr_render_pass;
struct wlr_keyboard_key_event;

enum class ViewType {
  NCURSOR,
  GCURSOR,
  TCURSOR,
};

class View {
public:
  const std::string &output_name() const { return output_name_; }
  void set_output_name(std::string name) { output_name_ = std::move(name); }
  virtual ~View() = default;
  virtual void layout() = 0;
  virtual void render(wlr_render_pass *pass) = 0;
  virtual void focus() = 0;
  virtual void handle_key(wlr_keyboard_key_event *event) = 0;
  virtual ViewType type() const = 0;

private:
  std::string output_name_ = "default";
};
