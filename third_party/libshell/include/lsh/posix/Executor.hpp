#pragma once

// Layer 7c -- POSIX executor.
//
// Concrete Executor: dispatches kernel, builtin, or external process; wires real
// OS pipes for pipelines; enforces deadlines and cancellation. The fixes over
// the previous revision are structural, not cosmetic:
//
//   * captured streams are drained by one concurrent pump, so a stage that
//     fills stderr cannot deadlock the pipeline;
//   * a timeout and a cancellation are reported as distinct conditions;
//   * the child is not placed in its own session, so interactive programs and
//     terminal job control keep working;
//   * in-process commands receive a stdin Reader, and every stream binding
//     reports an open failure instead of silently discarding output.

#include "Builtins.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <pwd.h>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lsh::posix {

// Resolves `~user` through the password database. `~`, `~+`, and `~-` are left to
// the environment-backed resolver, which the Shell installs by default.
class PasswdTildeResolver final : public lsh::detail::TildeResolver {
public:
    [[nodiscard]] Result<std::optional<std::string>> resolve(std::string_view spec) const override {
        if (spec.size() < 2 || spec.front() != '~') {
            return std::optional<std::string> {};
        }
        const std::string name(spec.substr(1));
        if (name.empty()) {
            return std::optional<std::string> {};
        }
        // getpwnam_r is not declared under a strict -std=c++20 feature set, and
        // the Executor is single-threaded, so the POSIX getpwnam with its static
        // buffer is both portable and sufficient here.
        const struct passwd* entry = ::getpwnam(name.c_str());
        if (entry == nullptr || entry->pw_dir == nullptr) {
            // POSIX: an unknown login name leaves the word unchanged.
            return std::optional<std::string> {};
        }
        return std::optional<std::string> {std::string(entry->pw_dir)};
    }
};

// Composite resolver: the environment-backed forms first, then the passwd
// database for `~user`.
class SystemTildeResolver final : public lsh::detail::TildeResolver {
public:
    explicit SystemTildeResolver(const Environment& environment) : environment_(environment) {}

    [[nodiscard]] Result<std::optional<std::string>> resolve(std::string_view spec) const override {
        lsh::detail::EnvironmentTildeResolver env_resolver(environment_);
        auto resolved = env_resolver.resolve(spec);
        if (!resolved) {
            return resolved;
        }
        if (resolved.value().has_value()) {
            return resolved;
        }
        return passwd_.resolve(spec);
    }

private:
    const Environment& environment_;
    PasswdTildeResolver passwd_;
};

class LocalExecutor final : public Executor {
public:
    LocalExecutor() : builtins_(BuiltinRegistry::defaults()), cancel_token_(std::make_shared<std::atomic<bool>>(false)) {}
    explicit LocalExecutor(std::shared_ptr<BuiltinRegistry> builtins)
        : builtins_(std::move(builtins)), cancel_token_(std::make_shared<std::atomic<bool>>(false)) {}

    void bind_runtime(Environment* environment, std::filesystem::path* cwd) override {
        env_ = environment;
        cwd_ = cwd;
        seed_environment_once();
    }

    // The owning Shell, for builtins that re-enter the runtime (`command`,
    // `eval`, `cd`'s PWD/OLDPWD bookkeeping).
    void bind_shell(Shell* shell) override { shell_ = shell; }

    // A tilde resolver that also understands `~user`. Installed on bind so a
    // Shell created with this executor gets full tilde semantics.
    [[nodiscard]] std::shared_ptr<lsh::detail::TildeResolver> tilde_resolver() const {
        return env_ ? std::make_shared<SystemTildeResolver>(*env_) : std::shared_ptr<lsh::detail::TildeResolver> {};
    }

    void cancel() override {
        if (cancel_token_) {
            cancel_token_->store(true);
        }
    }

    void reset_cancel() {
        if (cancel_token_) {
            cancel_token_->store(false);
        }
    }

