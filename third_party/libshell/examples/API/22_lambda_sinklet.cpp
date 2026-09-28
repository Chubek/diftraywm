#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto downstream = api_examples::capture_memory();
    std::size_t bytes = 0;

    auto sinklet = std::make_shared<lsh::LambdaSinklet>(
        [downstream, &bytes](std::string_view chunk, lsh::SinkletContext&) -> lsh::Result<void> {
            bytes += chunk.size();
            return downstream->write(chunk);
        });

    auto report = api_examples::run_or_throw(
        shell,
        lsh::dsl::redirect(
            lsh::dsl::cmd("printf", "%s\n", "alpha", "beta"),
            lsh::to_sinklet(lsh::RedirectStream::stdout_stream, sinklet)));
    api_examples::require(report.status.success(), "sinklet example failed");

    std::cout << "bytes=" << bytes << '\n';
    std::cout << downstream->bytes();
}
