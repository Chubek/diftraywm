#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto memory = api_examples::capture_memory();
    auto sinklet = std::make_shared<lsh::GrepSinklet>("error", memory);

    auto report = api_examples::run_or_throw(
        shell,
        lsh::dsl::redirect(
            lsh::dsl::cmd("printf", "%s\n", "ok", "error: disk full", "error: retry"),
            lsh::to_sinklet(lsh::RedirectStream::stdout_stream, sinklet)));
    api_examples::require(report.status.success(), "grep sinklet failed");
    std::cout << memory->bytes();
}
