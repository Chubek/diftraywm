#pragma once

#include "LibShell.hpp"
#include "LibShell-Kernel.hpp"
#include "LibShell-Posix.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace api_examples {

inline lsh::Shell local_shell() {
    return lsh::Shell(std::make_shared<lsh::LocalExecutor>());
}

inline std::shared_ptr<lsh::MemoryWriter> capture_memory() {
    return std::make_shared<lsh::MemoryWriter>();
}

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

inline lsh::ExecutionReport run_or_throw(lsh::Shell& shell, const lsh::ir::Program& program) {
    auto result = shell.run(program);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return result.value();
}

inline lsh::ExecutionReport run_or_throw(lsh::Shell& shell, const lsh::dsl::Expr& expr) {
    return run_or_throw(shell, expr.program());
}

inline std::string capture_stdout(lsh::Shell& shell, const lsh::dsl::Expr& expr) {
    auto memory = capture_memory();
    auto report = run_or_throw(shell, lsh::dsl::redirect(expr, lsh::to_memory(lsh::RedirectStream::stdout_stream, memory)));
    require(report.status.success(), "command failed");
    return memory->bytes();
}

inline std::filesystem::path fresh_dir(std::string_view name) {
    auto path = std::filesystem::temp_directory_path() / std::string(name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

inline void print_line(const std::string& text) {
    std::cout << text << '\n';
}

} // namespace api_examples
