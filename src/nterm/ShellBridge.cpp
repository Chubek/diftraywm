#include "nterm/LibShellTerminal.hpp"
#include <iostream>
#include <unistd.h>

namespace lsh::cli {
Result<ir::Program> parse_line(std::string_view line);
}

bool diftraywm_shell_available(const char *path) {
  return path && *path && (std::string_view(path) == "libshell" || ::access(path, X_OK) == 0);
}

// Runs only in NTerm's PTY child; command execution never blocks the compositor.
int diftraywm_run_embedded_shell() {
  try {
    // Keep the interactive shell alive on Ctrl+C. Caught handlers reset to
    // default at exec, so foreground external commands still receive SIGINT.
    struct sigaction action {};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGQUIT, &action, nullptr);
    lsh::Shell shell{std::make_shared<lsh::LocalExecutor>()};
    shell.set_command_substitution_parser(lsh::cli::parse_line);
    int status = 0;
    std::string line;
    while (std::cout << "diftray> " << std::flush, std::getline(std::cin, line)) {
      if (line == "exit" || line == "quit") return status;
      if (line.empty()) continue;
      auto program = lsh::cli::parse_line(line);
      if (!program) {
        std::cerr << "libshell: " << program.error().message << '\n';
        status = 2;
        continue;
      }
      auto result = shell.run(program.value());
      if (!result) {
        std::cerr << "libshell: " << result.error().message << '\n';
        status = 1;
        continue;
      }
      status = result.value().status.code;
      while (::waitpid(-1, nullptr, WNOHANG) > 0) {}
      for (const auto &diagnostic : result.value().diagnostics)
        std::cerr << "libshell: " << diagnostic.message << '\n';
    }
    return status;
  } catch (const std::exception &error) {
    std::cerr << "libshell: " << error.what() << '\n';
    return 1;
  }
}
