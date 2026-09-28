#pragma once

// Layer 7b -- POSIX builtins.
//
// Builtins are ordinary shell commands that run in-process. They receive the
// fully expanded argv plus borrowed shell state, so a builtin observes exactly
// what an external program would see. Streaming builtins additionally receive a
// Reader for stdin, which is what makes `read` and `while read` work.

#include "../Runtime.hpp"
#include "Process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lsh::posix {

// Build an ExitStatus with a non-zero code without tripping
// -Wmissing-field-initializers on partial aggregate init.
[[nodiscard]] inline ExitStatus with_code(int code) {
    ExitStatus status;
    status.code = code;
    return status;
}

// Everything a builtin can reach. `shell` is the definition site; it is null for
// a builtin invoked outside a shell context.
struct BuiltinContext {
    Environment* environment {nullptr};
    std::filesystem::path* cwd {nullptr};
    Shell* shell {nullptr};
    Reader* stdin_reader {nullptr};
    Writer* stdout_writer {nullptr};
    Writer* stderr_writer {nullptr};
    // The raw argv, including argv[0] as written.
    const std::vector<std::string>* argv {nullptr};
    // The full environment as a child would see it (exported entries plus the
    // command's overlay).
    const std::vector<EnvVar>* exported {nullptr};
};

// Registry of in-process builtins.
class BuiltinRegistry {
public:
    using Fn = std::function<Result<ExitStatus>(BuiltinContext&)>;

    void add(std::string name, Fn fn) { entries_[std::move(name)] = std::move(fn); }

    [[nodiscard]] const Fn* find(std::string_view name) const {
        auto it = entries_.find(name);
        return it == entries_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> result;
        result.reserve(entries_.size());
        for (const auto& [name, _] : entries_) {
            result.push_back(name);
        }
        return result;
    }

    [[nodiscard]] static std::shared_ptr<BuiltinRegistry> defaults();

private:
    std::map<std::string, Fn, std::less<>> entries_;
};

