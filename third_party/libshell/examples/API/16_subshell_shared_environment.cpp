#include "common.hpp"

int main() {
    auto shell = api_examples::local_shell();
    auto report = api_examples::run_or_throw(
        shell,
        lsh::dsl::subshell(lsh::dsl::cmd("export", "SESSION_TOKEN=shared"), lsh::EnvironmentInheritance::shared_explicit));
    api_examples::require(report.status.success(), "shared subshell failed");
    auto value = shell.env().get("SESSION_TOKEN");
    api_examples::require(value.has_value(), "expected shared mutation");
    std::cout << *value << '\n';
}
