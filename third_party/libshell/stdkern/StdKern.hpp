#pragma once

#include "LibShell-Kernel.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lsh::stdkern {
namespace detail {

inline kernel::Version version() { return kernel::Version {0, 1, 0, "stdkern"}; }

inline Result<void> write(Writer* writer, std::string_view bytes) {
    if (!writer || bytes.empty()) {
        return {};
    }
    return writer->write(bytes);
}

inline ExitStatus status(int code) {
    ExitStatus result;
    result.code = code;
    return result;
}

inline Result<ExitStatus> ok() { return ExitStatus {}; }
inline Result<ExitStatus> fail(int code = 1) { return status(code); }

inline kernel::Metadata metadata(std::string name, std::string summary, std::string documentation) {
    kernel::Metadata result;
    result.name = std::move(name);
    result.version = version();
    result.summary = std::move(summary);
    result.documentation = std::move(documentation);
    return result;
}

inline bool has_echo_n(const std::vector<std::string>& argv, std::size_t& first_operand) {
    first_operand = 1;
    bool newline = true;
    while (first_operand < argv.size() && argv[first_operand] == "-n") {
        newline = false;
        ++first_operand;
    }
    return newline;
}

inline Result<ExitStatus> echo(const kernel::Invocation& invocation) {
    std::size_t first_operand = 1;
    const bool newline = has_echo_n(invocation.argv, first_operand);
    std::string out;
    for (std::size_t index = first_operand; index < invocation.argv.size(); ++index) {
        if (index > first_operand) {
            out.push_back(' ');
        }
        out += invocation.argv[index];
    }
    if (newline) {
        out.push_back('\n');
    }
    if (auto result = write(invocation.stdout_writer, out); !result) {
        return result.error();
    }
    return ok();
}

inline Result<ExitStatus> printf_kernel(const kernel::Invocation& invocation) {
    if (invocation.argv.size() < 2) {
        return fail(1);
    }
    const std::string& format = invocation.argv[1];
    std::size_t argument = 2;
    std::string out;
    for (;;) {
        std::size_t consumed = 0;
        for (std::size_t index = 0; index < format.size(); ++index) {
            char ch = format[index];
            if (ch == '\\' && index + 1 < format.size()) {
                char escaped = format[++index];
                out.push_back(escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped);
                continue;
            }
            if (ch == '%' && index + 1 < format.size()) {
                char specifier = format[++index];
                if (specifier == 's') {
                    if (argument < invocation.argv.size()) {
                        out += invocation.argv[argument++];
                        ++consumed;
                    }
                } else if (specifier == 'd') {
                    if (argument < invocation.argv.size()) {
                        out += invocation.argv[argument++];
                        ++consumed;
                    } else {
                        out.push_back('0');
                    }
                } else if (specifier == '%') {
                    out.push_back('%');
                } else {
                    out.push_back('%');
                    out.push_back(specifier);
                }
                continue;
            }
            out.push_back(ch);
        }
        if (consumed == 0 || argument >= invocation.argv.size()) {
            break;
        }
    }
    if (auto result = write(invocation.stdout_writer, out); !result) {
        return result.error();
    }
    return ok();
}

inline Result<ExitStatus> basename_kernel(const kernel::Invocation& invocation) {
    if (invocation.argv.size() < 2 || invocation.argv.size() > 3) {
        if (auto result = write(invocation.stderr_writer, "basename: expected NAME [SUFFIX]\n"); !result) {
            return result.error();
        }
        return fail(1);
    }
    std::filesystem::path path(invocation.argv[1]);
    std::string name = path.filename().string();
    if (name.empty()) {
        name = path.parent_path().filename().string();
    }
    if (invocation.argv.size() == 3 && name.size() >= invocation.argv[2].size()
        && name.compare(name.size() - invocation.argv[2].size(), invocation.argv[2].size(), invocation.argv[2]) == 0) {
        name.resize(name.size() - invocation.argv[2].size());
    }
    name.push_back('\n');
    if (auto result = write(invocation.stdout_writer, name); !result) {
        return result.error();
    }
    return ok();
}

inline Result<ExitStatus> dirname_kernel(const kernel::Invocation& invocation) {
    if (invocation.argv.size() != 2) {
        if (auto result = write(invocation.stderr_writer, "dirname: expected NAME\n"); !result) {
            return result.error();
        }
        return fail(1);
    }
    std::filesystem::path path(invocation.argv[1]);
    std::string name = path.parent_path().string();
    if (name.empty()) {
        name = ".";
    }
    name.push_back('\n');
    if (auto result = write(invocation.stdout_writer, name); !result) {
        return result.error();
    }
    return ok();
}

inline Result<ExitStatus> env_kernel(const kernel::Invocation& invocation) {
    if (invocation.argv.size() != 1) {
        if (auto result = write(invocation.stderr_writer, "env: command execution is outside stdkern scope\n"); !result) {
            return result.error();
        }
        return fail(1);
    }
    if (!invocation.environment) {
        return ok();
    }
    std::string out;
    for (const EnvVar& entry : invocation.environment->exported_entries()) {
        out += entry.key;
        out.push_back('=');
        out += entry.value;
        out.push_back('\n');
    }
    if (auto result = write(invocation.stdout_writer, out); !result) {
        return result.error();
    }
    return ok();
}

inline Result<ExitStatus> printenv_kernel(const kernel::Invocation& invocation) {
    if (!invocation.environment) {
        return ok();
    }
    std::string out;
    if (invocation.argv.size() == 1) {
        for (const EnvVar& entry : invocation.environment->exported_entries()) {
            out += entry.key;
            out.push_back('=');
            out += entry.value;
            out.push_back('\n');
        }
    } else {
        for (std::size_t index = 1; index < invocation.argv.size(); ++index) {
            if (auto value = invocation.environment->get(invocation.argv[index])) {
                out += *value;
                out.push_back('\n');
            }
        }
    }
    if (auto result = write(invocation.stdout_writer, out); !result) {
        return result.error();
    }
    return ok();
}

inline std::shared_ptr<kernel::FunctionKernel> function_kernel(
    kernel::Metadata metadata,
    kernel::FunctionKernel::ExecuteFn execute) {
    return std::make_shared<kernel::FunctionKernel>(std::move(metadata), std::move(execute));
}

} // namespace detail

