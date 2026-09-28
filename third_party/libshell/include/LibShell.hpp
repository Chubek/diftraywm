#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lsh {

enum class ErrorCode : std::uint16_t {
    ok = 0,
    not_found = 1001,
    permission_denied = 1002,
    invalid_redirection = 1101,
    bad_connective = 1102,
    empty_argv = 1103,
    bad_expansion = 1201,
    execution_failed = 1301,
    io_error = 1302,
    timeout = 1303,
    canceled = 1304,
    resource_limit = 1305,
    invalid_graph = 1401,
};

struct Diagnostic {
    ErrorCode code {ErrorCode::ok};
    std::string message;
    std::string path;

    [[nodiscard]] constexpr bool ok() const noexcept { return code == ErrorCode::ok; }
};

template <typename Value>
class Result {
public:
    Result(Value value) : storage_(std::move(value)) {}
    Result(Diagnostic diagnostic) : storage_(std::move(diagnostic)) {}

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<Value>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] Value& value() & { return std::get<Value>(storage_); }
    [[nodiscard]] const Value& value() const& { return std::get<Value>(storage_); }
    [[nodiscard]] Value&& value() && { return std::get<Value>(std::move(storage_)); }

    [[nodiscard]] Diagnostic& error() & { return std::get<Diagnostic>(storage_); }
    [[nodiscard]] const Diagnostic& error() const& { return std::get<Diagnostic>(storage_); }

private:
    std::variant<Value, Diagnostic> storage_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {}

    [[nodiscard]] bool has_value() const noexcept { return !diagnostic_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const Diagnostic& error() const& { return *diagnostic_; }

private:
    std::optional<Diagnostic> diagnostic_;
};

enum class RedirectStream : std::uint8_t { stdin_stream, stdout_stream, stderr_stream };
enum class RedirectMode : std::uint8_t { read, truncate, append, clobber, duplicate };
enum class StdioTargetKind : std::uint8_t { inherit, null_device, pipe, file, fd, memory, sinklet };
enum class PipefailPolicy : std::uint8_t { last, any_failed, none };
enum class Connective : std::uint8_t { sequence, and_if, or_if, background };
enum class ExpansionKind : std::uint8_t { raw, single_quoted, double_quoted, variable, arithmetic, command, lua, glob };
enum class CommandSource : std::uint8_t { external, builtin, kernel, adapter, auto_resolve };
enum class EnvironmentInheritance : std::uint8_t { copy, exported_only, empty, shared_explicit };

struct Timeout {
    std::chrono::milliseconds duration {0};
};

struct ResourceLimits {
    std::optional<std::uint64_t> cpu_time_seconds;
    std::optional<std::uint64_t> memory_bytes;
    std::optional<std::uint64_t> file_size_bytes;
    std::optional<std::uint32_t> open_files;
    std::optional<std::uint32_t> processes;
};

struct EnvVar {
    std::string key;
    std::string value;
    bool exported {true};
};

class Environment {
public:
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const {
        auto found = vars_.find(std::string(key));
        if (found == vars_.end()) {
            return std::nullopt;
        }
        return found->second.value;
    }

    [[nodiscard]] bool is_exported(std::string_view key) const {
        auto found = vars_.find(std::string(key));
        return found != vars_.end() && found->second.exported;
    }

    void set(std::string key, std::string value, bool exported = true) {
        auto [it, _] = vars_.insert_or_assign(key, EnvVar {.key = key, .value = std::move(value), .exported = exported});
        it->second.key = it->first;
    }

    void unset(std::string_view key) { vars_.erase(std::string(key)); }

    void export_var(std::string_view key) {
        auto found = vars_.find(std::string(key));
        if (found != vars_.end()) {
            found->second.exported = true;
        }
    }

    [[nodiscard]] std::vector<EnvVar> entries() const {
        std::vector<EnvVar> values;
        values.reserve(vars_.size());
        for (const auto& [_, value] : vars_) {
            values.push_back(value);
        }
        return values;
    }

    [[nodiscard]] std::vector<EnvVar> exported_entries() const {
        std::vector<EnvVar> values;
        for (const auto& [_, value] : vars_) {
            if (value.exported) {
                values.push_back(value);
            }
        }
        return values;
    }

private:
    std::map<std::string, EnvVar, std::less<>> vars_;
};

namespace detail {
Result<std::string> eval_qamrpp_lua(std::string_view script, const Environment& environment);
}

class Writer {
public:
    virtual ~Writer() = default;
    virtual Result<void> write(std::string_view bytes) = 0;
    virtual Result<void> flush() { return {}; }
    virtual Result<void> close() { return {}; }
};

class OstreamWriter final : public Writer {
public:
    explicit OstreamWriter(std::ostream& stream) : stream_(&stream) {}

    Result<void> write(std::string_view bytes) override {
        stream_->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!*stream_) {
            return Diagnostic {ErrorCode::io_error, "failed to write to stream", {}};
        }
        return {};
    }

    Result<void> flush() override {
        stream_->flush();
        if (!*stream_) {
            return Diagnostic {ErrorCode::io_error, "failed to flush stream", {}};
        }
        return {};
    }

private:
    std::ostream* stream_;
};

class MemoryWriter final : public Writer {
public:
    Result<void> write(std::string_view bytes) override {
        buffer_.append(bytes);
        return {};
    }

    [[nodiscard]] const std::string& bytes() const noexcept { return buffer_; }
    [[nodiscard]] std::string take() { return std::move(buffer_); }

private:
    std::string buffer_;
};

} // namespace lsh

namespace lsh {

struct SinkletContext {
    std::string source;
    Environment* environment {nullptr};
};

class Sinklet {
public:
    virtual ~Sinklet() = default;
    virtual Result<void> begin(SinkletContext&) { return {}; }
    virtual Result<void> write(std::string_view chunk, SinkletContext&) = 0;
    virtual Result<void> end(SinkletContext&) { return {}; }
};

class LambdaSinklet final : public Sinklet {
public:
    using BeginFn = std::function<Result<void>(SinkletContext&)>;
    using WriteFn = std::function<Result<void>(std::string_view, SinkletContext&)>;
    using EndFn = std::function<Result<void>(SinkletContext&)>;

    explicit LambdaSinklet(WriteFn write, BeginFn begin = {}, EndFn end = {})
        : begin_(std::move(begin)), write_(std::move(write)), end_(std::move(end)) {}

    Result<void> begin(SinkletContext& context) override {
        return begin_ ? begin_(context) : Result<void> {};
    }

    Result<void> write(std::string_view chunk, SinkletContext& context) override {
        if (!write_) {
            return Diagnostic {ErrorCode::invalid_graph, "lambda sinklet has no write handler", context.source};
        }
        return write_(chunk, context);
    }

