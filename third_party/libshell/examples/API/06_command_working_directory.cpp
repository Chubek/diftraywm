#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto workdir = api_examples::fresh_dir("libsh_api_example_cwd");
    auto memory = api_examples::capture_memory();

    lsh::ir::Command command = lsh::ir::make_command({"/bin/pwd"});
    command.cwd = workdir.string();
    command.redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));

    auto report = api_examples::run_or_throw(shell, lsh::ir::Program {lsh::ir::command(std::move(command), "per-command-cwd")});
    api_examples::require(report.status.success(), "pwd failed");
    std::cout << memory->bytes();
}
