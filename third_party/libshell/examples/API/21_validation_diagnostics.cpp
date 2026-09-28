#include "common.hpp"

int main() {
    lsh::ir::Command command;
    command.argv.push_back(lsh::Argument {{lsh::Expansion {.kind = static_cast<lsh::ExpansionKind>(255), .text = "bad"}}});
    command.argv.push_back(lsh::Argument {{lsh::Expansion {.kind = lsh::ExpansionKind::raw, .text = std::string("nul\0byte", 8)}}});

    auto report = lsh::ir::validate(lsh::ir::Program {lsh::ir::command(std::move(command), "bad-command")});
    std::cout << "diagnostics=" << report.diagnostics.size() << '\n';
    for (const auto& diagnostic : report.diagnostics) {
        std::cout << diagnostic.path << ": " << diagnostic.message << '\n';
    }
}