    Result<void> end(SinkletContext& context) override {
        return end_ ? end_(context) : Result<void> {};
    }

private:
    BeginFn begin_;
    WriteFn write_;
    EndFn end_;
};

// Adapter exposing a Sinklet behind the Writer interface. begin/end run once;
// each write chunk is forwarded to Sinklet::write. Used to route a command's
// stdout/stderr into a stream processor via StdioTargetKind::sinklet.
class SinkletWriter final : public Writer {
public:
    explicit SinkletWriter(std::shared_ptr<Sinklet> sinklet) : sinklet_(std::move(sinklet)) {}

    Result<void> write(std::string_view bytes) override {
        if (!sinklet_) {
            return Diagnostic {ErrorCode::invalid_graph, "sinklet writer has no sinklet", {}};
        }
        if (!begun_) {
            if (auto result = sinklet_->begin(context_); !result) {
                return result.error();
            }
            begun_ = true;
        }
        return sinklet_->write(bytes, context_);
    }

    Result<void> close() override {
        if (sinklet_ && begun_) {
            begun_ = false;
            return sinklet_->end(context_);
        }
        return {};
    }

private:
    std::shared_ptr<Sinklet> sinklet_;
    SinkletContext context_;
    bool begun_ {false};
};

// Sinklet that forwards every chunk verbatim to two downstream writers (tee).
class TeeSinklet final : public Sinklet {
public:
    TeeSinklet(std::shared_ptr<Writer> primary, std::shared_ptr<Writer> secondary)
        : primary_(std::move(primary)), secondary_(std::move(secondary)) {}

    Result<void> write(std::string_view chunk, SinkletContext&) override {
        if (primary_) {
            if (auto r = primary_->write(chunk); !r) {
                return r.error();
            }
        }
        if (secondary_) {
            return secondary_->write(chunk);
        }
        return {};
    }

private:
    std::shared_ptr<Writer> primary_;
    std::shared_ptr<Writer> secondary_;
};

// Line-buffered sinklet base: buffers partial lines, invokes on_line per
// complete newline-terminated line, flushes the remainder on end.
class LineSinklet : public Sinklet {
public:
    explicit LineSinklet(std::shared_ptr<Writer> downstream) : downstream_(std::move(downstream)) {}

    Result<void> write(std::string_view chunk, SinkletContext& context) override {
        partial_.append(chunk);
        for (;;) {
            auto nl = partial_.find('\n');
            if (nl == std::string::npos) {
                break;
            }
            std::string line = partial_.substr(0, nl);
            partial_.erase(0, nl + 1);
            if (auto r = on_line(line, downstream_, context); !r) {
                return r.error();
            }
        }
        return {};
    }

    Result<void> end(SinkletContext& context) override {
        if (!partial_.empty()) {
            if (auto r = on_line(partial_, downstream_, context); !r) {
                return r.error();
            }
            partial_.clear();
        }
        if (downstream_) {
            return downstream_->flush();
        }
        return {};
    }

protected:
    std::shared_ptr<Writer> downstream_;

