#include "common.hpp"

int main() {
    using namespace lsh::dsl;

    auto shell = api_examples::local_shell();
    auto expr = pipe(
        cmd("/bin/sh", "-c", "exit 7"),
        cmd("/bin/cat"),
        lsh::PipefailPolicy::any_failed);

    auto report = api_examples::run_or_throw(shell, expr);
    std::cout << "status=" << report.status.code << '\n';
}
