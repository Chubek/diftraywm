#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();

    lsh::ir::Command command = lsh::ir::make_command({"printenv", "DEPLOY_STAGE"});
    command.environment_overlay.push_back(lsh::EnvVar {.key = "DEPLOY_STAGE", .value = "staging", .exported = true});

    auto memory = api_examples::capture_memory();
    command.redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));

    auto report = api_examples::run_or_throw(shell, lsh::ir::Program {lsh::ir::command(std::move(command), "env-overlay")});
    api_examples::require(report.status.success(), "printenv failed");
    std::cout << memory->bytes();
}