    virtual Result<void> on_line(std::string_view line, const std::shared_ptr<Writer>& downstream, SinkletContext& context) = 0;

private:
    std::string partial_;
};

// Forwards only lines containing needle.
class GrepSinklet final : public LineSinklet {
public:
    GrepSinklet(std::string needle, std::shared_ptr<Writer> downstream)
        : LineSinklet(std::move(downstream)), needle_(std::move(needle)) {}

protected:
    Result<void> on_line(std::string_view line, const std::shared_ptr<Writer>& downstream, SinkletContext&) override {
        if (line.find(needle_) != std::string_view::npos) {
            std::string out;
            out.append(line.data(), line.size());
            out.push_back('\n');
            if (downstream) {
                return downstream->write(out);
            }
        }
        return {};
    }

private:
    std::string needle_;
};

// Emits each line as {"line": "<escaped>"}\n.
class JsonLinesSinklet final : public LineSinklet {
public:
    explicit JsonLinesSinklet(std::shared_ptr<Writer> downstream) : LineSinklet(std::move(downstream)) {}

protected:
    Result<void> on_line(std::string_view line, const std::shared_ptr<Writer>& downstream, SinkletContext&) override {
        if (!downstream) {
            return {};
        }
        std::string out = "{\"line\": \"";
        for (char ch : line) {
            if (ch == '\\' || ch == '"') {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
        out += "\"}\n";
        return downstream->write(out);
    }
};

struct StdioTarget {
    StdioTargetKind kind {StdioTargetKind::inherit};
    std::optional<std::string> file;
    std::optional<int> fd;
    std::shared_ptr<Writer> memory;
    std::shared_ptr<Sinklet> sinklet;
};

struct Redirection {
    RedirectStream stream {RedirectStream::stdout_stream};
    RedirectMode mode {RedirectMode::truncate};
    StdioTarget target;
};

struct Expansion {
    ExpansionKind kind {ExpansionKind::raw};
    std::string text;
    bool field_splitting {false};
};

struct Argument {
    std::vector<Expansion> fragments;

    static Argument raw(std::string value) {
        return Argument {{Expansion {.kind = ExpansionKind::raw, .text = std::move(value)}}};
    }
};

namespace ir {

struct Node;
using NodePtr = std::shared_ptr<Node>;

struct Command {
    std::vector<Argument> argv;
    std::vector<EnvVar> environment_overlay;
    std::optional<std::string> cwd;
    std::vector<Redirection> redirections;
    CommandSource source {CommandSource::auto_resolve};
};

struct Pipeline {
    std::vector<Command> commands;
    PipefailPolicy pipefail {PipefailPolicy::last};
    bool merge_stderr {false};
};

struct Sequence {
    NodePtr left;
    NodePtr right;
    Connective connective {Connective::sequence};
};

struct Subshell {
    NodePtr body;
    EnvironmentInheritance inheritance {EnvironmentInheritance::copy};
    std::vector<Redirection> redirections;
};

struct Redirected {
    NodePtr subject;
    std::vector<Redirection> redirections;
};

using NodeValue = std::variant<Command, Pipeline, Sequence, Subshell, Redirected>;

struct Node {
    NodeValue value;
    std::string debug_name;
};

struct Program {
    NodePtr root;
};

inline NodePtr node(NodeValue value, std::string debug_name = {}) {
    return std::make_shared<Node>(Node {std::move(value), std::move(debug_name)});
}

inline NodePtr command(Command command_node, std::string debug_name = {}) {
    return node(std::move(command_node), std::move(debug_name));
}

inline Command make_command(std::initializer_list<std::string_view> argv) {
    Command command_node;
    command_node.argv.reserve(argv.size());
    for (std::string_view item : argv) {
        command_node.argv.push_back(Argument::raw(std::string(item)));
    }
    return command_node;
}

struct ValidationReport {
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
};

namespace detail {

inline void append_path(std::string& path, std::string_view segment) {
    if (!path.empty()) {
        path.push_back('.');
    }
    path.append(segment);
}

inline void diagnose(ValidationReport& report, ErrorCode code, std::string message, const std::string& path) {
    report.diagnostics.push_back(Diagnostic {code, std::move(message), path});
}

inline bool argument_is_empty(const Argument& argument) {
    return argument.fragments.empty()
        || std::all_of(argument.fragments.begin(), argument.fragments.end(), [](const Expansion& expansion) {
               return expansion.kind == ExpansionKind::raw && expansion.text.empty();
           });
}

inline bool has_nul_byte(std::string_view text) { return text.find('\0') != std::string_view::npos; }

inline bool expansion_kind_valid(ExpansionKind kind) {
    switch (kind) {
    case ExpansionKind::raw:
    case ExpansionKind::single_quoted:
    case ExpansionKind::double_quoted:
    case ExpansionKind::variable:
    case ExpansionKind::arithmetic:
    case ExpansionKind::command:
    case ExpansionKind::lua:
    case ExpansionKind::glob:
        return true;
    }
    return false;
}

inline void validate_argument(const Argument& argument, ValidationReport& report, const std::string& path) {
    for (std::size_t index = 0; index < argument.fragments.size(); ++index) {
        const Expansion& fragment = argument.fragments[index];
        std::string fragment_path = path;
        append_path(fragment_path, "frag" + std::to_string(index));
        if (!expansion_kind_valid(fragment.kind)) {
            diagnose(report, ErrorCode::bad_expansion, "argument contains an unknown expansion kind", fragment_path);
        }
        if (has_nul_byte(fragment.text)) {
            diagnose(report, ErrorCode::bad_expansion, "argument contains NUL byte", fragment_path);
        }
    }
}

inline void validate_redirection(const Redirection& redirection, ValidationReport& report, const std::string& path) {
    if (redirection.mode == RedirectMode::read && redirection.stream != RedirectStream::stdin_stream) {
        diagnose(report, ErrorCode::invalid_redirection, "read redirection is only valid for stdin", path);
    }

    switch (redirection.target.kind) {
    case StdioTargetKind::file:
        if (!redirection.target.file || redirection.target.file->empty()) {
            diagnose(report, ErrorCode::invalid_redirection, "file redirection requires a non-empty path", path);
        }
        break;
    case StdioTargetKind::fd:
        if (!redirection.target.fd || *redirection.target.fd < 0) {
            diagnose(report, ErrorCode::invalid_redirection, "fd redirection requires a non-negative descriptor", path);
        }
        break;
    case StdioTargetKind::memory:
        if (!redirection.target.memory) {
            diagnose(report, ErrorCode::invalid_redirection, "memory redirection requires a writer", path);
        }
        break;
    case StdioTargetKind::sinklet:
        if (!redirection.target.sinklet) {
            diagnose(report, ErrorCode::invalid_redirection, "sinklet redirection requires a sinklet", path);
        }
        break;
    case StdioTargetKind::inherit:
    case StdioTargetKind::null_device:
    case StdioTargetKind::pipe:
        break;
    }

    const bool writes_file = redirection.mode == RedirectMode::truncate
        || redirection.mode == RedirectMode::append
        || redirection.mode == RedirectMode::clobber;
    if (writes_file && redirection.target.kind == StdioTargetKind::fd && !redirection.target.fd) {
        diagnose(report, ErrorCode::invalid_redirection, "output fd redirection requires a descriptor", path);
    }
}

inline void validate_command(const Command& command_node, ValidationReport& report, const std::string& path) {
    if (command_node.argv.empty() || argument_is_empty(command_node.argv.front())) {
        diagnose(report, ErrorCode::empty_argv, "command argv must contain an executable argument", path);
    }

    for (std::size_t index = 0; index < command_node.argv.size(); ++index) {
        std::string arg_path = path;
        append_path(arg_path, "argv" + std::to_string(index));
        validate_argument(command_node.argv[index], report, arg_path);
    }

    for (std::size_t index = 0; index < command_node.redirections.size(); ++index) {
        std::string child_path = path;
        append_path(child_path, "redir" + std::to_string(index));
        validate_redirection(command_node.redirections[index], report, child_path);
    }
}

inline void validate_node(const NodePtr& node_ptr, ValidationReport& report, const std::string& path) {
    if (!node_ptr) {
        diagnose(report, ErrorCode::invalid_graph, "IR node pointer is null", path);
        return;
    }

    std::visit([&](const auto& node_value) {
        using T = std::decay_t<decltype(node_value)>;
        if constexpr (std::is_same_v<T, Command>) {
            validate_command(node_value, report, path);
        } else if constexpr (std::is_same_v<T, Pipeline>) {
            if (node_value.commands.empty()) {
                diagnose(report, ErrorCode::invalid_graph, "pipeline must contain at least one command", path);
            }
            for (std::size_t index = 0; index < node_value.commands.size(); ++index) {
                std::string child_path = path;
                append_path(child_path, "cmd" + std::to_string(index));
                validate_command(node_value.commands[index], report, child_path);
            }
        } else if constexpr (std::is_same_v<T, Sequence>) {
            if (!node_value.left || !node_value.right) {
                diagnose(report, ErrorCode::bad_connective, "sequence connective requires left and right nodes", path);
            }
            validate_node(node_value.left, report, path + ".left");
            validate_node(node_value.right, report, path + ".right");
        } else if constexpr (std::is_same_v<T, Subshell>) {
            if (!node_value.body) {
                diagnose(report, ErrorCode::invalid_graph, "subshell requires a body", path);
            }
            validate_node(node_value.body, report, path + ".body");
            for (std::size_t index = 0; index < node_value.redirections.size(); ++index) {
                validate_redirection(node_value.redirections[index], report, path + ".redir" + std::to_string(index));
            }
        } else if constexpr (std::is_same_v<T, Redirected>) {
            if (!node_value.subject) {
                diagnose(report, ErrorCode::invalid_graph, "redirected node requires a subject", path);
            }
            validate_node(node_value.subject, report, path + ".subject");
            for (std::size_t index = 0; index < node_value.redirections.size(); ++index) {
                validate_redirection(node_value.redirections[index], report, path + ".redir" + std::to_string(index));
            }
        }
    }, node_ptr->value);
}

} // namespace detail

inline ValidationReport validate(const Program& program) {
    ValidationReport report;
    detail::validate_node(program.root, report, "program");
    return report;
}

} // namespace ir

inline Argument literal(std::string value) { return Argument::raw(std::move(value)); }

inline Argument single_quoted(std::string value) {
    return Argument {{Expansion {.kind = ExpansionKind::single_quoted, .text = std::move(value)}}};
}

inline Argument double_quoted(std::string value) {
    return Argument {{Expansion {.kind = ExpansionKind::double_quoted, .text = std::move(value)}}};
}

inline Argument variable(std::string name, bool field_splitting = false) {
    return Argument {{Expansion {.kind = ExpansionKind::variable, .text = std::move(name), .field_splitting = field_splitting}}};
}

inline Argument arithmetic(std::string expression) {
    return Argument {{Expansion {.kind = ExpansionKind::arithmetic, .text = std::move(expression)}}};
}

inline Argument command_substitution(std::string script, bool field_splitting = true) {
    return Argument {{Expansion {.kind = ExpansionKind::command, .text = std::move(script), .field_splitting = field_splitting}}};
}

inline Argument lua(std::string script) {
    return Argument {{Expansion {.kind = ExpansionKind::lua, .text = std::move(script)}}};
}

inline Argument glob(std::string pattern) {
    return Argument {{Expansion {.kind = ExpansionKind::glob, .text = std::move(pattern)}}};
}

namespace detail {

inline Argument to_argument(Argument argument) { return argument; }
inline Argument to_argument(std::string value) { return literal(std::move(value)); }
inline Argument to_argument(std::string_view value) { return literal(std::string(value)); }
inline Argument to_argument(const char* value) { return literal(value == nullptr ? std::string {} : std::string(value)); }

inline StdioTarget file_target(std::string path) {
    StdioTarget target;
    target.kind = StdioTargetKind::file;
    target.file = std::move(path);
    return target;
}

inline Redirection make_file_redirection(RedirectStream stream, RedirectMode mode, std::string path) {
    return Redirection {.stream = stream, .mode = mode, .target = file_target(std::move(path))};
}

} // namespace detail

inline Redirection in(std::string path) {
    return detail::make_file_redirection(RedirectStream::stdin_stream, RedirectMode::read, std::move(path));
}

inline Redirection out(std::string path) {
    return detail::make_file_redirection(RedirectStream::stdout_stream, RedirectMode::truncate, std::move(path));
}

inline Redirection append(std::string path) {
    return detail::make_file_redirection(RedirectStream::stdout_stream, RedirectMode::append, std::move(path));
}

inline Redirection err(std::string path) {
    return detail::make_file_redirection(RedirectStream::stderr_stream, RedirectMode::truncate, std::move(path));
}

inline Redirection err_append(std::string path) {
    return detail::make_file_redirection(RedirectStream::stderr_stream, RedirectMode::append, std::move(path));
}

inline Redirection to_fd(RedirectStream stream, int fd) {
    StdioTarget target;
    target.kind = StdioTargetKind::fd;
    target.fd = fd;
    return Redirection {.stream = stream, .mode = RedirectMode::duplicate, .target = std::move(target)};
}

inline Redirection to_null(RedirectStream stream) {
    StdioTarget target;
    target.kind = StdioTargetKind::null_device;
    return Redirection {.stream = stream, .mode = RedirectMode::truncate, .target = std::move(target)};
}

inline Redirection to_memory(RedirectStream stream, std::shared_ptr<Writer> memory) {
    StdioTarget target;
    target.kind = StdioTargetKind::memory;
    target.memory = std::move(memory);
    return Redirection {.stream = stream, .mode = RedirectMode::truncate, .target = std::move(target)};
}

inline Redirection to_sinklet(RedirectStream stream, std::shared_ptr<Sinklet> sinklet) {
    StdioTarget target;
    target.kind = StdioTargetKind::sinklet;
    target.sinklet = std::move(sinklet);
    return Redirection {.stream = stream, .mode = RedirectMode::truncate, .target = std::move(target)};
}

namespace dsl {

class Expr {
public:
    Expr() = default;
    Expr(ir::NodePtr node) : node_(std::move(node)) {}
    Expr(ir::Command command) : node_(ir::command(std::move(command))) {}

    [[nodiscard]] const ir::NodePtr& node() const noexcept { return node_; }
    [[nodiscard]] ir::Program program() const { return ir::Program {node_}; }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(node_); }

private:
    ir::NodePtr node_;
};

template <typename... Args>
[[nodiscard]] Expr cmd(std::string executable, Args&&... args) {
    ir::Command command;
    command.argv.reserve(sizeof...(Args) + 1);
    command.argv.push_back(literal(std::move(executable)));
    (command.argv.push_back(detail::to_argument(std::forward<Args>(args))), ...);
    return Expr {std::move(command)};
}

[[nodiscard]] inline Expr builtin(std::string name) {
    ir::Command command;
    command.source = CommandSource::builtin;
    command.argv.push_back(literal(std::move(name)));
    return Expr {std::move(command)};
}

[[nodiscard]] inline Expr kernel(std::string name) {
    ir::Command command;
    command.source = CommandSource::kernel;
    command.argv.push_back(literal(std::move(name)));
    return Expr {std::move(command)};
}

namespace detail {

inline bool append_pipeline_commands(const ir::NodePtr& node, std::vector<ir::Command>& commands) {
    if (!node) {
        return false;
    }
    if (const auto* command = std::get_if<ir::Command>(&node->value)) {
        commands.push_back(*command);
        return true;
    }
    if (const auto* pipeline = std::get_if<ir::Pipeline>(&node->value)) {
        commands.insert(commands.end(), pipeline->commands.begin(), pipeline->commands.end());
        return true;
    }
    return false;
}

} // namespace detail

[[nodiscard]] inline Expr pipe(Expr left, Expr right, PipefailPolicy pipefail = PipefailPolicy::last, bool merge_stderr = false) {
    ir::Pipeline pipeline;
    pipeline.pipefail = pipefail;
    pipeline.merge_stderr = merge_stderr;
    const bool ok = detail::append_pipeline_commands(left.node(), pipeline.commands)
        && detail::append_pipeline_commands(right.node(), pipeline.commands);
    return ok ? Expr {ir::node(std::move(pipeline), "pipeline")} : Expr {ir::node(ir::Pipeline {}, "invalid-pipeline")};
}

[[nodiscard]] inline Expr operator|(Expr left, Expr right) { return pipe(std::move(left), std::move(right)); }

[[nodiscard]] inline Expr sequence(Expr left, Expr right, Connective connective = Connective::sequence) {
    return Expr {ir::node(ir::Sequence {.left = left.node(), .right = right.node(), .connective = connective}, "sequence")};
}

[[nodiscard]] inline Expr operator&&(Expr left, Expr right) { return sequence(std::move(left), std::move(right), Connective::and_if); }
[[nodiscard]] inline Expr operator||(Expr left, Expr right) { return sequence(std::move(left), std::move(right), Connective::or_if); }

[[nodiscard]] inline Expr then(Expr left, Expr right) { return sequence(std::move(left), std::move(right), Connective::sequence); }

[[nodiscard]] inline Expr subshell(Expr body, EnvironmentInheritance inheritance = EnvironmentInheritance::copy) {
    ir::Subshell subshell_node;
    subshell_node.body = body.node();
    subshell_node.inheritance = inheritance;
    return Expr {ir::node(std::move(subshell_node), "subshell")};
}

[[nodiscard]] inline Expr redirect(Expr subject, Redirection redirection) {
    if (subject.node()) {
        if (auto* command = std::get_if<ir::Command>(&subject.node()->value)) {
            auto copy = *command;
            copy.redirections.push_back(std::move(redirection));
            return Expr {ir::command(std::move(copy), subject.node()->debug_name)};
        }
    }
    return Expr {ir::node(ir::Redirected {.subject = subject.node(), .redirections = {std::move(redirection)}}, "redirect")};
}

[[nodiscard]] inline Expr redirect(Expr subject, std::vector<Redirection> redirections) {
    if (subject.node()) {
        if (auto* command = std::get_if<ir::Command>(&subject.node()->value)) {
            auto copy = *command;
            copy.redirections.insert(copy.redirections.end(), redirections.begin(), redirections.end());
            return Expr {ir::command(std::move(copy), subject.node()->debug_name)};
        }
    }
    return Expr {ir::node(ir::Redirected {.subject = subject.node(), .redirections = std::move(redirections)}, "redirect")};
}

} // namespace dsl

struct ExitStatus {
    int code {0};
    bool signaled {false};
    std::optional<int> signal;
    bool timed_out {false};
    bool canceled {false};