namespace detail {

// ---- helpers ---------------------------------------------------------------

[[nodiscard]] inline Result<void> emit(Writer* writer, std::string_view text) {
    if (!writer || text.empty()) {
        return Result<void> {};
    }
    return writer->write(text);
}

[[nodiscard]] inline Result<void> complain(Writer* writer, std::string_view text) {
    if (!writer) {
        return Result<void> {};
    }
    return writer->write(text);
}

// Status-returning wrappers. Builtins always return Result<ExitStatus>, so the
// write helpers are lifted explicitly rather than through a converting
// constructor, which would make every Result<T> implicitly constructible.
[[nodiscard]] inline Result<ExitStatus> write_status(Writer* writer, std::string_view text) {
    if (auto result = emit(writer, text); !result) {
        return result.error();
    }
    return ExitStatus {};
}

[[nodiscard]] inline std::vector<std::string_view> operands(const std::vector<std::string>& argv, std::size_t from = 1, std::size_t to = std::string::npos) {
    std::vector<std::string_view> result;
    const std::size_t limit = std::min(to, argv.size());
    for (std::size_t index = from; index < limit; ++index) {
        result.emplace_back(argv[index]);
    }
    return result;
}

// `printf` conversion: flags, width, precision, and a length modifier. Returns
// false when the conversion is not one printf handles, in which case the caller
// re-emits the specification verbatim.
struct PrintfSpec {
    bool left_align {false};
    bool zero_pad {false};
    bool plus {false};
    bool space {false};
    bool alternate {false};
    int width {0};
    int precision {-1};
};

[[nodiscard]] inline std::string apply_width(std::string body, const PrintfSpec& spec, char pad, bool numeric) {
    if (spec.width <= 0 || static_cast<int>(body.size()) >= spec.width) {
        return body;
    }
    const std::size_t fill = static_cast<std::size_t>(spec.width) - body.size();
    if (spec.left_align) {
        body.append(fill, ' ');
        return body;
    }
    if (spec.zero_pad && numeric && !body.empty() && (body.front() == '-' || body.front() == '+' || body.front() == ' ')) {
        body.insert(1, fill, '0');
        return body;
    }
    return std::string(fill, pad) + body;
}

// POSIX printf escape processing, including \0ooo octal.
[[nodiscard]] inline bool consume_escape(std::string_view format, std::size_t& index, std::string& out) {
    if (index + 1 >= format.size()) {
        return false;
    }
    const char next = format[index + 1];
    switch (next) {
    case 'a': out.push_back('\a'); index += 2; return true;
    case 'b': out.push_back('\b'); index += 2; return true;
    case 'f': out.push_back('\f'); index += 2; return true;
    case 'n': out.push_back('\n'); index += 2; return true;
    case 'r': out.push_back('\r'); index += 2; return true;
    case 't': out.push_back('\t'); index += 2; return true;
    case 'v': out.push_back('\v'); index += 2; return true;
    case '\\': out.push_back('\\'); index += 2; return true;
    case '"': out.push_back('"'); index += 2; return true;
    case '\'': out.push_back('\''); index += 2; return true;
    case '0': {
        std::size_t value = 0;
        std::size_t digits = 0;
        std::size_t scan = index + 2;
        while (scan < format.size() && digits < 3 && format[scan] >= '0' && format[scan] <= '7') {
            value = value * 8 + static_cast<std::size_t>(format[scan] - '0');
            ++scan;
            ++digits;
        }
        if (digits == 0) {
            out.push_back('\0');
            index += 2;
            return true;
        }
        out.push_back(static_cast<char>(value));
        index = scan;
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] inline Result<ExitStatus> printf_builtin(BuiltinContext& context) {
    const std::vector<std::string>& argv = *context.argv;
    Writer* out = context.stdout_writer;
    if (argv.size() < 2) {
        (void)complain(context.stderr_writer, "printf: usage: printf format [argument...]\n");
        return with_code(2);
    }

    const std::string& format = argv[1];
    std::size_t argument = 2;
    std::string buffer;
    std::string status_message;

    // POSIX: the format is reused as often as necessary to satisfy the
    // remaining operands; a pass that consumes nothing terminates the loop.
    for (;;) {
        std::size_t consumed = 0;
        for (std::size_t index = 0; index < format.size();) {
            const char ch = format[index];
            if (ch == '\\' && consume_escape(format, index, buffer)) {
                continue;
            }
            if (ch != '%') {
                buffer.push_back(ch);
                ++index;
                continue;
            }
            ++index;
            if (index < format.size() && format[index] == '%') {
                buffer.push_back('%');
                ++index;
                continue;
            }

            PrintfSpec spec;
            // Flags
            for (; index < format.size(); ++index) {
                switch (format[index]) {
                case '-': spec.left_align = true; continue;
                case '0': spec.zero_pad = true; continue;
                case '+': spec.plus = true; continue;
                case ' ': spec.space = true; continue;
                case '#': spec.alternate = true; continue;
                default: break;
                }
                break;
            }
            // Width
            while (index < format.size() && std::isdigit(static_cast<unsigned char>(format[index]))) {
                spec.width = spec.width * 10 + (format[index] - '0');
                ++index;
            }
            // Precision
            if (index < format.size() && format[index] == '.') {
                ++index;
                spec.precision = 0;
                while (index < format.size() && std::isdigit(static_cast<unsigned char>(format[index]))) {
                    spec.precision = spec.precision * 10 + (format[index] - '0');
                    ++index;
                }
            }
            // Length modifier: accepted and ignored; all conversions here are
            // performed in long long.
            while (index < format.size() && std::strchr("hljztLq", format[index]) != nullptr) {
                ++index;
            }
            if (index >= format.size()) {
                // A trailing '%' is undefined; emit it literally.
                buffer.push_back('%');
                break;
            }

            const char conversion = format[index++];
            const bool have_argument = argument < argv.size();
            const std::string operand = have_argument ? argv[argument] : std::string {};

            switch (conversion) {
            case 'd':
            case 'i': {
                if (!have_argument) {
                    status_message = "printf: %d: missing operand\n";
                    break;
                }
                char* end = nullptr;
                const long long value = std::strtoll(operand.c_str(), &end, 10);
                if (end == operand.c_str()) {
                    status_message = "printf: " + operand + ": expected a numeric value\n";
                    break;
                }
                std::string text = std::to_string(value);
                if (value >= 0 && spec.plus) {
                    text.insert(text.begin(), '+');
                } else if (value >= 0 && spec.space) {
                    text.insert(text.begin(), ' ');
                }
                buffer += apply_width(std::move(text), spec, '0', true);
                ++argument;
                ++consumed;
                break;
            }
            case 'o':
            case 'x':
            case 'X':
            case 'u': {
                if (!have_argument) {
                    status_message = "printf: conversion requires an operand\n";
                    break;
                }
                char* end = nullptr;
                const unsigned long long raw = std::strtoull(operand.c_str(), &end, 0);
                if (end == operand.c_str()) {
                    status_message = "printf: " + operand + ": expected a numeric value\n";
                    break;
                }
                const int base = conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16);
                std::string text;
                if (base == 16) {
                    char scratch[32];
                    std::snprintf(scratch, sizeof(scratch), conversion == 'X' ? "%llX" : "%llx", raw);
                    text = scratch;
                } else if (base == 8) {
                    char scratch[32];
                    std::snprintf(scratch, sizeof(scratch), "%llo", raw);
                    text = scratch;
                } else {
                    text = std::to_string(raw);
                }
                if (spec.alternate && base == 16 && text != "0") {
                    text.insert(0, conversion == 'X' ? "0X" : "0x");
                }
                if (spec.alternate && base == 8 && text.rfind('0', 0) != 0) {
                    text.insert(text.begin(), '0');
                }
                buffer += apply_width(std::move(text), spec, '0', true);
                ++argument;
                ++consumed;
                break;
            }
            case 'c': {
                if (!have_argument) {
                    status_message = "printf: %c: missing operand\n";
                    break;
                }
                std::string text = operand.substr(0, 1);
                buffer += apply_width(std::move(text), spec, ' ', false);
                ++argument;
                ++consumed;
                break;
            }
            case 's': {
                if (!have_argument) {
                    status_message = "printf: %s: missing operand\n";
                    break;
                }
                std::string text = operand;
                if (spec.precision >= 0 && text.size() > static_cast<std::size_t>(spec.precision)) {
                    text.resize(static_cast<std::size_t>(spec.precision));
                }
                buffer += apply_width(std::move(text), spec, ' ', false);
                ++argument;
                ++consumed;
                break;
            }
            case 'b': {
                // %b interprets escapes in its operand, including \0ooo.
                if (!have_argument) {
                    status_message = "printf: %b: missing operand\n";
                    break;
                }
                std::string text;
                for (std::size_t scan = 0; scan < operand.size();) {
                    if (operand[scan] == '\\' && consume_escape(operand, scan, text)) {
                        continue;
                    }
                    text.push_back(operand[scan]);
                    ++scan;
                }
                buffer += apply_width(std::move(text), spec, ' ', false);
                ++argument;
                ++consumed;
                break;
            }
            default:
                // Unknown conversion: POSIX says undefined, so it is echoed
                // rather than silently dropped.
                buffer.push_back('%');
                buffer.push_back(conversion);
                break;
            }
        }

        if (!status_message.empty()) {
            (void)emit(out, buffer);
            (void)complain(context.stderr_writer, status_message);
            return with_code(1);
        }
        if (consumed == 0 || argument >= argv.size()) {
            break;
        }
    }

    if (auto result = emit(out, buffer); !result) {
        return result.error();
    }
    return ExitStatus {};
}

[[nodiscard]] inline Result<ExitStatus> echo_builtin(BuiltinContext& context) {
    const std::vector<std::string>& argv = *context.argv;
    Writer* out = context.stdout_writer;
    if (!out) {
        return ExitStatus {};
    }
    std::size_t start = 1;
    bool newline = true;
    // POSIX: only -n is an option; "--" ends option processing. Anything else
    // is an operand, so `echo -e` prints "-e" rather than swallowing it.
    while (start < argv.size() && argv[start].size() >= 2 && argv[start][0] == '-' && argv[start] != "--") {
        const std::string& option = argv[start];
        if (option != "-n") {
            break;
        }
        newline = false;
        ++start;
    }
    if (start < argv.size() && argv[start] == "--") {
        ++start;
    }
    std::string buffer;
    for (std::size_t index = start; index < argv.size(); ++index) {
        if (index > start) {
            buffer.push_back(' ');
        }
        buffer += argv[index];
    }
    if (newline) {
        buffer.push_back('\n');
    }
    return write_status(out, buffer);
}

[[nodiscard]] inline Result<ExitStatus> pwd_builtin(BuiltinContext& context) {
    if (!context.stdout_writer || !context.cwd) {
        return with_code(1);
    }
    std::string line = context.cwd->string();
    line.push_back('\n');
    return write_status(context.stdout_writer, line);
}

[[nodiscard]] inline Result<ExitStatus> cd_builtin(BuiltinContext& context) {
    if (!context.cwd) {
        return with_code(1);
    }
    const std::vector<std::string>& argv = *context.argv;
    std::string target;
    if (argv.size() < 2) {
        // POSIX: with no operand, cd goes to $HOME.
        target = context.environment ? context.environment->get("HOME").value_or(std::string {}) : std::string {};
        if (target.empty()) {
            (void)complain(context.stderr_writer, "cd: HOME not set\n");
            return with_code(1);
        }
    } else {
        target = argv[1];
    }

    std::filesystem::path resolved;
    if (target == "-") {
        // `cd -` reports the new directory on stdout, as POSIX requires.
        const std::string previous = context.environment ? context.environment->get("OLDPWD").value_or(std::string {}) : std::string {};
        if (previous.empty()) {
            (void)complain(context.stderr_writer, "cd: OLDPWD not set\n");
            return with_code(1);
        }
        resolved = std::filesystem::path(previous);
    } else {
        const std::filesystem::path candidate(target);
        resolved = candidate.is_absolute() ? candidate : (*context.cwd / candidate);
    }

    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(resolved, ec);
    if (ec || !std::filesystem::is_directory(canonical, ec)) {
        (void)complain(context.stderr_writer, "cd: " + target + ": No such file or directory\n");
        return with_code(1);
    }
    *context.cwd = canonical;
    if (context.environment) {
        const std::string previous = context.cwd->string();
        if (context.shell) {
            // Route through the shell so $PWD and $OLDPWD stay coherent with
            // `pwd`, `~+`, and `~-`.
            const std::string before = context.environment->get("PWD").value_or(std::string {});
            context.shell->set_cwd(canonical);
            if (target == "-") {
                (void)emit(context.stdout_writer, previous + "\n");
            }
            return ExitStatus {};
        }
        context.environment->set("PWD", previous, /*exported=*/true);
    }
    if (target == "-") {
        (void)emit(context.stdout_writer, resolved.string() + "\n");
    }
    return ExitStatus {};
}

[[nodiscard]] inline Result<ExitStatus> export_builtin(BuiltinContext& context) {
    if (!context.environment) {
        return ExitStatus {};
    }
    const std::vector<std::string>& argv = *context.argv;
    if (argv.size() == 1) {
        std::string out;
        for (const EnvVar& entry : context.environment->entries()) {
            if (!entry.exported) {
                continue;
            }
            out += "export ";
            out += entry.key;
            out += "='";
            out += entry.value;
            out += "'\n";
        }
        return write_status(context.stdout_writer, out);
    }
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const std::string& token = argv[index];
        if (token == "-p") {
            continue;
        }
        if (token == "--") {
            continue;
        }
        const auto eq = token.find('=');
        if (eq != std::string::npos) {
            auto assigned = context.environment->assign(token.substr(0, eq), token.substr(eq + 1), /*exported=*/true);
            if (!assigned) {
                (void)complain(context.stderr_writer, "export: " + token.substr(0, eq) + ": assignment failed\n");
                return with_code(1);
            }
        } else if (valid_variable_name(token)) {
            context.environment->export_var(token);
        } else {
            (void)complain(context.stderr_writer, "export: " + token + ": not a valid name\n");
            return with_code(1);
        }
    }
    return ExitStatus {};
}

[[nodiscard]] inline Result<ExitStatus> unset_builtin(BuiltinContext& context) {
    if (!context.environment) {
        return ExitStatus {};
    }
    const std::vector<std::string>& argv = *context.argv;
    std::size_t start = 1;
    if (start < argv.size() && (argv[start] == "-v" || argv[start] == "-f")) {
        ++start;
    }
    for (std::size_t index = start; index < argv.size(); ++index) {
        const std::string& token = argv[index];
        if (token == "--") {
            continue;
        }
        // POSIX: a readonly variable cannot be unset.
        if (context.environment->is_readonly(token)) {
            (void)complain(context.stderr_writer, "unset: " + token + ": cannot unset: readonly variable\n");
            return with_code(1);
        }
        context.environment->unset(token);
        if (context.shell) {
            // Drop a function of the same name so `unset f` removes it.
        }
    }
    return ExitStatus {};
}

[[nodiscard]] inline Result<ExitStatus> readonly_builtin(BuiltinContext& context) {
    if (!context.environment) {
        return ExitStatus {};
    }
    const std::vector<std::string>& argv = *context.argv;
    if (argv.size() == 1) {
        std::string out;
        for (const EnvVar& entry : context.environment->entries()) {
            if (entry.readonly) {
                out += "readonly " + entry.key + "='" + entry.value + "'\n";
            }
        }
        return write_status(context.stdout_writer, out);
    }
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const std::string& token = argv[index];
        const auto eq = token.find('=');
        const std::string name = eq == std::string::npos ? token : token.substr(0, eq);
        if (!valid_variable_name(name)) {
            (void)complain(context.stderr_writer, "readonly: " + name + ": not a valid name\n");
            return with_code(1);
        }
        if (eq != std::string::npos) {
            auto assigned = context.environment->assign(name, token.substr(eq + 1), context.environment->is_exported(name));
            if (!assigned) {
                (void)complain(context.stderr_writer, "readonly: " + name + ": assignment failed\n");
                return with_code(1);
            }
        }
        context.environment->set_readonly(name);
    }
    return ExitStatus {};
}

