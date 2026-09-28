#include "common.hpp"

int main() {
    using namespace lsh::dsl;

    auto dry_run = std::make_shared<lsh::DryRunExecutor>();
    lsh::Shell shell(dry_run);
    shell.env().set("TARGET", "README.md");

    auto report = api_examples::run_or_throw(
        shell,
        cmd("grep", "-n", "LibShell", lsh::variable("TARGET")) | cmd("wc", "-l"));
    api_examples::require(report.status.success(), "dry run failed");

    const auto& specs = dry_run->recorded_specs();
    api_examples::require(specs.size() == 2, "expected two recorded specs");
    api_examples::print_line(specs.front().argv.front());
    api_examples::print_line(specs.back().argv.front());
}
