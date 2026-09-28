#include "LibShell-Posix.hpp"

// Redirection constructors for the parser, and the embedded-Lua adapter.
#include "lsh/DSL.hpp"
#include "lsh/Scripting.hpp"

namespace lsh::cli {
Result<ir::Program> parse_script(std::string_view text);
}

#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
    bool read_stdin {false};
};

void print_usage(std::ostream& out, std::string_view exe) {
    out << "Usage: " << exe << " [OPTION]... [--] [COMMAND [ARG]...]\n"
        << "\n"
        << "Options:\n"
        << "  -c, --command CMD   execute CMD\n"
        << "  -s, --stdin         read a script from standard input\n"
        << "  -h, --help          display this help and exit\n"
        << "  -V, --version       output version information and exit\n"
        << "\n"
        << "With no COMMAND, an interactive session is started; `exit` ends it.\n";
}

void print_version(std::ostream& out) { out << "libsh 0.2.0\n"; }

[[nodiscard]] std::optional<CliConfig> parse_cli(int argc, char** argv) {
    CliConfig config;
    bool end_of_options = false;
    const std::string exe = argc > 0 ? argv[0] : "libsh";

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (!end_of_options) {
            if (arg == "--") {
                end_of_options = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                config.mode = CliMode::help;
                return config;
            }
            if (arg == "-V" || arg == "--version") {
                config.mode = CliMode::version;
                return config;
            }
            if (arg == "-s" || arg == "--stdin") {
                config.read_stdin = true;
                config.mode = CliMode::command;
                continue;
            }
            if (arg == "-c" || arg == "--command") {
                if (index + 1 >= argc) {
                    std::cerr << "libsh: missing argument for " << arg << '\n';
                    print_usage(std::cerr, exe);
                    return std::nullopt;
                }
                config.mode = CliMode::command;
                config.command = argv[++index];
                continue;
            }
            if (arg.starts_with('-') && arg != "-") {
                std::cerr << "libsh: unknown option: " << arg << '\n';
                print_usage(std::cerr, exe);
                return std::nullopt;
            }
        }

        config.mode = CliMode::command;
        if (!config.command.empty()) {
            config.command.push_back(' ');
        }
        config.command += arg;
    }
    return config;
}

void print_diagnostic(const lsh::Diagnostic& diagnostic) {
    std::cerr << "libsh: " << diagnostic.message;
    if (!diagnostic.path.empty()) {
        std::cerr << " (" << diagnostic.path << ')';
    }
    std::cerr << '\n';
}

// Runs one parsed program and renders its report. Returns the shell status.
int run_program(lsh::Shell& shell, const lsh::ir::Program& program) {
    auto report = shell.run(program);
    if (!report) {
        print_diagnostic(report.error());
        return 2;
    }
    for (const auto& diagnostic : report.value().diagnostics) {
        print_diagnostic(diagnostic);
    }
    return report.value().status.code;
}

// Reads a whole script from standard input. Used for `libsh -s` and for piping.
[[nodiscard]] std::string read_all(std::istream& input) {
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// An interactive session keeps reading while the script is incomplete, so a
// here-document or an unterminated quote spans prompt boundaries the way a real
// terminal does.
int run_interactive(lsh::Shell& shell) {
    std::string buffer;
    for (;;) {
        std::cout << (buffer.empty() ? "libsh> " : "  ...> ") << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            break;
        }
        if (buffer.empty() && (line == "exit" || line == "quit")) {
            break;
        }
        buffer += line;
        buffer.push_back('\n');

        auto program = lsh::cli::parse_script(buffer);
        if (!program && program.error().code == lsh::ErrorCode::syntax_error) {
            // The construct is not finished; keep reading.
            continue;
        }
        buffer.clear();
        if (!program) {
            print_diagnostic(program.error());
            continue;
        }
        (void)run_program(shell, program.value());
        if (shell.exit_requested()) {
            return shell.exit_code();
        }
    }
    return shell.exit_requested() ? shell.exit_code() : 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto cli = parse_cli(argc, argv);
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

    lsh::Shell shell {std::make_shared<lsh::posix::LocalExecutor>()};
    // The CLI owns the grammar, so command substitution, `eval`, and `$(...)`
    // all see exactly the language the front end parses.
    shell.set_command_substitution_parser([](std::string_view line) { return lsh::cli::parse_script(line); });
    shell.set_lua_evaluator(&lsh::scripting::eval_lua_qamrpp);
    // `~user` needs the password database, which only the platform layer has.
    shell.set_tilde_resolver(std::make_shared<lsh::posix::SystemTildeResolver>(shell.env()));
    shell.options().name = "libsh";

    if (cli->mode == CliMode::interactive) {
        return run_interactive(shell);
    }

    std::string script = cli->command;
    if (cli->read_stdin) {
        script = read_all(std::cin);
    }
    // A trailing newline lets the lexer resolve a here-document body.
    if (script.empty() || script.back() != '\n') {
        script.push_back('\n');
    }
    auto program = lsh::cli::parse_script(script);
    if (!program) {
        print_diagnostic(program.error());
        return 2;
    }
    const int status = run_program(shell, program.value());
    return shell.exit_requested() ? shell.exit_code() : status;
}