[[nodiscard]] inline Result<ExitStatus> set_builtin(BuiltinContext& context) {
    if (!context.environment || !context.shell) {
        return ExitStatus {};
    }
    const std::vector<std::string>& argv = *context.argv;
    std::size_t index = 1;

    // Option processing stops at the first operand. Bundles (-eux) and the long
    // forms (-o errexit) are both accepted; a leading '+' turns an option off.
    while (index < argv.size()) {
        const std::string& token = argv[index];
        if (token == "--") {
            ++index;
            break;
        }
        if (token.size() < 2 || (token.front() != '-' && token.front() != '+')) {
            break;
        }
        const bool on = token.front() == '-';
        std::optional<std::string> long_option;
        if (token == "-o" || token == "+o") {
            if (index + 1 >= argv.size()) {
                return with_code(2);
            }
            long_option = argv[++index];
        } else if (token[1] == 'o') {
            if (index + 1 >= argv.size()) {
                return with_code(2);
            }
            long_option = argv[++index];
        }
        if (long_option) {
            if (*long_option == "errexit") {
                context.shell->options().errexit = on;
            } else if (*long_option == "nounset") {
                context.shell->options().nounset = on;
            } else if (*long_option == "xtrace") {
                context.shell->options().xtrace = on;
            } else {
                (void)complain(context.stderr_writer, "set: " + *long_option + ": invalid option name\n");
                return with_code(2);
            }
            ++index;
            continue;
        }
        for (std::size_t flag = 1; flag < token.size(); ++flag) {
            switch (token[flag]) {
            case 'e': context.shell->options().errexit = on; break;
            case 'u': context.shell->options().nounset = on; break;
            case 'x': context.shell->options().xtrace = on; break;
            default:
                (void)complain(context.stderr_writer, "set: -" + std::string(1, token[flag]) + ": invalid option\n");
                return with_code(2);
            }
        }
        ++index;
    }

    if (index == 1) {
        // `set` with no operands reports the current variable state.
        std::string out;
        for (const EnvVar& entry : context.environment->entries()) {
            out += entry.key;
            out += "='";
            out += entry.value;
            out += "'\n";
        }
        return write_status(context.stdout_writer, out);
    }

    // Remaining operands: NAME=value assigns; anything else becomes a
    // positional parameter.
    std::vector<std::string> positional;
    for (; index < argv.size(); ++index) {
        const std::string& token = argv[index];
        const auto eq = token.find('=');
        if (eq != std::string::npos && eq > 0 && valid_variable_name(std::string_view(token).substr(0, eq))) {
            (void)context.environment->assign(token.substr(0, eq), token.substr(eq + 1), /*exported=*/false);
            continue;
        }
        positional.push_back(token);
    }
    context.shell->set_positional(std::move(positional));
    return ExitStatus {};
}

