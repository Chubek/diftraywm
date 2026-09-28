#include "notelet/Notelet.hpp"

#include <cstdlib>
#include <chrono>
#include <thread>
#include <algorithm>
#include <iostream>
#include <string>

int main() {
  NoteletCatalog catalog;
  std::string error;
  if (!catalog.discover(DIFTRAY_BUNDLED_NOTELETS_DIR, error) ||
      catalog.names().size() != 3 ||
      std::find(catalog.names().begin(), catalog.names().end(), "hello") == catalog.names().end()) {
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
                        error) || catalog.names().size() != 3) {
    std::cerr << "archive path or duplicate precedence failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  auto scratch = catalog.open("scratchpad", error);
  if (!scratch || !scratch->render("", error, "open") ||
      !scratch->render("é", error) || !scratch->render("界", error) ||
      scratch->frame().find("é界") == std::string::npos ||
      !scratch->render("Backspace", error) ||
      scratch->frame().find("é界") != std::string::npos ||
      scratch->frame().find("é") == std::string::npos ||
      !scratch->render("", error, "resize") || scratch->frame().find("é") == std::string::npos) {
    std::cerr << "scratchpad editing failed: " << error << '\n'; return EXIT_FAILURE;
  }
  auto dashboard = catalog.open("desktop", error);
  dashboard->set_context({{"outputs", "left 1920x1080"}, {"workspace", "4"}});
  if (!dashboard->render("", error, "open") ||
      dashboard->frame().find("left 1920x1080") == std::string::npos) {
    std::cerr << "desktop snapshot failed: " << error << '\n'; return EXIT_FAILURE;
  }
  if (!scratch->request_render("a", error) || !scratch->request_render("b", error)) return EXIT_FAILURE;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (scratch->busy() && std::chrono::steady_clock::now() < deadline) {
    scratch->poll(error);
    if (!error.empty()) { std::cerr << error << '\n'; return EXIT_FAILURE; }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (scratch->busy() || scratch->frame().find("éab") == std::string::npos) {
    std::cerr << "worker queue lost state\n"; return EXIT_FAILURE;
  }
  Notelet failure("failure", "notelet:set \"x\" \"bad\"; G:die \"expected\";",
                  "const notelet = G:load \"diftray.notelet\";", {});
  if (!failure.request_render("", error)) return EXIT_FAILURE;
  while (failure.busy() && std::chrono::steady_clock::now() < deadline) {
    failure.poll(error);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (error.find("expected") == std::string::npos || failure.busy()) {
    std::cerr << "worker error not reported\n"; return EXIT_FAILURE;
  }
  Notelet blocked("blocked", "const e = G:load \"std.exec\"; e:system \"sleep 5\";", "", {});
  if (!blocked.request_render("", error)) return EXIT_FAILURE;
  const auto blocked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (blocked.busy() && std::chrono::steady_clock::now() < blocked_deadline) {
    blocked.poll(error);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (blocked.busy() || error.find("timed out") == std::string::npos) {
    std::cerr << "blocked script was not cancelled: " << error << '\n'; return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
