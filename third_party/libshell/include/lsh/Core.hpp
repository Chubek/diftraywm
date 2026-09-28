#pragma once

// Layer 0 -- foundation.
//
// Value types only: diagnostics, the Result carrier, environment storage, the
// byte-stream abstraction (Reader/Writer/Sinklet), redirection descriptors, and
// process status. No platform headers, no IR, no execution policy. Everything
// above this layer may include Core; Core includes nothing from the project.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lsh {

// ---- diagnostics ----------------------------------------------------------

enum class ErrorCode : std::uint16_t {
    ok = 0,
    not_found = 1001,
    permission_denied = 1002,
    invalid_redirection = 1101,
    bad_connective = 1102,
    empty_argv = 1103,
    syntax_error = 1104,
    bad_expansion = 1201,
    bad_arithmetic = 1202,
    execution_failed = 1301,
    io_error = 1302,
    timeout = 1303,
    canceled = 1304,
    resource_limit = 1305,
    invalid_graph = 1401,
    readonly_variable = 1402,
};

struct Diagnostic {
    ErrorCode code {ErrorCode::ok};
    std::string message;
    std::string path;

    [[nodiscard]] constexpr bool ok() const noexcept { return code == ErrorCode::ok; }

    [[nodiscard]] explicit operator bool() const noexcept { return code != ErrorCode::ok; }
};

// Carrier for either a value or a diagnostic. Deliberately total: every public
// entry point returns Result so failures travel as data, never as exceptions.
template <typename Value>
class Result {
public:
    Result(Value value) : storage_(std::move(value)) {}         // NOLINT(google-explicit-constructor)
    Result(Diagnostic diagnostic) : storage_(std::move(diagnostic)) {}

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<Value>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] Value& value() & { return std::get<Value>(storage_); }
    [[nodiscard]] const Value& value() const& { return std::get<Value>(storage_); }
    [[nodiscard]] Value&& value() && { return std::get<Value>(std::move(storage_)); }

    [[nodiscard]] const Value* operator->() const { return &value(); }
    [[nodiscard]] Value* operator->() { return &value(); }

    [[nodiscard]] Value value_or(Value fallback) const& {
        return has_value() ? value() : std::move(fallback);
    }

    [[nodiscard]] Diagnostic& error() & { return std::get<Diagnostic>(storage_); }
    [[nodiscard]] const Diagnostic& error() const& { return std::get<Diagnostic>(storage_); }

private:
    std::variant<Value, Diagnostic> storage_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return !diagnostic_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Diagnostic& error() const& { return *diagnostic_; }
    [[nodiscard]] Diagnostic& error() & { return *diagnostic_; }

    [[nodiscard]] Result<void> ok() const { return {}; }

private:
    std::optional<Diagnostic> diagnostic_;
};

[[nodiscard]] inline Diagnostic ok_diagnostic() { return Diagnostic {ErrorCode::ok, {}, {}}; }

// Constructs a diagnostic. Returning Diagnostic (rather than a Result) lets
// `return failure(...)` work in any function returning Result<T>, including
// Result<void>.
[[nodiscard]] inline Diagnostic failure(ErrorCode code, std::string message, std::string path = {}) {
    return Diagnostic {code, std::move(message), std::move(path)};
}

// ---- process status -------------------------------------------------------

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

[[nodiscard]] inline ExitStatus exit_status(int code) {
    ExitStatus status;
    status.code = code;
    return status;
}

// Non-local control transfer produced by return/break/continue. Carried in the
// execution report instead of hidden state so loops can be reasoned about and
// inspected like any other value.
enum class ControlSignal : std::uint8_t { none, return_, break_, continue_ };

// ---- time and resources ---------------------------------------------------

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

// ---- environment ----------------------------------------------------------

enum class EnvironmentInheritance : std::uint8_t { copy, exported_only, empty, shared_explicit };

struct EnvVar {
    std::string key;
    std::string value;
    bool exported {true};
    bool readonly {false};
};

