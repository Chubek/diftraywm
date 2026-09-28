#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto output = api_examples::capture_stdout(shell, lsh::dsl::cmd("echo", lsh::arithmetic("7 * (6 + 1)")));
    std::cout << output;
}