    [[nodiscard]] bool success() const noexcept {
        return code == 0 && !signaled && !timed_out && !canceled;
    }
};

// Kernel contracts depend only on the base runtime types (Environment, Writer,
// Result, ExitStatus) defined above. Included at global scope — once ExitStatus
// is complete — so the Shell can coordinate kernel resolution without leaking
// the layering boundary. The canonical contract surface remains in
// LibShell-Kernel.hpp.
} // namespace lsh
#include "LibShell-Kernel.hpp"
namespace lsh {

struct ExecutionReport {
    ExitStatus status;
    std::optional<int> pid;
    std::vector<ExitStatus> pipeline_statuses;
    std::optional<std::uint64_t> bytes_stdout;
    std::optional<std::uint64_t> bytes_stderr;
    std::vector<Diagnostic> diagnostics;
};

struct ExecSpec {
    std::vector<std::string> argv;
    std::vector<EnvVar> environment;
    std::optional<std::string> cwd;
    std::vector<Redirection> redirections;
    std::optional<Timeout> timeout;
    std::optional<ResourceLimits> limits;
    CommandSource source {CommandSource::auto_resolve};
    bool trace {false};
    bool sandboxed {false};
    std::shared_ptr<kernel::Registry> kernels;
    std::shared_ptr<kernel::Kernel> resolved_kernel;
};

class ScriptingBackend {
public:
    virtual ~ScriptingBackend() = default;
    virtual Result<std::string> eval_command(std::string_view script, const Environment& environment) = 0;
    virtual Result<std::string> eval_lua(std::string_view script, const Environment& environment) = 0;
};

namespace detail {

inline Result<long long> eval_arithmetic(std::string_view expr) {
    std::string_view s = expr;
    auto skip_ws = [&] {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
    };

    std::function<Result<long long>()> parse_expr, parse_term, parse_factor;

    parse_expr = [&]() -> Result<long long> {
        auto lhs = parse_term();
        if (!lhs) {
            return lhs;
        }
        skip_ws();
        while (!s.empty() && (s.front() == '+' || s.front() == '-')) {
            char op = s.front();
            s.remove_prefix(1);
            auto rhs = parse_term();
            if (!rhs) {
                return rhs;
            }
            lhs.value() = op == '+' ? lhs.value() + rhs.value() : lhs.value() - rhs.value();
            skip_ws();
        }
        return lhs;
    };

    parse_term = [&]() -> Result<long long> {
        auto lhs = parse_factor();
        if (!lhs) {
            return lhs;
        }
        skip_ws();
        while (!s.empty() && (s.front() == '*' || s.front() == '/' || s.front() == '%')) {
            char op = s.front();
            s.remove_prefix(1);
            auto rhs = parse_factor();
            if (!rhs) {
                return rhs;
            }
            if (op == '*') {
                lhs.value() = lhs.value() * rhs.value();
            } else if (rhs.value() == 0) {
                return Diagnostic {ErrorCode::bad_expansion, "division by zero in arithmetic expansion", std::string(expr)};
            } else {
                lhs.value() = op == '/' ? lhs.value() / rhs.value() : lhs.value() % rhs.value();
            }
            skip_ws();
        }
        return lhs;
    };

    parse_factor = [&]() -> Result<long long> {
        skip_ws();
        if (s.empty()) {
            return Diagnostic {ErrorCode::bad_expansion, "unexpected end of arithmetic expression", std::string(expr)};
        }
        if (s.front() == '(') {
            s.remove_prefix(1);
            auto value = parse_expr();
            if (!value) {
                return value;
            }
            skip_ws();
            if (s.empty() || s.front() != ')') {
                return Diagnostic {ErrorCode::bad_expansion, "missing closing parenthesis in arithmetic expression", std::string(expr)};
            }
            s.remove_prefix(1);
            return value;
        }
        if (s.front() == '-') {
            s.remove_prefix(1);
            auto value = parse_factor();
            if (!value) {
                return value;
            }
            return -value.value();
        }
        if (s.front() == '+') {
            s.remove_prefix(1);
            return parse_factor();
        }
        if (std::isdigit(static_cast<unsigned char>(s.front()))) {
            long long value = 0;
            while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.front()))) {
                value = value * 10 + (s.front() - '0');
                s.remove_prefix(1);
            }
            return value;
        }
        return Diagnostic {ErrorCode::bad_expansion, "invalid token in arithmetic expression", std::string(expr)};
    };