// POSIX name grammar: [A-Za-z_][A-Za-z0-9_]*.
[[nodiscard]] inline bool valid_variable_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto head = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(head) || head == '_')) {
        return false;
    }
    for (std::size_t index = 1; index < name.size(); ++index) {
        const auto ch = static_cast<unsigned char>(name[index]);
        if (!(std::isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return true;
}

class Environment {
public:
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const {
        auto found = vars_.find(key);
        if (found == vars_.end()) {
            return std::nullopt;
        }
        return found->second.value;
    }

    [[nodiscard]] bool is_exported(std::string_view key) const {
        auto found = vars_.find(key);
        return found != vars_.end() && found->second.exported;
    }

    [[nodiscard]] bool is_readonly(std::string_view key) const {
        auto found = vars_.find(key);
        return found != vars_.end() && found->second.readonly;
    }

    [[nodiscard]] bool contains(std::string_view key) const { return vars_.find(key) != vars_.end(); }

    // Unconditional store. Used by the runtime to seed defaults and to service
    // internal updates; callers that must honour `readonly` use assign().
    void set(std::string key, std::string value, bool exported = true) {
        EnvVar entry;
        entry.key = key;
        entry.value = std::move(value);
        entry.exported = exported;
        auto [it, _] = vars_.insert_or_assign(std::move(key), std::move(entry));
        it->second.key = it->first;
    }

    // POSIX assignment: rejects malformed names and readonly violations so the
    // shell can surface a diagnostic instead of silently overwriting.
    Result<void> assign(std::string key, std::string value, bool exported = true) {
        if (!valid_variable_name(key)) {
            return failure(ErrorCode::bad_expansion, "not a valid shell variable name", key);
        }
        if (is_readonly(key)) {
            return failure(ErrorCode::readonly_variable, "variable is read-only", key);
        }
        set(std::move(key), std::move(value), exported);
        return {};
    }

    void unset(std::string_view key) {
        // std::map has no heterogeneous erase before C++23, so resolve the
        // iterator first and erase through it.
        auto found = vars_.find(key);
        if (found != vars_.end()) {
            vars_.erase(found);
        }
    }

    void export_var(std::string_view key) {
        auto found = vars_.find(key);
        if (found != vars_.end()) {
            found->second.exported = true;
        }
    }

    void unexport_var(std::string_view key) {
        auto found = vars_.find(key);
        if (found != vars_.end()) {
            found->second.exported = false;
        }
    }

    void set_readonly(std::string_view key) {
        auto found = vars_.find(key);
        if (found != vars_.end()) {
            found->second.readonly = true;
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

    [[nodiscard]] std::size_t size() const noexcept { return vars_.size(); }
    [[nodiscard]] bool empty() const noexcept { return vars_.empty(); }

    void clear() { vars_.clear(); }

private:
    std::map<std::string, EnvVar, std::less<>> vars_;
};

// ---- byte streams ---------------------------------------------------------

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
            return failure(ErrorCode::io_error, "failed to write to stream");
        }
        return {};
    }

    Result<void> flush() override {
        stream_->flush();
        if (!*stream_) {
            return failure(ErrorCode::io_error, "failed to flush stream");
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
    void clear() noexcept { buffer_.clear(); }

private:
    std::string buffer_;
};

// Source half of the stream abstraction. Added with heredocs and in-process
// pipelines: builtins and kernels could previously never observe stdin, and
// external processes had no way to receive a synthesized payload.
class Reader {
public:
    virtual ~Reader() = default;
    // Reads up to `max` bytes. An empty buffer signals end of input.
    virtual Result<std::size_t> read(std::string& buffer, std::size_t max) = 0;
    virtual Result<void> close() { return {}; }
};

class MemoryReader final : public Reader {
public:
    MemoryReader() = default;
    explicit MemoryReader(std::string payload) : buffer_(std::move(payload)) {}

    Result<std::size_t> read(std::string& out, std::size_t max) override {
        if (cursor_ >= buffer_.size()) {
            out.clear();
            return std::size_t {0};
        }
        const std::size_t count = std::min(max, buffer_.size() - cursor_);
        out.assign(buffer_, cursor_, count);
        cursor_ += count;
        return count;
    }

    [[nodiscard]] bool exhausted() const noexcept { return cursor_ >= buffer_.size(); }

private:
    std::string buffer_;
    std::size_t cursor_ {0};
};

// Discards all input (the `</dev/null` equivalent for in-process readers).
class NullReader final : public Reader {
public:
    Result<std::size_t> read(std::string& out, std::size_t) override {
        out.clear();
        return std::size_t {0};
    }
};

class SinkletContext {
public:
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
            return failure(ErrorCode::invalid_graph, "lambda sinklet has no write handler", context.source);
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
            return failure(ErrorCode::invalid_graph, "sinklet writer has no sinklet");
        }
        if (!begun_) {
            if (auto result = sinklet_->begin(context_); !result) {
                return result.error();
            }
            begun_ = true;
        }
        return sinklet_->write(bytes, context_);
    }

    Result<void> flush() override { return {}; }

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
            if (auto result = primary_->write(chunk); !result) {
                return result.error();
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
            if (auto result = on_line(line, downstream_, context); !result) {
                return result.error();
            }
        }
        return {};
    }

    Result<void> end(SinkletContext& context) override {
        if (!partial_.empty()) {
            auto remainder = std::move(partial_);
            partial_.clear();
            if (auto result = on_line(remainder, downstream_, context); !result) {
                return result.error();
            }
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

// ---- redirection descriptors ---------------------------------------------

// StdioTarget and Redirection live in Expansion.hpp: a here-string carries an
// unexpanded word, so the target must be able to reference an Argument without
// Core depending on the expansion layer.

enum class RedirectStream : std::uint8_t { stdin_stream, stdout_stream, stderr_stream };
enum class RedirectMode : std::uint8_t { read, truncate, append, clobber, duplicate, read_write, close };
enum class StdioTargetKind : std::uint8_t {
    inherit,
    null_device,
    pipe,
    file,
    fd,
    memory,
    sinklet,
    closed,
    // A redirection path the Shell has not expanded yet.
    deferred,
    // A here-string: a word the Shell expands into an in-memory payload.
    here_string,
};
enum class PipefailPolicy : std::uint8_t { last, any_failed, none };
enum class Connective : std::uint8_t { sequence, and_if, or_if, background };
enum class CommandSource : std::uint8_t { external, builtin, kernel, adapter, auto_resolve };

} // namespace lsh
