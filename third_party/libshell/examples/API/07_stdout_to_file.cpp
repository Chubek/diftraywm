#include "common.hpp"

#include <fstream>
#include <sstream>

int main() {
    auto shell = api_examples::local_shell();
    auto root = api_examples::fresh_dir("libsh_api_example_file");
    auto path = root / "service.log";

    auto report = api_examples::run_or_throw(shell, lsh::dsl::redirect(lsh::dsl::cmd("echo", "healthy"), lsh::out(path.string())));
    api_examples::require(report.status.success(), "redirect failed");

    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::cout << ss.str();
}