    auto result = parse_expr();
    if (!result) {
        return result;
    }
    skip_ws();
    if (!s.empty()) {
        return Diagnostic {ErrorCode::bad_expansion, "trailing characters in arithmetic expression", std::string(expr)};
    }
    return result;
}

inline bool has_glob_meta(std::string_view text) {
    return text.find_first_of("*?[") != std::string_view::npos;
}

inline std::vector<std::string> expand_glob(std::string_view pattern) {
    namespace fs = std::filesystem;
    fs::path path_pattern(pattern);
    fs::path parent = path_pattern.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    std::string leaf = path_pattern.filename().empty() ? std::string(pattern) : path_pattern.filename().string();

    std::vector<std::string> matches;
    std::error_code ec;
    if (!fs::exists(parent, ec)) {
        return {std::string(pattern)};
    }

    auto matches_segment = [&](std::string_view name, std::string_view pat) {
        // Minimal glob: '*', '?', and literal characters. No bracket ranges.
        std::size_t n = name.size(), m = pat.size();
        std::vector<std::vector<bool>> dp(n + 1, std::vector<bool>(m + 1, false));
        dp[0][0] = true;
        for (std::size_t j = 1; j <= m && pat[j - 1] == '*'; ++j) {
            dp[0][j] = true;
        }
        for (std::size_t i = 1; i <= n; ++i) {
            for (std::size_t j = 1; j <= m; ++j) {
                if (pat[j - 1] == '*') {
                    dp[i][j] = dp[i - 1][j] || dp[i][j - 1];
                } else if (pat[j - 1] == '?' || pat[j - 1] == name[i - 1]) {
                    dp[i][j] = dp[i - 1][j - 1];
                }
            }
        }
        return dp[n][m];
    };

    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        std::string name = entry.path().filename().string();
        if (name.empty() || (name.front() == '.' && leaf.front() != '.')) {
            continue;
        }
        if (matches_segment(name, leaf)) {
            fs::path full = path_pattern.has_parent_path() ? (parent / name) : fs::path(name);
            matches.push_back(full.string());
        }
    }

    if (matches.empty()) {
        return {std::string(pattern)};
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

inline ExitStatus select_pipeline_status(const std::vector<ExitStatus>& statuses, PipefailPolicy policy) {
    if (statuses.empty() || policy == PipefailPolicy::none) {
        return ExitStatus {};
    }
    if (policy == PipefailPolicy::last) {
        return statuses.back();
    }
    auto failed = std::find_if(statuses.begin(), statuses.end(), [](const ExitStatus& status) {
        return !status.success();
    });
    return failed == statuses.end() ? ExitStatus {} : *failed;
}

} // namespace detail

