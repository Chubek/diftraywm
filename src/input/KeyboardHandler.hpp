#pragma once

#include <cstdint>

class Compositor;

class KeyboardHandler {
public:
  explicit KeyboardHandler(Compositor *compositor = nullptr);
  bool handle_key(uint32_t keysym, uint32_t modifiers, uint32_t state,
                  uint32_t unicode, uint32_t keycode = UINT32_MAX);

private:
  Compositor *compositor_ = nullptr;
};
