#include "input/KeyboardHandler.hpp"

KeyboardHandler::KeyboardHandler(Compositor *compositor) : compositor_(compositor) {}
void KeyboardHandler::handle_key(wlr_keyboard *, const wlr_keyboard_key_event &) {}