class Expander {
public:
    Expander() = default;
    explicit Expander(std::shared_ptr<ScriptingBackend> scripting) : scripting_(std::move(scripting)) {}

    void set_scripting(std::shared_ptr<ScriptingBackend> scripting) { scripting_ = std::move(scripting); }

    virtual Result<std::string> expand_fragment(const Expansion& expansion, const Environment& environment) const {
        switch (expansion.kind) {
        case ExpansionKind::raw:
        case ExpansionKind::single_quoted:
        case ExpansionKind::double_quoted:
            return expansion.text;
        case ExpansionKind::variable:
            if (auto value = environment.get(expansion.text)) {
                return *value;
            }
            return std::string {};
        case ExpansionKind::arithmetic: {
            auto value = detail::eval_arithmetic(expansion.text);
            if (!value) {
                return value.error();
            }
            return std::to_string(value.value());
        }
        case ExpansionKind::command:
            if (scripting_) {
                return scripting_->eval_command(expansion.text, environment);
            }
            return Diagnostic {ErrorCode::bad_expansion, "command substitution backend is unavailable", expansion.text};
        case ExpansionKind::lua:
            if (scripting_) {
                return scripting_->eval_lua(expansion.text, environment);
            }
            return Diagnostic {ErrorCode::bad_expansion, "Lua backend is unavailable", expansion.text};
        case ExpansionKind::glob:
            return expansion.text;
        }
        return Diagnostic {ErrorCode::bad_expansion, "unknown expansion kind", expansion.text};
    }

    virtual Result<std::string> expand_argument(const Argument& argument, const Environment& environment) const {
        std::string output;
        for (const Expansion& fragment : argument.fragments) {
            auto expanded = expand_fragment(fragment, environment);
            if (!expanded) {
                return expanded.error();
            }
            output += expanded.value();
        }
        return output;
    }

    virtual Result<std::vector<std::string>> expand_argv(const std::vector<Argument>& arguments, const Environment& environment) const {
        std::vector<std::string> argv;
        argv.reserve(arguments.size());
        for (const Argument& argument : arguments) {
            bool globbable = argument.fragments.size() == 1
                && argument.fragments.front().kind == ExpansionKind::glob
                && detail::has_glob_meta(argument.fragments.front().text);
            if (globbable) {
                for (auto& match : detail::expand_glob(argument.fragments.front().text)) {
                    argv.push_back(std::move(match));
                }
                continue;
            }
            auto expanded = expand_argument(argument, environment);
            if (!expanded) {
                return expanded.error();
            }
            if (argument.fragments.size() == 1 && argument.fragments.front().field_splitting) {
                split_fields(std::move(expanded).value(), argv);
            } else {
                argv.push_back(std::move(expanded).value());
            }
        }
        return argv;
    }

private:
    static void split_fields(std::string text, std::vector<std::string>& out) {
        std::string current;
        auto flush = [&] {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        };
        for (char ch : text) {
            if (ch == ' ' || ch == '\t' || ch == '\n') {
                flush();
            } else {
                current.push_back(ch);
            }
        }
        flush();
    }

    std::shared_ptr<ScriptingBackend> scripting_;
};

class Executor {
public:
    virtual ~Executor() = default;
    virtual Result<ExecutionReport> run(const ExecSpec& spec) = 0;

    // Bind shell-owned runtime state (environment, working directory) by
    // reference so builtins and kernels that mutate shell state (cd/export) or
    // require a live environment (kernel initialize) can observe it. Default is
    // a no-op; concrete executors override. The executor observes, never owns.
    virtual void bind_runtime(Environment* /*environment*/, std::filesystem::path* /*cwd*/) {}

    virtual Result<ExecutionReport> run_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail, bool merge_stderr) {
        // Default implementation runs each command in isolation. Concrete
        // executors that support real pipe wiring override this.
        ExecutionReport report;
        report.pipeline_statuses.reserve(specs.size());
        for (auto& spec : specs) {
            auto sub = run(spec);
            if (!sub) {
                return sub.error();
            }
            report.pipeline_statuses.push_back(sub.value().status);
        }
        report.status = detail::select_pipeline_status(report.pipeline_statuses, pipefail);
        (void)merge_stderr;
        return report;
    }
};

