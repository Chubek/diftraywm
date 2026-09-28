#include "common.hpp"

namespace lsh::cli {
Result<ir::Program> parse_line(std::string_view line);
}

int main() {
    auto shell = api_examples::local_shell();
    shell.set_command_substitution_parser([](std::string_view line) { return lsh::cli::parse_line(line); });

    auto output = api_examples::capture_stdout(
        shell,
        lsh::dsl::cmd("echo", lsh::command_substitution("printf '%s\\n' alpha beta | wc -l")));
    std::cout << output;
}
