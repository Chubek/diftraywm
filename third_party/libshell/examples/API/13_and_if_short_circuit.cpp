#include "common.hpp"

int main() {
    using namespace lsh::dsl;

    auto shell = api_examples::local_shell();
    auto memory = api_examples::capture_memory();

    auto expr = redirect(builtin("false") && cmd("echo", "should-not-run"), lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));
    auto report = api_examples::run_or_throw(shell, expr);

    std::cout << "status=" << report.status.code << '\n';
    std::cout << "stdout=" << memory->bytes();
}