class DryRunExecutor final : public Executor {
public:
    [[nodiscard]] const std::vector<ExecSpec>& recorded_specs() const noexcept { return specs_; }
    void clear() { specs_.clear(); }

    Result<ExecutionReport> run(const ExecSpec& spec) override {
        specs_.push_back(spec);
        ExecutionReport report;
        report.status.code = 0;
        return report;
    }

    Result<ExecutionReport> run_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail, bool /*merge_stderr*/) override {
        ExecutionReport report;
        report.pipeline_statuses.reserve(specs.size());
        for (auto& spec : specs) {
            specs_.push_back(spec);
            report.pipeline_statuses.push_back(ExitStatus {});
        }
        report.status = detail::select_pipeline_status(report.pipeline_statuses, pipefail);
        return report;
    }

private:
    std::vector<ExecSpec> specs_;
};

class Shell;

// Concrete ScriptingBackend wired to a Shell. Command substitution re-enters
// the runtime. Lua evaluation uses a fresh QaMRpp context per expansion:
// host environment is exposed through env(NAME), and identifier-safe variable
// names are also available as read-only Lua globals. QaMRpp standard libraries
// are intentionally not loaded; inline expansion does not acquire filesystem
// or process capabilities by default.
class ShellScriptingBackend final : public ScriptingBackend {
public:
    using ParseHook = std::function<Result<ir::Program>(std::string_view)>;

    explicit ShellScriptingBackend(Shell* shell) : shell_(shell) {}

    void set_parse_hook(ParseHook hook) { parse_hook_ = std::move(hook); }

    Result<std::string> eval_command(std::string_view script, const Environment& environment) override;
    Result<std::string> eval_lua(std::string_view script, const Environment& environment) override;

private:
    Shell* shell_;
    ParseHook parse_hook_;
};

struct ShellOptions {
    PipefailPolicy pipefail {PipefailPolicy::last};
    bool trace {false};
    bool sandboxed {false};
    std::optional<Timeout> timeout;
    std::optional<ResourceLimits> limits;
};

class Shell {
public:
    Shell() : executor_(std::make_shared<DryRunExecutor>()), expander_(std::make_shared<Expander>()) {
        install_runtime();
    }

    explicit Shell(std::shared_ptr<Executor> executor, std::shared_ptr<Expander> expander = std::make_shared<Expander>())
        : executor_(std::move(executor)), expander_(std::move(expander)) {
        install_runtime();
    }

    [[nodiscard]] Environment& env() noexcept { return environment_; }
    [[nodiscard]] const Environment& env() const noexcept { return environment_; }

    [[nodiscard]] const std::filesystem::path& cwd() const noexcept { return cwd_; }
    void set_cwd(std::filesystem::path cwd) { cwd_ = std::move(cwd); }

    [[nodiscard]] ShellOptions& options() noexcept { return options_; }
    [[nodiscard]] const ShellOptions& options() const noexcept { return options_; }

    [[nodiscard]] std::shared_ptr<kernel::Registry> kernels() const noexcept { return kernels_; }
    void set_kernels(std::shared_ptr<kernel::Registry> kernels) { kernels_ = std::move(kernels); }

    // Wire a full grammar parser into command substitution ($(…)). Without it,
    // substitution bodies are whitespace-split into a single command. The hook
    // is invoked when an ExpansionKind::command fragment is expanded.
    void set_command_substitution_parser(ShellScriptingBackend::ParseHook hook) {
        if (scripting_) {
            scripting_->set_parse_hook(std::move(hook));
        }
    }

    Result<ExecutionReport> run(const ir::Program& program) {
        auto validation = ir::validate(program);
        if (!validation.ok()) {
            ExecutionReport report;
            report.status.code = 1;
            report.diagnostics = std::move(validation.diagnostics);
            return report;
        }
        return run_node(program.root);
    }

    Result<ExecutionReport> run(const ir::NodePtr& node) { return run(ir::Program {node}); }

private:
    // Wire command-substitution and shell-state binding into the executor. The
    // scripting backend borrows this Shell; it is owned here and dies with it.
    void install_runtime() {
        scripting_ = std::make_shared<ShellScriptingBackend>(this);
        expander_->set_scripting(scripting_);
        executor_->bind_runtime(&environment_, &cwd_);
    }

    Result<ExecutionReport> run_node(const ir::NodePtr& node) {
        return std::visit([&](const auto& node_value) -> Result<ExecutionReport> {
            using T = std::decay_t<decltype(node_value)>;
            if constexpr (std::is_same_v<T, ir::Command>) {
                return run_command(node_value, {});
            } else if constexpr (std::is_same_v<T, ir::Pipeline>) {
                return run_pipeline(node_value);
            } else if constexpr (std::is_same_v<T, ir::Sequence>) {
                return run_sequence(node_value);
            } else if constexpr (std::is_same_v<T, ir::Subshell>) {
                return run_subshell(node_value);
            } else if constexpr (std::is_same_v<T, ir::Redirected>) {
                return run_redirected(node_value);
            }
        }, node->value);
    }

    Result<ExecutionReport> run_command(const ir::Command& command, std::vector<Redirection> inherited_redirections) {
        auto spec = make_exec_spec(command, std::move(inherited_redirections));
        if (!spec) {
            return spec.error();
        }
        return executor_->run(std::move(spec).value());
    }

    Result<ExecSpec> make_exec_spec(const ir::Command& command, std::vector<Redirection> inherited_redirections) {
        auto argv = expander_->expand_argv(command.argv, environment_);
        if (!argv) {
            return argv.error();
        }

        std::vector<EnvVar> environment = environment_.exported_entries();
        environment.insert(environment.end(), command.environment_overlay.begin(), command.environment_overlay.end());

        inherited_redirections.insert(inherited_redirections.end(), command.redirections.begin(), command.redirections.end());
        ExecSpec spec;
        spec.argv = std::move(argv).value();
        spec.environment = std::move(environment);
        spec.cwd = command.cwd.has_value() ? command.cwd : std::optional<std::string> {cwd_.string()};
        spec.redirections = std::move(inherited_redirections);
        spec.trace = options_.trace;
        spec.sandboxed = options_.sandboxed;
        spec.timeout = options_.timeout;
        spec.limits = options_.limits;
        spec.kernels = kernels_;
        spec.source = command.source;

        // Resolve auto/kernel sources against the kernel registry so executors
        // receive a bound kernel instance instead of an unhandled source tag.
        if (!spec.argv.empty() && kernels_
            && (command.source == CommandSource::auto_resolve || command.source == CommandSource::kernel)) {
            if (auto resolved = kernels_->find(spec.argv.front())) {
                spec.source = CommandSource::kernel;
                spec.resolved_kernel = resolved;
            } else if (command.source == CommandSource::kernel) {
                return Diagnostic {ErrorCode::not_found, "kernel not registered", spec.argv.front()};
            }
        }
        return spec;
    }

