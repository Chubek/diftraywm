#include "notelet/Notelet.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main() {
  NoteletCatalog catalog;
  std::string error;
  if (!catalog.discover(DIFTRAY_BUNDLED_NOTELETS_DIR, error) ||
      catalog.names().size() != 1 || catalog.names()[0] != "hello") {
    std::cerr << "discovery failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  auto app = catalog.open("hello", error);
  if (!app || !app->render("", error) ||
      app->frame().find("Bundled resource loaded.") == std::string::npos) {
    std::cerr << "initial render failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  if (!app->render("x", error) || app->frame().find("x") == std::string::npos ||
      !app->render("y", error) || app->frame().find("Previous key:\nx") == std::string::npos) {
    std::cerr << "key or state failed: " << error << " frame: " << app->frame() << '\n';
    return EXIT_FAILURE;
  }
  if (catalog.open("missing", error)) return EXIT_FAILURE;
  if (!catalog.discover(std::string(DIFTRAY_BUNDLED_NOTELETS_DIR) +
                            "/hello.notelet:" + DIFTRAY_BUNDLED_NOTELETS_DIR,
                        error) || catalog.names().size() != 1) {
    std::cerr << "archive path or duplicate precedence failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
