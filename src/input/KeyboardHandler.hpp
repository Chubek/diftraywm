#pragma once

#include <cstdint>

struct wlr_keyboard;
struct wlr_keyboard_key_event;
class Compositor;

class KeyboardHandler {
public:
  explicit KeyboardHandler(Compositor *compositor = nullptr);
  void handle_key(wlr_keyboard *keyboard, const wlr_keyboard_key_event &event);

private:
  Compositor *compositor_ = nullptr;
};
