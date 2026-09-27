#pragma once

#include <cstdint>

class Compositor;

class KeyboardHandler {
public:
  explicit KeyboardHandler(Compositor *compositor = nullptr);
  bool handle_key(uint32_t keysym, uint32_t modifiers, uint32_t state,
                  uint32_t unicode);

private:
  Compositor *compositor_ = nullptr;
};
