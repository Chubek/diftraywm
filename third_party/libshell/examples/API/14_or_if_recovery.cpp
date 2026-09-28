#include "common.hpp"

int main() {
    using namespace lsh::dsl;

    auto shell = api_examples::local_shell();
    auto memory = api_examples::capture_memory();

    auto expr = redirect(builtin("false") || cmd("echo", "fallback"), lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));
    auto report = api_examples::run_or_throw(shell, expr);
    api_examples::require(report.status.success(), "or-if flow failed");
    std::cout << memory->bytes();
}
