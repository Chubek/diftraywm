#include "common.hpp"

int main() {
    using namespace lsh::dsl;

    auto shell = api_examples::local_shell();
    auto memory = api_examples::capture_memory();

    auto flow = redirect(
        then(cmd("export", "LIBSH_MODE=demo"), cmd("printenv", "LIBSH_MODE")),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));

    auto report = api_examples::run_or_throw(shell, flow);
    api_examples::require(report.status.success(), "export sequence failed");
    std::cout << memory->bytes();
}