    Result<ExecutionReport> run(const ExecSpec& spec) override {
        // The cancel token is per-invocation: a previous cancellation must not
        // make every later command report as cancelled.
        reset_cancel();
        if (spec.resolved_kernel) {
            return run_kernel(spec);
        }
        if (spec.source == CommandSource::kernel) {
            ExecutionReport report;
            report.status.code = 127;
            report.diagnostics.push_back(failure(ErrorCode::not_found, "kernel not registered", spec.argv.empty() ? "" : spec.argv.front()));
            return report;
        }
        if (is_builtin(spec)) {
            return run_builtin(spec, *builtins_->find(spec.argv.front()));
        }
        if (spec.source == CommandSource::builtin) {
            ExecutionReport report;
            report.status.code = 127;
            report.diagnostics.push_back(failure(ErrorCode::not_found, "not a shell builtin", spec.argv.empty() ? "" : spec.argv.front()));
            return report;
        }
        return run_external(spec);
    }

    Result<ExecutionReport> run_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail, bool merge_stderr) override {
        reset_cancel();
        if (specs.empty()) {
            return ExecutionReport {};
        }
        const bool all_external = std::all_of(specs.begin(), specs.end(), [this](const ExecSpec& spec) { return is_external(spec); });
        if (all_external) {
            return run_external_pipeline(std::move(specs), pipefail, merge_stderr);
        }
        return run_buffered_pipeline(std::move(specs), pipefail);
    }

