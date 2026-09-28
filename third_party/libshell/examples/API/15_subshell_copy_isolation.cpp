#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto report = api_examples::run_or_throw(
        shell,
        lsh::dsl::subshell(lsh::dsl::cmd("set", "SESSION_TOKEN=ephemeral"), lsh::EnvironmentInheritance::copy));
    api_examples::require(report.status.success(), "isolated subshell failed");
    std::cout << (shell.env().get("SESSION_TOKEN").has_value() ? "present\n" : "absent\n");
}
