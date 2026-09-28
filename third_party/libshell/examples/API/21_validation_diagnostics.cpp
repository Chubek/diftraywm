#include "common.hpp"

int main() {
    // Two deliberately malformed fragments: an unknown expansion kind, and a
    // literal carrying a NUL byte. Both must be rejected before execution.
    lsh::Expansion unknown_kind;
    unknown_kind.kind = static_cast<lsh::ExpansionKind>(255);
    unknown_kind.text = "bad";

    lsh::Expansion nul_text;
    nul_text.kind = lsh::ExpansionKind::raw;
    nul_text.text = std::string("nul\0byte", 8);

    lsh::ir::Command command;
    command.argv.push_back(lsh::Argument {{unknown_kind}});
    command.argv.push_back(lsh::Argument {{nul_text}});

    auto report = lsh::ir::validate(lsh::ir::Program {lsh::ir::command(std::move(command), "bad-command")});
    std::cout << "diagnostics=" << report.diagnostics.size() << '\n';
    for (const auto& diagnostic : report.diagnostics) {
        std::cout << diagnostic.path << ": " << diagnostic.message << '\n';
    }
}
