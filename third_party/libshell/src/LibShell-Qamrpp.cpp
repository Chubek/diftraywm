#include "lsh/Scripting.hpp"

#include "lsh/Expansion.hpp"

#include <QaMRpp.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace lsh::scripting {
namespace {

// A shell variable name is also a valid Lua identifier only when it matches the
// identifier grammar; anything else is reachable through env(NAME) alone.
[[nodiscard]] bool identifier_safe(std::string_view name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_')) {
        return false;
    }
    return std::all_of(
        name.begin() + 1, name.end(), [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; });
}

} // namespace

Result<std::string> eval_lua_qamrpp(std::string_view script, const Environment& environment) {
    try {
        qamrpp::Context context;
        // env(NAME) is the only escape hatch, and it is read-only: an inline
        // script cannot mutate the shell's state.
        context.register_native(
            "env",
            [&environment](qamrpp::Context&, std::vector<qamrpp::ValuePtr>& args) {
                if (args.empty() || !args.front()) {
                    return std::make_shared<qamrpp::Value>();
                }
                auto value = environment.get(args.front()->to_string());
                return value ? std::make_shared<qamrpp::Value>(*value) : std::make_shared<qamrpp::Value>();
            });
        for (const EnvVar& entry : environment.entries()) {
            if (identifier_safe(entry.key)) {
                context.globals[entry.key] = std::make_shared<qamrpp::Value>(entry.value);
            }
        }

        qamrpp::ValuePtr value = context.run(std::string(script));
        return value ? value->to_string() : std::string {};
    } catch (const std::exception& error) {
        return failure(ErrorCode::bad_expansion, std::string("Lua evaluation failed: ") + error.what(), std::string(script));
    }
}

} // namespace lsh::scripting
