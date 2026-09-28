#include "common.hpp"

#include <fstream>
#include <sstream>

int main() {
    auto shell = api_examples::local_shell();
    auto root = api_examples::fresh_dir("libsh_api_example_append");
    auto path = root / "audit.log";

    auto first = api_examples::run_or_throw(shell, lsh::dsl::redirect(lsh::dsl::cmd("echo", "start"), lsh::append(path.string())));
    api_examples::require(first.status.success(), "first append failed");
    auto second = api_examples::run_or_throw(shell, lsh::dsl::redirect(lsh::dsl::cmd("echo", "stop"), lsh::append(path.string())));
    api_examples::require(second.status.success(), "second append failed");

    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::cout << ss.str();
}
