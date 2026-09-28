#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto memory = api_examples::capture_memory();

    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("/bin/sh", "-c", "echo warning >&2"),
        lsh::to_memory(lsh::RedirectStream::stderr_stream, memory));

    auto report = api_examples::run_or_throw(shell, expr);
    api_examples::require(report.status.success(), "stderr producer failed");
    std::cout << memory->bytes();
}