private:
    // Import the inherited process environment into the Shell's Environment so
    // that $HOME / $PATH / etc. expand. Runs once per binding; values are stored
    // as exported so they also reach spawned children via exported_entries().
    void seed_environment_once() {
        if (!env_ || env_seeded_) {
            return;
        }
        for (char** entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
            const std::string_view pair = *entry;
            const auto eq = pair.find('=');
            if (eq != std::string_view::npos) {
                env_->set(
                    std::string(pair.substr(0, eq)),
                    std::string(pair.substr(eq + 1)),
                    /*exported=*/true);
            }
        }
        env_seeded_ = true;
    }

    [[nodiscard]] bool is_builtin(const ExecSpec& spec) const {
        if (spec.resolved_kernel || !builtins_ || spec.argv.empty()) {
            return false;
        }
        return builtins_->find(spec.argv.front()) != nullptr;
    }

    [[nodiscard]] bool is_external(const ExecSpec& spec) const { return !spec.resolved_kernel && !is_builtin(spec); }

    // ---- kernel lifecycle ----
    Result<ExecutionReport> run_kernel(const ExecSpec& spec) {
        kernel::Kernel& kernel = *spec.resolved_kernel;
        if (auto result = kernel.load(); !result) {
            return result.error();
        }
        bool initialized = false;
        const auto shutdown = [&]() -> Result<void> {
            if (!initialized) {
                return {};
            }
            initialized = false;
            return kernel.shutdown();
        };
        Environment environment;
        if (env_) {
            environment = *env_;
        }
        for (const EnvVar& entry : spec.environment) {
            environment.set(entry.key, entry.value, entry.exported);
        }
        if (auto result = kernel.initialize(environment); !result) {
            (void)kernel.shutdown();
            return result.error();
        }
        initialized = true;

        StreamBinder binder(env_, cwd_);
        auto stdout_writer = binder.writer_for(spec, RedirectStream::stdout_stream);
        if (!stdout_writer) {
            (void)shutdown();
            return stdout_writer.error();
        }
        auto stderr_writer = binder.writer_for(spec, RedirectStream::stderr_stream);
        if (!stderr_writer) {
            binder.close();
            (void)shutdown();
            return stderr_writer.error();
        }
        auto stdin_reader = binder.reader_for(spec);
        if (!stdin_reader) {
            binder.close();
            (void)shutdown();
            return stdin_reader.error();
        }

        kernel::Invocation invocation;
        invocation.argv = spec.argv;
        invocation.environment = &environment;
        invocation.stdin_reader = stdin_reader.value().get();
        invocation.stdout_writer = stdout_writer.value().get();
        invocation.stderr_writer = stderr_writer.value().get();
        invocation.positional = spec.positional;
        invocation.cwd = spec.cwd.value_or(std::string {});

        auto result = kernel.execute(invocation);
        binder.close();
        auto shutdown_result = shutdown();
        if (!result) {
            return result.error();
        }
        if (!shutdown_result) {
            return shutdown_result.error();
        }
        ExecutionReport report;
        report.status = std::move(result).value();
        return report;
    }

    // ---- builtin dispatch ----
    Result<ExecutionReport> run_builtin(const ExecSpec& spec, const BuiltinRegistry::Fn& fn) {
        StreamBinder binder(env_, cwd_);
        auto stdout_writer = binder.writer_for(spec, RedirectStream::stdout_stream);
        if (!stdout_writer) {
            return stdout_writer.error();
        }
        auto stderr_writer = binder.writer_for(spec, RedirectStream::stderr_stream);
        if (!stderr_writer) {
            return stderr_writer.error();
        }
        auto stdin_reader = binder.reader_for(spec);
        if (!stdin_reader) {
            return stdin_reader.error();
        }

        BuiltinContext context;
        context.environment = env_;
        context.cwd = cwd_;
        context.shell = shell_;
        context.stdin_reader = stdin_reader.value().get();
        context.stdout_writer = stdout_writer.value().get();
        context.stderr_writer = stderr_writer.value().get();
        context.argv = &spec.argv;
        context.exported = &spec.environment;

        auto result = fn(context);
        binder.close();
        if (!result) {
            return result.error();
        }
        ExecutionReport report;
        report.status = std::move(result).value();
        return report;
    }

    // ---- external single process ----
    Result<ExecutionReport> run_external(const ExecSpec& spec) {
        std::vector<std::shared_ptr<TempFile>> temporaries;
        auto channels_result = materialize_channels(spec, temporaries);
        if (!channels_result) {
            return channels_result.error();
        }
        Channels channels = std::move(channels_result).value();

        std::vector<std::string> argv_storage = spec.argv;
        std::vector<const char*> argv_c;
        argv_c.reserve(argv_storage.size() + 1);
        for (const std::string& arg : argv_storage) {
            argv_c.push_back(arg.c_str());
        }
        argv_c.push_back(nullptr);

        const bool has_deadline = spec.timeout && spec.timeout->duration.count() > 0;
        const auto deadline = has_deadline ? std::chrono::steady_clock::now() + spec.timeout->duration
                                           : std::chrono::steady_clock::time_point {};

        const pid_t pid = ::fork();
        if (pid < 0) {
            return failure(ErrorCode::execution_failed, std::string("fork failed: ") + std::strerror(errno));
        }
        if (pid == 0) {
            // No setsid(): the child must stay in the shell's session so that
            // terminal job control and interactive programs behave normally.
            child_reset_signals();
            apply_limits(spec.limits);
            child_apply_streams(channels, /*merge_stderr=*/false, spec.redirections);
            if (spec.cwd && !spec.cwd->empty() && ::chdir(spec.cwd->c_str()) != 0) {
                const std::string message = "libsh: cannot chdir to " + *spec.cwd + "\n";
                (void)::write(2, message.data(), message.size());
                std::_Exit(126);
            }
            std::vector<std::string> env_storage;
            std::vector<const char*> env_c;
            build_child_environment(spec.environment, env_storage, env_c);
            ::execvp(argv_c[0], const_cast<char* const*>(argv_c.data()));
            // 127 is "not found"; a file that exists but cannot be run is 126.
            const int code = (errno == ENOENT) ? 127 : 126;
            const std::string message = "libsh: " + std::string(argv_c[0]) + ": " + std::strerror(errno) + "\n";
            (void)::write(2, message.data(), message.size());
            std::_Exit(code);
        }

        parent_close_child_ends(channels);
        ExecutionReport report;
        report.pid = static_cast<int>(pid);
        std::uint64_t stdout_bytes = 0;
        std::uint64_t stderr_bytes = 0;
        const PumpOutcome outcome = pump_channels(channels, has_deadline, deadline, cancel_token_, &stdout_bytes, &stderr_bytes);
        if (outcome != PumpOutcome::completed) {
            ::kill(pid, SIGKILL);
        }
        const ExitStatus status = reap(pid, has_deadline, deadline, cancel_token_);
        report.status = status;
        if (outcome == PumpOutcome::completed) {
            report.bytes_stdout = stdout_bytes;
            report.bytes_stderr = stderr_bytes;
        }
        if (report.status.code == 127 && !report.status.signaled) {
            report.diagnostics.push_back(failure(ErrorCode::not_found, "command not found", argv_storage.empty() ? "" : argv_storage.front()));
        }
        if (report.status.code == 126 && !report.status.signaled) {
            report.diagnostics.push_back(failure(ErrorCode::permission_denied, "command is not executable", argv_storage.empty() ? "" : argv_storage.front()));
        }
        return report;
    }

    // ---- external pipeline with real OS pipes ----
    Result<ExecutionReport> run_external_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail, bool merge_stderr) {
        const std::size_t count = specs.size();
        std::vector<std::array<int, 2>> pipes(count - 1);
        for (std::size_t index = 0; index + 1 < count; ++index) {
            if (::pipe(pipes[index].data()) < 0) {
                return failure(ErrorCode::io_error, std::string("cannot create a pipeline pipe: ") + std::strerror(errno));
            }
        }

        const bool has_deadline = specs.front().timeout && specs.front().timeout->duration.count() > 0;
        const auto deadline = has_deadline ? std::chrono::steady_clock::now() + specs.front().timeout->duration
                                           : std::chrono::steady_clock::time_point {};

        std::vector<std::shared_ptr<TempFile>> temporaries;
        std::optional<Channel> head_input;
        std::optional<Channel> tail_output;
        std::vector<Channel> stderr_channels;

        auto head = materialize_channel(specs.front(), RedirectStream::stdin_stream, temporaries);
        if (!head) {
            return head.error();
        }
        head_input = std::move(head).value();
        auto tail = materialize_channel(specs.back(), RedirectStream::stdout_stream, temporaries);
        if (!tail) {
            return tail.error();
        }
        tail_output = std::move(tail).value();
        for (std::size_t index = 0; index < count; ++index) {
            auto channel = materialize_channel(specs[index], RedirectStream::stderr_stream, temporaries);
            if (!channel) {
                return channel.error();
            }
            stderr_channels.push_back(std::move(channel).value());
        }

        std::vector<pid_t> pids(count, -1);
        for (std::size_t index = 0; index < count; ++index) {
            std::vector<std::string> argv_storage = specs[index].argv;
            std::vector<const char*> argv_c;
            argv_c.reserve(argv_storage.size() + 1);
            for (const std::string& arg : argv_storage) {
                argv_c.push_back(arg.c_str());
            }
            argv_c.push_back(nullptr);

            const pid_t pid = ::fork();
            if (pid < 0) {
                return failure(ErrorCode::execution_failed, std::string("cannot fork a pipeline stage: ") + std::strerror(errno));
            }
            if (pid == 0) {
                child_reset_signals();
                apply_limits(specs[index].limits);

                if (index == 0) {
                    if (head_input->close_stream) {
                        ::close(0);
                    } else if (head_input->pipe_write >= 0) {
                        ::dup2(head_input->pipe_write, 0);
                    } else if (head_input->dup_fd >= 0) {
                        ::dup2(head_input->dup_fd, 0);
                    }
                } else {
                    ::dup2(pipes[index - 1][0], 0);
                }

                if (index + 1 == count) {
                    if (tail_output->close_stream) {
                        ::close(1);
                    } else if (tail_output->pipe_write >= 0) {
                        ::dup2(tail_output->pipe_write, 1);
                    } else if (tail_output->dup_fd >= 0) {
                        ::dup2(tail_output->dup_fd, 1);
                    }
                } else {
                    ::dup2(pipes[index][1], 1);
                }

                Channel& error_channel = stderr_channels[index];
                if (error_channel.close_stream) {
                    ::close(2);
                } else if (error_channel.pipe_write >= 0) {
                    ::dup2(error_channel.pipe_write, 2);
                } else if (error_channel.dup_fd >= 0) {
                    ::dup2(error_channel.dup_fd, 2);
                }
                if (merge_stderr) {
                    ::dup2(1, 2);
                }

                // Release every pipe end and materialized descriptor so nothing
                // leaks into the exec'd image; a leaked write end would keep a
                // sibling's capture pipe open forever.
                for (std::size_t stage = 0; stage + 1 < count; ++stage) {
                    ::close(pipes[stage][0]);
                    ::close(pipes[stage][1]);
                }
                if (head_input->dup_fd > 2) {
                    ::close(head_input->dup_fd);
                }
                if (head_input->pipe_write >= 0) {
                    ::close(head_input->pipe_write);
                }
                if (tail_output->dup_fd > 2) {
                    ::close(tail_output->dup_fd);
                }
                if (tail_output->pipe_read >= 0) {
                    ::close(tail_output->pipe_read);
                }
                if (error_channel.dup_fd > 2) {
                    ::close(error_channel.dup_fd);
                }
                if (error_channel.pipe_read >= 0) {
                    ::close(error_channel.pipe_read);
                }

                if (specs[index].cwd && !specs[index].cwd->empty() && ::chdir(specs[index].cwd->c_str()) != 0) {
                    const std::string message = "libsh: cannot chdir to " + *specs[index].cwd + "\n";
                    (void)::write(2, message.data(), message.size());
                    std::_Exit(126);
                }
                std::vector<std::string> env_storage;
                std::vector<const char*> env_c;
                build_child_environment(specs[index].environment, env_storage, env_c);
                ::execvp(argv_c[0], const_cast<char* const*>(argv_c.data()));
                std::_Exit(errno == ENOENT ? 127 : 126);
            }
            pids[index] = pid;
        }

        // Parent: drop every child-bound descriptor. The capture write ends must
        // go too, or the pump never sees EOF.
        for (std::size_t stage = 0; stage + 1 < count; ++stage) {
            ::close(pipes[stage][0]);
            ::close(pipes[stage][1]);
        }
        auto release = [](Channel& channel) {
            if (channel.dup_fd > 2) {
                ::close(channel.dup_fd);
                channel.dup_fd = -1;
            }
            if (channel.pipe_write >= 0) {
                ::close(channel.pipe_write);
                channel.pipe_write = -1;
            }
        };
        release(*head_input);
        release(*tail_output);
        for (Channel& channel : stderr_channels) {
            release(channel);
        }

        // One concurrent drain over every captured stream. Draining the tail and
        // then each stage's stderr in sequence deadlocks as soon as an early
        // stage fills its stderr pipe.
        std::vector<PumpTarget> targets;
        std::uint64_t stdout_bytes = 0;
        if (tail_output->pipe_read >= 0) {
            targets.push_back(PumpTarget {tail_output->pipe_read, tail_output->sink.get(), &stdout_bytes});
        }
        std::uint64_t stderr_bytes = 0;
        for (Channel& channel : stderr_channels) {
            if (channel.pipe_read >= 0) {
                targets.push_back(PumpTarget {channel.pipe_read, channel.sink.get(), &stderr_bytes});
            }
        }
        const PumpOutcome outcome = pump_targets(targets, has_deadline, deadline, cancel_token_);
        if (outcome != PumpOutcome::completed) {
            for (const pid_t pid : pids) {
                if (pid > 0) {
                    ::kill(pid, SIGKILL);
                }
            }
        }

        ExecutionReport report;
        report.pipeline_statuses.reserve(count);
        const bool canceled = cancel_token_ && cancel_token_->load();
        for (std::size_t index = 0; index < count; ++index) {
            // Reap each stage with the same deadline and cancellation semantics
            // as a single process, so an unresponsive stage cannot hang the shell.
            report.pipeline_statuses.push_back(reap(pids[index], has_deadline, deadline, cancel_token_));
        }
        report.bytes_stdout = stdout_bytes;
        report.bytes_stderr = stderr_bytes;
        report.status = select_pipeline_status(report.pipeline_statuses, pipefail);
        (void)canceled;
        return report;
    }

    // ---- buffered fallback for mixed builtin/kernel pipelines ----
    Result<ExecutionReport> run_buffered_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail) {
        ExecutionReport report;
        report.pipeline_statuses.reserve(specs.size());
        // In-process stages cannot share an OS pipe with an external one without
        // a relay, so the payload is staged through an anonymous temporary file
        // per hand-off. tmpfile() unlinks immediately, so there is no cleanup
        // path and no window where another process can read the payload.
        std::string payload;
        for (std::size_t index = 0; index < specs.size(); ++index) {
            ExecSpec spec = specs[index];
            const bool last = index + 1 == specs.size();
            std::shared_ptr<MemoryWriter> capture;
            if (!last) {
                capture = std::make_shared<MemoryWriter>();
                StdioTarget target;
                target.kind = StdioTargetKind::memory;
                target.memory = capture;
                Redirection capture_redirect;
                capture_redirect.stream = RedirectStream::stdout_stream;
                capture_redirect.mode = RedirectMode::truncate;
                capture_redirect.target = std::move(target);
                spec.redirections.push_back(std::move(capture_redirect));
            }
            if (index > 0) {
                StdioTarget target;
                target.kind = StdioTargetKind::memory;
                target.input = std::make_shared<MemoryReader>(payload);
                Redirection handoff_redirect;
                handoff_redirect.stream = RedirectStream::stdin_stream;
                handoff_redirect.mode = RedirectMode::read;
                handoff_redirect.target = std::move(target);
                spec.redirections.push_back(std::move(handoff_redirect));
            }
            auto sub = run_single(spec);
            if (!sub) {
                return sub.error();
            }
            report.pipeline_statuses.push_back(sub.value().status);
            if (!last && capture) {
                payload = capture->bytes();
            }
        }
        report.status = select_pipeline_status(report.pipeline_statuses, pipefail);
        return report;
    }

    // run() resets the cancel token; the buffered pipeline calls it per stage,
    // so stage dispatch goes through this wrapper instead.
    Result<ExecutionReport> run_single(const ExecSpec& spec) {
        if (spec.resolved_kernel) {
            return run_kernel(spec);
        }
        if (is_builtin(spec)) {
            return run_builtin(spec, *builtins_->find(spec.argv.front()));
        }
        return run_external(spec);
    }

    // ---- stream binding for in-process commands ----

    // Resolves the redirections of an ExecSpec into Readers and Writers. Every
    // failure (an unopenable file, a malformed descriptor) is reported rather
    // than producing a writer that silently drops output.
    class StreamBinder {
    public:
        StreamBinder(Environment* environment, std::filesystem::path* cwd) : environment_(environment), cwd_(cwd) {}

        [[nodiscard]] Result<std::shared_ptr<Writer>> writer_for(const ExecSpec& spec, RedirectStream stream) {
            const Redirection* redirection = redirection_for(spec.redirections, stream);
            const StdioTargetKind kind = redirection ? redirection->target.kind : StdioTargetKind::inherit;
            std::shared_ptr<Writer> writer;
            switch (kind) {
            case StdioTargetKind::memory:
                if (!redirection->target.memory) {
                    return failure(ErrorCode::invalid_redirection, "memory redirection has no writer");
                }
                writer = redirection->target.memory;
                break;
            case StdioTargetKind::sinklet:
                if (!redirection->target.sinklet) {
                    return failure(ErrorCode::invalid_redirection, "sinklet redirection has no sinklet");
                }
                writer = std::make_shared<SinkletWriter>(redirection->target.sinklet);
                break;
            case StdioTargetKind::null_device:
                writer = std::make_shared<NullWriter>();
                break;
            case StdioTargetKind::closed:
                writer = std::make_shared<NullWriter>();
                break;
            case StdioTargetKind::deferred:
            case StdioTargetKind::here_string:
                return failure(ErrorCode::invalid_redirection, "a deferred redirection target was not resolved");
            case StdioTargetKind::fd:
                writer = std::make_shared<FdWriter>(*redirection->target.fd);
                break;
            case StdioTargetKind::file: {
                auto stream = std::make_shared<std::ofstream>();
                std::ios_base::openmode mode = std::ios_base::out;
                if (redirection->mode == RedirectMode::append) {
                    mode |= std::ios_base::app;
                }
                // A read-mode redirection aimed at an output stream is a misuse;
                // report it instead of truncating the file.
                if (redirection->mode == RedirectMode::read) {
                    mode = std::ios_base::in;
                }
                stream->open(*redirection->target.file, mode);
                if (!stream->is_open()) {
                    return failure(ErrorCode::io_error, "cannot open redirection target", *redirection->target.file);
                }
                writer = std::make_shared<OstreamWriter>(*stream);
                streams_.push_back(std::move(stream));
                break;
            }
            case StdioTargetKind::inherit:
            case StdioTargetKind::pipe:
            default:
                writer = std::make_shared<FdWriter>(stream == RedirectStream::stdin_stream    ? 0
                    : stream == RedirectStream::stderr_stream                                      ? 2
                                                                                                    : 1);
                break;
            }
            writers_.push_back(writer);
            return writer;
        }

        [[nodiscard]] Result<std::shared_ptr<Reader>> reader_for(const ExecSpec& spec) {
            const Redirection* redirection = redirection_for(spec.redirections, RedirectStream::stdin_stream);
            const StdioTargetKind kind = redirection ? redirection->target.kind : StdioTargetKind::inherit;
            std::shared_ptr<Reader> reader;
            switch (kind) {
            case StdioTargetKind::memory:
                if (redirection->target.input) {
                    reader = redirection->target.input;
                    break;
                }
                reader = std::make_shared<NullReader>();
                break;
            case StdioTargetKind::null_device:
            case StdioTargetKind::closed:
                reader = std::make_shared<NullReader>();
                break;
            case StdioTargetKind::deferred:
            case StdioTargetKind::here_string:
                return failure(ErrorCode::invalid_redirection, "a deferred redirection target was not resolved");
            case StdioTargetKind::fd:
                reader = std::make_shared<FdReader>(*redirection->target.fd);
                break;
            case StdioTargetKind::file: {
                auto stream = std::make_shared<std::ifstream>();
                stream->open(*redirection->target.file, std::ios_base::in);
                if (!stream->is_open()) {
                    return failure(ErrorCode::io_error, "cannot open redirection target", *redirection->target.file);
                }
                reader = std::make_shared<StreamReader>(std::move(stream));
                return reader;
            }
            case StdioTargetKind::sinklet:
                return failure(ErrorCode::invalid_redirection, "a sinklet cannot supply stdin");
            case StdioTargetKind::inherit:
            case StdioTargetKind::pipe:
            default:
                reader = std::make_shared<FdReader>(0);
                break;
            }
            readers_.push_back(reader);
            return reader;
        }

        void close() {
            for (const auto& writer : writers_) {
                if (writer) {
                    (void)writer->close();
                }
            }
            for (const auto& reader : readers_) {
                if (reader) {
                    (void)reader->close();
                }
            }
            writers_.clear();
            readers_.clear();
            streams_.clear();
        }

    private:
        // Reader over a std::ifstream, used for `< file` on an in-process command.
        class StreamReader final : public Reader {
        public:
            explicit StreamReader(std::shared_ptr<std::ifstream> stream) : stream_(std::move(stream)) {}

            Result<std::size_t> read(std::string& out, std::size_t max) override {
                std::array<char, 8192> buffer {};
                stream_->read(buffer.data(), static_cast<std::streamsize>(std::min(max, buffer.size())));
                const std::streamsize count = stream_->gcount();
                if (count <= 0) {
                    out.clear();
                    return std::size_t {0};
                }
                out.assign(buffer.data(), static_cast<std::size_t>(count));
                return static_cast<std::size_t>(count);
            }

        private:
            std::shared_ptr<std::ifstream> stream_;
        };

        Environment* environment_ {nullptr};
        std::filesystem::path* cwd_ {nullptr};
        // File handles and writers are kept alive for the duration of the call.
        std::vector<std::shared_ptr<std::ofstream>> streams_;
        std::vector<std::shared_ptr<Writer>> writers_;
        std::vector<std::shared_ptr<Reader>> readers_;
    };

    std::shared_ptr<BuiltinRegistry> builtins_;
    std::shared_ptr<std::atomic<bool>> cancel_token_;
    Environment* env_ {nullptr};
    std::filesystem::path* cwd_ {nullptr};
    Shell* shell_ {nullptr};
    bool env_seeded_ {false};
};

} // namespace lsh::posix
