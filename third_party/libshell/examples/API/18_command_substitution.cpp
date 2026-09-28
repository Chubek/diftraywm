#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto output = api_examples::capture_stdout(shell, lsh::dsl::cmd("echo", lsh::command_substitution("echo release-42")));
    std::cout << output;
}
