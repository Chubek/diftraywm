#include "LibShell-Posix.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lsh::cli {
Result<ir::Program> parse_line(std::string_view line);
}

namespace {

enum class CliMode {
    interactive,
    command,
    help,
    version,
};

struct CliConfig {
    CliMode mode {CliMode::interactive};
    std::string command;
};

void print_usage(std::ostream& out, std::string_view exe) {
    out << "Usage: " << exe << " [OPTION]... [--] [COMMAND [ARG]...]\n"
        << "\n"
        << "Options:\n"
        << "  -c, --command CMD   execute CMD\n"
        << "  -h, --help          display this help and exit\n"
        << "  -V, --version       output version information and exit\n";
}

void print_version(std::ostream& out) { out << "libsh 0.1.0\n"; }

[[nodiscard]] std::optional<CliConfig> parse_cli(int argc, char** argv) {
    CliConfig cfg;
    bool end_of_options = false;
    std::string exe = argc > 0 ? argv[0] : "libsh";

    for (int index = 1; index < argc; ++index) {
        std::string_view arg = argv[index];
        if (!end_of_options) {
            if (arg == "--") {
                end_of_options = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                cfg.mode = CliMode::help;
                return cfg;
            }
            if (arg == "-V" || arg == "--version") {
                cfg.mode = CliMode::version;
                return cfg;
            }
            if (arg == "-c" || arg == "--command") {
                if (index + 1 >= argc) {
                    std::cerr << "libsh: missing argument for " << arg << '\n';
                    print_usage(std::cerr, exe);
                    return std::nullopt;
                }
                cfg.mode = CliMode::command;
                cfg.command = argv[++index];
                continue;
            }
            if (arg.starts_with('-')) {
                std::cerr << "libsh: unknown option: " << arg << '\n';
                print_usage(std::cerr, exe);
                return std::nullopt;
            }
        }

        cfg.mode = CliMode::command;
        if (!cfg.command.empty()) {
            cfg.command.push_back(' ');
        }
        cfg.command += arg;
    }

    return cfg;
}

void print_diagnostic(const lsh::Diagnostic& diagnostic) {
    std::cerr << "libsh: " << diagnostic.message;
    if (!diagnostic.path.empty()) {
        std::cerr << " (" << diagnostic.path << ')';
    }
    std::cerr << '\n';
}

int run_line(lsh::Shell& shell, std::string_view line) {
    auto program = lsh::cli::parse_line(line);
    if (!program) {
        print_diagnostic(program.error());
        return 2;
    }
    auto report = shell.run(program.value());
    if (!report) {
        print_diagnostic(report.error());
        return 1;
    }
    for (const auto& diagnostic : report.value().diagnostics) {
        print_diagnostic(diagnostic);
    }
    return report.value().status.code;
}

} // namespace

int main(int argc, char** argv) {
    auto cli = parse_cli(argc, argv);
    if (!cli) {
        return 2;
    }
    if (cli->mode == CliMode::help) {
        print_usage(std::cout, argc > 0 ? argv[0] : "libsh");
        return 0;
    }
    if (cli->mode == CliMode::version) {
        print_version(std::cout);
        return 0;
    }

    lsh::Shell shell {std::make_shared<lsh::LocalExecutor>()};
    shell.set_command_substitution_parser(
        [](std::string_view line) { return lsh::cli::parse_line(line); });
    if (cli->mode == CliMode::command) {
        return run_line(shell, cli->command);
    }

    std::string line;
    while (std::cout << "libsh> " && std::getline(std::cin, line)) {
        if (line == "exit" || line == "quit") {
            break;
        }
        if (line.empty()) {
            continue;
        }
        (void)run_line(shell, line);
    }
    return 0;
}
