#include "help/HelpPager.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main() {
  HelpPager pager(DIFTRAY_HELP_TEST_PATH);
  std::string message;
  if (!pager.open("help-pager", message) || pager.page_name() != "help-pager") {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
  }
  if (!pager.find("pager", message) || pager.match_count() == 0 ||
      !pager.next_match(message) || !pager.previous_match(message)) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
  }
  if (!pager.set_bookmark("pager", message) || !pager.open("notelets", message) ||
      !pager.open_bookmark("pager", message) || pager.page_name() != "help-pager") {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
  }
  if (pager.find("(", message)) {
    std::cerr << "invalid regex was accepted\n";
    return EXIT_FAILURE;
  }
  const auto frame = pager.render(48, 12);
  if (frame.find("HELP-PAGER") == std::string::npos || frame.find("q: close") == std::string::npos) {
    std::cerr << "pager did not render expected chrome\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
