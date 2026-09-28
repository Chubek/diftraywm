#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto registry = std::make_shared<lsh::kernel::Registry>();

    lsh::kernel::Metadata metadata;
    metadata.name = "healthcheck";
    metadata.summary = "emit a healthcheck verdict";

    auto kernel = std::make_shared<lsh::kernel::FunctionKernel>(
        std::move(metadata),
        [](const lsh::kernel::Invocation& invocation) -> lsh::Result<lsh::ExitStatus> {
            if (invocation.stdout_writer) {
                if (auto write = invocation.stdout_writer->write("healthy\n"); !write) {
                    return write.error();
                }
            }
            return lsh::ExitStatus {};
        });

    auto added = registry->add(kernel);
    api_examples::require(added.has_value(), "kernel registration failed");
    shell.set_kernels(registry);

    auto memory = api_examples::capture_memory();
    lsh::ir::Command command;
    command.source = lsh::CommandSource::kernel;
    command.argv.push_back(lsh::literal("healthcheck"));
    command.redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));

    auto report = api_examples::run_or_throw(shell, lsh::ir::Program {lsh::ir::command(std::move(command), "kernel-example")});
    api_examples::require(report.status.success(), "kernel execution failed");
    std::cout << memory->bytes();
}