inline std::shared_ptr<kernel::Registry> registry() {
    auto result = std::make_shared<kernel::Registry>();

    (void)result->add(detail::function_kernel(
        detail::metadata(":", "null command", "Return success without output."),
        [](const kernel::Invocation&) { return detail::ok(); }));
    (void)result->add(detail::function_kernel(
        detail::metadata("true", "successful command", "Return zero without output."),
        [](const kernel::Invocation&) { return detail::ok(); }));
    (void)result->add(detail::function_kernel(
        detail::metadata("false", "failing command", "Return one without output."),
        [](const kernel::Invocation&) { return detail::fail(); }));
    (void)result->add(detail::function_kernel(
        detail::metadata("echo", "write arguments", "Write operands separated by spaces, with optional -n."),
        detail::echo));
    (void)result->add(detail::function_kernel(
        detail::metadata("printf", "format arguments", "Format %s, %d, %%, \\n, and \\t deterministically."),
        detail::printf_kernel));
    (void)result->add(detail::function_kernel(
        detail::metadata("basename", "strip directory prefix", "Write the final pathname component; optional suffix is removed."),
        detail::basename_kernel));
    (void)result->add(detail::function_kernel(
        detail::metadata("dirname", "strip final component", "Write the directory component of a pathname."),
        detail::dirname_kernel));
    (void)result->add(detail::function_kernel(
        detail::metadata("env", "list exported environment", "Write exported NAME=VALUE entries; command execution is delegated to the shell."),
        detail::env_kernel));
    (void)result->add(detail::function_kernel(
        detail::metadata("printenv", "print environment values", "Write selected environment values or all exported NAME=VALUE entries."),
        detail::printenv_kernel));

    return result;
}

inline std::vector<kernel::Metadata> catalog() { return registry()->list(); }

} // namespace lsh::stdkern
