#include "compositor/Compositor.hpp"

#include <iostream>
#include <string>

namespace {
void print_help(const char *program) {
  std::cout << "Usage: " << program << " [--command COMMAND] [--help] [--version]\n"
            << "\n"
            << "Starts the DiftrayWM Wayland compositor. Cells of NTerm form the\n"
            << "NCursor view; graphical clients are promoted to GCursor views.\n"
            << "\n"
            << "Bindings:\n"
            << "  Meta+Escape     quit\n"
            << "  Meta+Tab        cell select mode\n"
            << "  Meta+Up/Down    reorder cells\n"
            << "  Meta+K          kill selected cell\n"
            << "  :               cell command bar\n"
            << "  Meta+:          global command bar\n"
            << "  Meta+D          launcher\n"
            << "  Meta+N          new NCursor\n"
            << "  Meta+Return     toggle TCursor\n"
            << "  Meta+F1-F4      quick restore GCursor\n"
            << "  Meta+Left/Right cycle tabs\n"
            << "  Meta+1-9        switch workspace\n"
            << "  Meta+0          workspace 10\n";
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
    std::cerr << compositor.status_line() << '\n';
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