    Result<ExecutionReport> run_pipeline(const ir::Pipeline& pipeline) {
        std::vector<ExecSpec> specs;
        specs.reserve(pipeline.commands.size());
        for (const ir::Command& command : pipeline.commands) {
            auto spec = make_exec_spec(command, {});
            if (!spec) {
                return spec.error();
            }
            specs.push_back(std::move(spec).value());
        }
        return executor_->run_pipeline(std::move(specs), pipeline.pipefail, pipeline.merge_stderr);
    }

    Result<ExecutionReport> run_sequence(const ir::Sequence& sequence) {
        auto left = run_node(sequence.left);
        if (!left) {
            return left.error();
        }

        const bool should_run_right = sequence.connective == Connective::sequence
            || sequence.connective == Connective::background
            || (sequence.connective == Connective::and_if && left.value().status.success())
            || (sequence.connective == Connective::or_if && !left.value().status.success());

        if (!should_run_right) {
            return left;
        }
        return run_node(sequence.right);
    }

    // Subshells isolate shell state per their inheritance policy. copy /
    // exported_only / empty run the body against a derived child environment
    // whose mutations are discarded on exit; shared_explicit runs against the
    // parent environment so mutations persist. Redirections apply to the body.
    Result<ExecutionReport> run_subshell(const ir::Subshell& subshell) {
        if (subshell.inheritance == EnvironmentInheritance::shared_explicit) {
            return run_with_redirections(subshell.body, subshell.redirections);
        }

        Environment saved = std::move(environment_);
        environment_ = derive_environment(subshell.inheritance);
        auto result = run_with_redirections(subshell.body, subshell.redirections);
        environment_ = std::move(saved);
        return result;
    }

    Environment derive_environment(EnvironmentInheritance policy) {
        Environment child;
        switch (policy) {
        case EnvironmentInheritance::copy:
        case EnvironmentInheritance::shared_explicit:
            child = environment_;
            break;
        case EnvironmentInheritance::exported_only:
            for (const EnvVar& entry : environment_.exported_entries()) {
                child.set(entry.key, entry.value, entry.exported);
            }
            break;
        case EnvironmentInheritance::empty:
            break;
        }
        return child;
    }

    Result<ExecutionReport> run_with_redirections(const ir::NodePtr& body, const std::vector<Redirection>& redirections) {
        if (redirections.empty()) {
            return run_node(body);
        }
        ir::Redirected redirected;
        redirected.subject = body;
        redirected.redirections = redirections;
        return run_redirected(redirected);
    }

    Result<ExecutionReport> run_redirected(const ir::Redirected& redirected) {
        if (auto* command = std::get_if<ir::Command>(&redirected.subject->value)) {
            return run_command(*command, redirected.redirections);
        }
        if (auto* pipeline = std::get_if<ir::Pipeline>(&redirected.subject->value)) {
            auto redirected_pipeline = *pipeline;
            if (!redirected_pipeline.commands.empty()) {
                auto& last = redirected_pipeline.commands.back();
                last.redirections.insert(last.redirections.end(), redirected.redirections.begin(), redirected.redirections.end());
            }
            return run_pipeline(redirected_pipeline);
        }
        if (auto* subshell = std::get_if<ir::Subshell>(&redirected.subject->value)) {
            auto merged = *subshell;
            merged.redirections.insert(merged.redirections.end(), redirected.redirections.begin(), redirected.redirections.end());
            return run_subshell(merged);
        }
        if (auto* seq = std::get_if<ir::Sequence>(&redirected.subject->value)) {
            // A redirection over a boolean/sequence connective applies to both
            // operands: wrap each side so the redirect survives the
            // short-circuit in run_sequence (which dispatches the operands
            // directly and would otherwise drop the outer redirections).
            if (redirected.redirections.empty()) {
                return run_sequence(*seq);
            }
            auto wrap = [&](const ir::NodePtr& node) -> ir::NodePtr {
                return ir::node(ir::Redirected {.subject = node, .redirections = redirected.redirections}, "redirect");
            };
            auto merged = *seq;
            merged.left = wrap(merged.left);
            merged.right = wrap(merged.right);
            return run_sequence(merged);
        }
        return run_node(redirected.subject);
    }

    [[nodiscard]] static ExitStatus select_pipeline_status(const std::vector<ExitStatus>& statuses, PipefailPolicy policy) {
        return detail::select_pipeline_status(statuses, policy);
    }

    Environment environment_;
    std::filesystem::path cwd_ {std::filesystem::current_path()};
    ShellOptions options_;
    std::shared_ptr<Executor> executor_;
    std::shared_ptr<Expander> expander_;
    std::shared_ptr<kernel::Registry> kernels_;
    std::shared_ptr<ShellScriptingBackend> scripting_;
};

inline Result<std::string> ShellScriptingBackend::eval_command(std::string_view script, const Environment& /*environment*/) {
    ir::NodePtr body;
    if (parse_hook_) {
        auto program = parse_hook_(script);
        if (!program) {
            return program.error();
        }
        body = program.value().root;
    } else {
        // No grammar hook available in the header: collapse the script to a
        // single command via whitespace splitting. Full pipeline grammar inside
        // $(...) requires wiring the CLI parser through set_parse_hook.
        ir::Command command;
        std::string word;
        auto flush = [&] {
            if (!word.empty()) {
                command.argv.push_back(Argument::raw(word));
                word.clear();
            }
        };
        for (char ch : script) {
            if (ch == ' ' || ch == '\t' || ch == '\n') {
                flush();
            } else {
                word.push_back(ch);
            }
        }
        flush();
        if (command.argv.empty()) {
            return std::string {};
        }
        body = ir::command(std::move(command), "cmdsubst");
    }

    auto memory = std::make_shared<MemoryWriter>();
    StdioTarget target;
    target.kind = StdioTargetKind::memory;
    target.memory = memory;
    Redirection redirect {.stream = RedirectStream::stdout_stream, .mode = RedirectMode::truncate, .target = std::move(target)};
    ir::Redirected redirected {.subject = body, .redirections = {std::move(redirect)}};

    auto report = shell_->run(ir::node(std::move(redirected), "cmdsubst-capture"));
    if (!report) {
        return report.error();
    }

    std::string out = memory->bytes();
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

inline Result<std::string> ShellScriptingBackend::eval_lua(std::string_view script, const Environment& environment) {
    return detail::eval_qamrpp_lua(script, environment);
}

} // namespace lsh

namespace lakposht = lsh;
