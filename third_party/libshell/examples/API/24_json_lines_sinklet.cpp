#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto memory = api_examples::capture_memory();
    auto sinklet = std::make_shared<lsh::JsonLinesSinklet>(memory);

    auto report = api_examples::run_or_throw(
        shell,
        lsh::dsl::redirect(
            lsh::dsl::cmd("printf", "%s\n", "deploy", "rollback"),
            lsh::to_sinklet(lsh::RedirectStream::stdout_stream, sinklet)));
    api_examples::require(report.status.success(), "json lines sinklet failed");
    std::cout << memory->bytes();
}
