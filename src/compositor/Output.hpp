#pragma once

#include <array>
#include <string>

class View;
class NCursorView;

struct OutputGeometry {
  std::string name;
  int x = 0, y = 0, width = 1280, height = 720;
};

// Focus is remembered independently for each monitor and workspace.
struct Output {
  OutputGeometry geometry;
  std::array<NCursorView *, 11> ncursors{};
  std::array<View *, 11> views{};
};
