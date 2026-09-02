#pragma once

#include <memory>

struct wlr_output;

class View;
class NCursorView;

struct Output {
  wlr_output *output = nullptr;
  NCursorView *ncursor_view = nullptr;
};
