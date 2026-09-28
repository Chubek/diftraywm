#include "common.hpp"

#include <fstream>

int main() {
    auto shell = api_examples::local_shell();
    auto root = api_examples::fresh_dir("libsh_api_example_glob");
    std::ofstream(root / "alpha.txt").put('\n');
    std::ofstream(root / "beta.txt").put('\n');
    std::ofstream(root / "skip.log").put('\n');
    shell.set_cwd(root);

    auto output = api_examples::capture_stdout(shell, lsh::dsl::cmd("echo", lsh::glob("*.txt")));
    std::cout << output;
}
