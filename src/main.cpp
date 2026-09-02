#include "compositor/Compositor.hpp"

#include <iostream>
#include <string>

namespace {
void print_help(const char *program) {
  std::cout << "Usage: " << program << " [--command COMMAND] [--help] [--version]\n"
            << "\n"
            << "Without arguments DiftrayWM starts an interactive terminal session when\n"
            << "attached to a TTY, or performs one non-interactive event pass.\n";
}
}

int main(int argc, char **argv) {
  std::string command;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      print_help(argv[0]);
      return 0;
    }
    if (argument == "--version" || argument == "-V") {
      std::cout << "DiftrayWM 0.1\n";
      return 0;
    }
    if (argument == "--command" || argument == "-c") {
      if (index + 1 >= argc) {
        std::cerr << "--command requires an argument\n";
        return 2;
      }
      command = argv[++index];
      continue;
    }
    std::cerr << "unknown argument: " << argument << '\n';
    print_help(argv[0]);
    return 2;
  }

  Compositor compositor;
  if (!compositor.init()) {
    return 1;
  }
  if (!command.empty()) {
    if (!compositor.command_bar().dispatch(command)) {
      std::cerr << compositor.command_bar().status_line() << '\n';
      return 1;
    }
    std::cout << compositor.command_bar().status_line() << '\n';
    return 0;
  }
  return compositor.run();
}
