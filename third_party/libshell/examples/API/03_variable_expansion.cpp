#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    shell.env().set("SERVICE_NAME", "api-gateway");
    auto output = api_examples::capture_stdout(shell, lsh::dsl::cmd("echo", lsh::variable("SERVICE_NAME")));
    std::cout << output;
}
