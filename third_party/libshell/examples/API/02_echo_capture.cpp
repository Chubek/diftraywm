#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto output = api_examples::capture_stdout(shell, lsh::dsl::cmd("echo", "deployment-ready"));
    std::cout << output;
}
