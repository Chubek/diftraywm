#include "input/KeyboardHandler.hpp"

#include "compositor/Compositor.hpp"

#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

KeyboardHandler::KeyboardHandler(Compositor *compositor) : compositor_(compositor) {}

bool KeyboardHandler::handle_key(uint32_t keysym, uint32_t modifiers, uint32_t state,
                                 uint32_t unicode, uint32_t keycode) {
  if (!compositor_) {
    return false;
  }
  return compositor_->handle_key(keysym, modifiers, state, unicode, keycode);
}
