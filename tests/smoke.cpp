#include "views/Cell.hpp"
#include "views/NCursorView.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  NCursorView view;
  Cell first("/bin/sh");
  Cell second("/bin/sh");
  if (!view.insert_cell(&first, false) || !view.insert_cell(&second, false)) {
    std::cerr << "insert failed\n";
    return EXIT_FAILURE;
  }
  view.set_output_box({0, 0, 800, 600});
  view.layout();
  if (first.box().height <= 0 || second.box().height <= 0) {
    std::cerr << "layout failed\n";
    return EXIT_FAILURE;
  }
  if (!view.move_selected_cell(-1)) {
    std::cerr << "move failed\n";
    return EXIT_FAILURE;
  }
  view.set_cell_select_mode(true);
  if (!view.cell_select_mode()) {
    std::cerr << "select mode failed\n";
    return EXIT_FAILURE;
  }
  view.set_workspace(3);
  if (view.workspace() != 3) {
    std::cerr << "workspace tag failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