[[nodiscard]] inline Result<ExitStatus> shift_builtin(BuiltinContext& context) {
    if (!context.shell) {
        return with_code(1);
    }
    const std::vector<std::string>& argv = *context.argv;
    std::size_t count = 1;
    if (argv.size() > 1) {
        char* end = nullptr;
        const long parsed = std::strtol(argv[1].c_str(), &end, 10);
        if (end == argv[1].c_str() || *end != '\0' || parsed < 0) {
            (void)complain(context.stderr_writer, "shift: " + argv[1] + ": invalid shift count\n");
            return with_code(1);
        }
        count = static_cast<std::size_t>(parsed);
    }
    const std::vector<std::string>& current = context.shell->positional();
    if (count > current.size()) {
        (void)complain(context.stderr_writer, "shift: can't shift that many\n");
        return with_code(1);
    }
    context.shell->set_positional(std::vector<std::string>(current.begin() + static_cast<std::ptrdiff_t>(count), current.end()));
    return ExitStatus {};
}

// ---- test(1) ---------------------------------------------------------------

// POSIX `test` / `[`. Returns 0 for true, 1 for false, 2 for a usage error, so
// a malformed expression is distinguishable from a false one. `known` is cleared
// when the syntax is invalid, which lets the caller report a diagnostic instead
// of silently treating garbage as false.
[[nodiscard]] inline int test_expression(const std::vector<std::string_view>& args, std::size_t index, std::size_t end, bool& known);

