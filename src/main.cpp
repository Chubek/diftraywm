#include "compositor/Compositor.hpp"

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

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
            << "  Meta+0          workspace 10\n"
            << "  Meta+S/V/O/P/Z  multiplexer split, focus and zoom\n"
            << "\n"
            << "Meta also responds to the Ctrl+Q prefix chord; see CONFIGURATION.md.\n"
            << "Run `diftrayctl --help` to drive this session from a shell.\n";
}

// Re-executes the compositor in place for `session restart`. Reading the
// original argv from /proc keeps every flag and environment the session was
// started with, and exec leaves the PID (and any session supervisor) intact.
[[noreturn]] void restart_self() {
  std::error_code error;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  std::vector<std::string> arguments;
  {
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(cmdline)),
                    std::istreambuf_iterator<char>());
    std::string current;
    for (const char ch : raw) {
      if (ch == '\0') {
        if (!current.empty()) arguments.push_back(current);
        current.clear();
      } else {
        current.push_back(ch);
      }
    }
    if (!current.empty()) arguments.push_back(current);
  }
  if (error || executable.empty() || arguments.empty()) {
    std::cerr << "restart failed: cannot recover the compositor command line\n";
    std::_Exit(70);
  }
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (auto &argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  // Restore the default disposition in case a caught signal left one ignored,
  // otherwise the restarted compositor would silently drop SIGTERM.
  ::signal(SIGINT, SIG_DFL);
  ::signal(SIGTERM, SIG_DFL);
  ::signal(SIGCHLD, SIG_DFL);
  ::execv(executable.c_str(), argv.data());
  std::cerr << "restart failed: " << std::strerror(errno) << '\n';
  std::_Exit(70);
}

}  // namespace

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
    compositor.flush_extension_commands();
    std::cout << compositor.command_bar().status_line() << '\n';
    // A one-shot command such as `session restart` still has to take effect.
    if (compositor.restart_requested()) {
      restart_self();
    }
    return 0;
  }
  const int result = compositor.run();
  if (compositor.restart_requested()) {
    restart_self();
  }
  return result;
}
