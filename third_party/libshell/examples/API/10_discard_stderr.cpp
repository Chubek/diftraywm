#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto memory = api_examples::capture_memory();

    auto expr = lsh::dsl::redirect(
        lsh::dsl::redirect(
            lsh::dsl::cmd("/bin/sh", "-c", "echo ok; echo noisy >&2"),
            lsh::to_null(lsh::RedirectStream::stderr_stream)),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));

    auto report = api_examples::run_or_throw(shell, expr);
    api_examples::require(report.status.success(), "discard stderr failed");
    std::cout << memory->bytes();
}