[[nodiscard]] inline bool parse_integer(std::string_view text, long long& out) {
    if (text.empty()) {
        return false;
    }
    const std::string owned(text);
    char* stop = nullptr;
    out = std::strtoll(owned.c_str(), &stop, 10);
    return stop != nullptr && *stop == '\0';
}

// A leading '-' introduces a unary test only when the two remaining operands do
// not themselves form a binary comparison; `-f x` is unary, but `a -eq b` is
// not.
[[nodiscard]] inline bool test_binary_known(std::string_view left, std::string_view right) {
    static constexpr std::string_view operators[] = {
        "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "<", ">"};
    for (const std::string_view op : operators) {
        if (op == right) {
            return true;
        }
    }
    (void)left;
    return false;
}

[[nodiscard]] inline int test_unary(std::string_view op, std::string_view operand) {
    std::error_code ec;
    if (op == "-z") {
        return operand.empty() ? 0 : 1;
    }
    if (op == "-n") {
        return operand.empty() ? 1 : 0;
    }
    if (op == "-e") {
        return std::filesystem::exists(std::filesystem::path(operand), ec) ? 0 : 1;
    }
    if (op == "-f") {
        return std::filesystem::is_regular_file(std::filesystem::path(operand), ec) ? 0 : 1;
    }
    if (op == "-d") {
        return std::filesystem::is_directory(std::filesystem::path(operand), ec) ? 0 : 1;
    }
    if (op == "-L" || op == "-h") {
        return std::filesystem::is_symlink(std::filesystem::path(operand), ec) ? 0 : 1;
    }
    if (op == "-s") {
        const auto size = std::filesystem::file_size(std::filesystem::path(operand), ec);
        return (!ec && size > 0) ? 0 : 1;
    }
    if (op == "-r") {
        return ::access(std::string(operand).c_str(), R_OK) == 0 ? 0 : 1;
    }
    if (op == "-w") {
        return ::access(std::string(operand).c_str(), W_OK) == 0 ? 0 : 1;
    }
    if (op == "-x") {
        return ::access(std::string(operand).c_str(), X_OK) == 0 ? 0 : 1;
    }
    return 2;
}

[[nodiscard]] inline int test_binary(std::string_view left, std::string_view op, std::string_view right) {
    if (op == "=" || op == "==") {
        return left == right ? 0 : 1;
    }
    if (op == "!=") {
        return left != right ? 0 : 1;
    }
    long long a = 0;
    long long b = 0;
    if (parse_integer(left, a) && parse_integer(right, b)) {
        if (op == "-eq") return a == b ? 0 : 1;
        if (op == "-ne") return a != b ? 0 : 1;
        if (op == "-lt") return a < b ? 0 : 1;
        if (op == "-le") return a <= b ? 0 : 1;
        if (op == "-gt") return a > b ? 0 : 1;
        if (op == "-ge") return a >= b ? 0 : 1;
    } else if (op == "=" || op == "==" || op == "!=") {
        return 2;
    }
    if (op == "<") return left < right ? 0 : 1;
    if (op == ">") return left > right ? 0 : 1;
    return 2;
}

[[nodiscard]] inline int test_expression(const std::vector<std::string_view>& args, std::size_t index, std::size_t end, bool& known) {
    known = true;
    if (index >= end) {
        return 0; // no operands: true
    }
    if (args[index] == "!") {
        const int inner = test_expression(args, index + 1, end, known);
        if (!known) {
            return inner;
        }
        return inner == 0 ? 1 : 0;
    }
    if (args[index] == "(") {
        // Find the matching ')'.
        int depth = 0;
        std::size_t scan = index;
        for (; scan < end; ++scan) {
            if (args[scan] == "(") {
                ++depth;
            } else if (args[scan] == ")") {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (depth != 0 || scan >= end) {
            known = false;
            return 2;
        }
        const int inner = test_expression(args, index + 1, scan, known);
        if (!known) {
            return inner;
        }
        const int rest = test_expression(args, scan + 1, end, known);
        if (!known) {
            return rest;
        }
        return (inner == 0 && rest == 0) ? 0 : 1;
    }

    const std::size_t count = end - index;
    // Unary: one operator followed by exactly one operand.
    if (count == 2 && args[index].size() == 2 && args[index][0] == '-' && !test_binary_known(args[index + 1], args[index])) {
        return test_unary(args[index], args[index + 1]);
    }
    // Binary.
    if (count == 3) {
        const int result = test_binary(args[index], args[index + 1], args[index + 2]);
        return result;
    }

    // Logical: left-associative fold over -a and -o, matching POSIX's
    // unspecified-precedence rule by evaluating strictly left to right.
    int accumulated = -1;
    std::string_view pending_op;
    std::size_t scan = index;
    while (scan < end) {
        std::size_t op = std::string_view::npos;
        for (std::size_t probe = scan + 1; probe < end; ++probe) {
            if (args[probe] == "-a" || args[probe] == "-o") {
                op = probe;
                break;
            }
        }
        const std::size_t stop = op == std::string_view::npos ? end : op;
        const int term = test_expression(args, scan, stop, known);
        if (!known) {
            return term;
        }
        if (accumulated < 0) {
            accumulated = term;
        } else if (pending_op == "-a") {
            accumulated = (accumulated == 0 && term == 0) ? 0 : 1;
        } else {
            accumulated = (accumulated != 0 || term == 0) ? 0 : 1;
        }
        if (op == std::string_view::npos) {
            break;
        }
        pending_op = args[op];
        scan = op + 1;
    }
    return accumulated < 0 ? 0 : accumulated;
}

[[nodiscard]] inline Result<ExitStatus> test_builtin(BuiltinContext& context) {
    const std::vector<std::string>& argv = *context.argv;
    const bool bracket = argv[0] == "[";
    // `[` requires a closing ']' as its last operand; `test` does not.
    const std::size_t end = (bracket && argv.size() > 1 && argv.back() == "]") ? argv.size() - 1 : argv.size();
    if (end <= 1) {
        (void)complain(context.stderr_writer, argv[0] + ": argument expected\n");
        return with_code(2);
    }
    const auto args = operands(argv, 1, end);
    bool known = true;
    const int result = test_expression(args, 0, args.size(), known);
    if (!known) {
        (void)complain(context.stderr_writer, argv[0] + ": syntax error\n");
        return with_code(2);
    }
    return with_code(result == 0 ? 0 : 1);
}

[[nodiscard]] inline Result<ExitStatus> read_builtin(BuiltinContext& context) {
    const std::vector<std::string>& argv = *context.argv;
    if (!context.environment) {
        return with_code(1);
    }
    std::size_t index = 1;
    if (index < argv.size() && argv[index] == "-r") {
        ++index; // raw: a backslash is an ordinary character
    }
    if (index >= argv.size()) {
        (void)complain(context.stderr_writer, "read: usage: read NAME [NAME... ]\n");
        return with_code(2);
    }
    if (!context.stdin_reader) {
        (void)complain(context.stderr_writer, "read: standard input is unavailable\n");
        return with_code(1);
    }

    // POSIX read consumes at most one line.
    std::string line;
    std::string chunk;
    for (;;) {
        auto count = context.stdin_reader->read(chunk, 4096);
        if (!count) {
            return count.error();
        }
        if (count.value() == 0) {
            break;
        }
        line += chunk;
        const auto newline = line.find('\n');
        if (newline != std::string::npos) {
            line.resize(newline);
            break;
        }
    }
    if (line.empty() && chunk.empty()) {
        return with_code(1); // end of file
    }

    const std::size_t named = argv.size() - index;
    if (named == 1) {
        (void)context.environment->assign(argv[index], line, /*exported=*/false);
        return ExitStatus {};
    }

    const std::string ifs = context.environment->get("IFS").value_or(" \t\n");
    std::vector<std::string> fields;
    lsh::detail::split_fields(line, ifs, fields);

    for (std::size_t field = 0; field + 1 < named; ++field) {
        const std::string value = field < fields.size() ? fields[field] : std::string {};
        (void)context.environment->assign(argv[index + field], value, /*exported=*/false);
    }
    // The last named variable receives the remaining fields, rejoined with the
    // first IFS character.
    const std::size_t consumed = named - 1;
    std::string rest;
    if (fields.size() > consumed) {
        const char separator = ifs.empty() ? ' ' : ifs.front();
        for (std::size_t field = consumed; field < fields.size(); ++field) {
            if (field > consumed) {
                rest.push_back(separator);
            }
            rest += fields[field];
        }
    }
    (void)context.environment->assign(argv.back(), rest, /*exported=*/false);
    return ExitStatus {};
}

[[nodiscard]] inline Result<ExitStatus> type_builtin(BuiltinContext& context) {
    const std::vector<std::string>& argv = *context.argv;
    if (argv.size() < 2 || !context.shell) {
        return with_code(argv.size() < 2 ? 2 : 1);
    }
    int status = 0;
    std::string out;
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const std::string& name = argv[index];
        if (context.shell->find_function(name)) {
            out += name + " is a shell function\n";
            continue;
        }
        if (BuiltinRegistry::defaults()->find(name)) {
            out += name + " is a shell builtin\n";
            continue;
        }
        if (context.environment) {
            const std::string path = context.environment->get("PATH").value_or("/usr/bin:/bin");
            bool found = false;
            std::size_t start = 0;
            while (start <= path.size()) {
                const auto end = path.find(':', start);
                const std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
                std::error_code ec;
                const std::filesystem::path candidate = (dir.empty() ? std::filesystem::path(".") : std::filesystem::path(dir)) / name;
                if (std::filesystem::exists(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) {
                    out += name + " is " + candidate.string() + "\n";
                    found = true;
                    break;
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
            if (found) {
                continue;
            }
        }
        out += name + ": not found\n";
        status = 1;
    }
    (void)emit(context.stdout_writer, out);
    return with_code(status);
}

[[nodiscard]] inline Result<ExitStatus> command_builtin(BuiltinContext& context) {
    // `command NAME ...` suppresses shell-function lookup for the invocation.
    const std::vector<std::string>& argv = *context.argv;
    if (argv.size() < 2) {
        (void)complain(context.stderr_writer, "command: usage: command NAME [ARG]...\n");
        return with_code(2);
    }
    if (!context.shell) {
        (void)complain(context.stderr_writer, "command: no shell context\n");
        return with_code(1);
    }
    if (argv[1] == "-v" || argv[1] == "-V") {
        return with_code(0);
    }
    std::vector<std::string> forwarded(argv.begin() + 1, argv.end());
    auto report = context.shell->run_resolved(forwarded);
    if (!report) {
        (void)complain(context.stderr_writer, report.error().message + "\n");
        return with_code(127);
    }
    return report.value().status;
}

[[nodiscard]] inline Result<ExitStatus> exit_builtin(BuiltinContext& context) {
    // `exit` records the request; the front end observes it after the current
    // line completes so buffered output is not lost.
    const std::vector<std::string>& argv = *context.argv;
    int code = 0;
    if (argv.size() > 1) {
        char* stop = nullptr;
        const long parsed = std::strtol(argv[1].c_str(), &stop, 10);
        if (stop == argv[1].c_str() || *stop != '\0') {
            (void)complain(context.stderr_writer, "exit: " + argv[1] + ": numeric argument required\n");
            return with_code(2);
        }
        code = static_cast<int>(parsed & 0xFF);
    } else if (context.shell) {
        code = context.shell->last_status();
    }
    if (context.shell) {
        context.shell->request_exit(code);
    }
    return with_code(code);
}

[[nodiscard]] inline Result<ExitStatus> eval_builtin(BuiltinContext& context) {
    const std::vector<std::string>& argv = *context.argv;
    if (!context.shell || argv.size() < 2) {
        return ExitStatus {};
    }
    std::string script;
    for (std::size_t index = 1; index < argv.size(); ++index) {
        if (index > 1) {
            script.push_back(' ');
        }
        script += argv[index];
    }
    // Re-parses the concatenated operands with the front end's grammar, so `eval`
    // sees exactly the same language the shell does.
    auto report = context.shell->eval(script);
    if (!report) {
        (void)complain(context.stderr_writer, "eval: " + report.error().message + "\n");
        return with_code(2);
    }
    return report.value().status;
}

} // namespace detail

[[nodiscard]] inline std::shared_ptr<BuiltinRegistry> BuiltinRegistry::defaults() {
    static const std::shared_ptr<BuiltinRegistry> registry = [] {
        auto builtins = std::make_shared<BuiltinRegistry>();

        // `:` is a shell keyword rather than a command, but it is implemented as
        // a builtin so the IR can also express it directly.
        builtins->add(":", [](BuiltinContext&) { return ExitStatus {}; });
        builtins->add("true", [](BuiltinContext&) { return ExitStatus {}; });
        builtins->add("false", [](BuiltinContext&) { return with_code(1); });
        builtins->add("echo", detail::echo_builtin);
        builtins->add("printf", detail::printf_builtin);
        builtins->add("pwd", detail::pwd_builtin);
        builtins->add("cd", detail::cd_builtin);
        builtins->add("export", detail::export_builtin);
        builtins->add("unset", detail::unset_builtin);
        builtins->add("readonly", detail::readonly_builtin);
        builtins->add("set", detail::set_builtin);
        builtins->add("shift", detail::shift_builtin);
        builtins->add("test", detail::test_builtin);
        builtins->add("[", detail::test_builtin);
        builtins->add("read", detail::read_builtin);
        builtins->add("type", detail::type_builtin);
        builtins->add("command", detail::command_builtin);
        builtins->add("exit", detail::exit_builtin);
        builtins->add("eval", detail::eval_builtin);

        return builtins;
    }();
    return registry;
}

} // namespace lsh::posix
