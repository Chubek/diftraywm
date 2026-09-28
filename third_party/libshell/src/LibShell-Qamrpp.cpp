#include "LibShell.hpp"

#include <QaMRpp.hpp>

namespace lsh::detail {

Result<std::string> eval_qamrpp_lua(std::string_view script, const Environment& environment) {
    auto identifier_safe = [](std::string_view name) {
        if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) {
            return false;
        }
        return std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_';
        });
    };

    try {
        qamrpp::Context context;
        context.register_native("env", [&environment](qamrpp::Context&, std::vector<qamrpp::ValuePtr>& args) {
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
        return Diagnostic {ErrorCode::bad_expansion, std::string("Lua evaluation failed: ") + error.what(), std::string(script)};
    }
}

} // namespace lsh::detail
