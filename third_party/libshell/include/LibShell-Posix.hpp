#pragma once

// Concrete POSIX executor for the LibShell runtime. Isolates fork/execvp,
// pipe wiring, resource limits, and signal handling behind the existing
// lsh::Executor contract so the core header (LibShell.hpp) stays free of
// platform headers. Requires a POSIX environment.

#include "LibShell.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// POSIX defines no standard header for the process environment vector; it is
// exposed by <unistd.h> only under extension feature macros. Declare it
// explicitly so importing the inherited environment does not depend on a
// specific feature-test setting.
extern "C" char** environ;

namespace lsh {

// Writer over a raw file descriptor. Used to back inherited streams (fd 1/2)
// so in-process builtins and kernels emit to the real terminal.
class FdWriter final : public Writer {
public:
    explicit FdWriter(int fd, bool own = false) : fd_(fd), own_(own) {}
    ~FdWriter() override {
        if (own_ && fd_ >= 0) {
            ::close(fd_);
        }
    }

    Result<void> write(std::string_view bytes) override {
        const char* data = bytes.data();
        std::size_t remaining = bytes.size();
        while (remaining > 0) {
            ssize_t written = ::write(fd_, data, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return Diagnostic {ErrorCode::io_error, "fd write failed", {}};
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        return {};
    }

private:
    int fd_;
    bool own_;
};

// Discards all output (>/dev/null equivalent for in-process writers).
class NullWriter final : public Writer {
public:
    Result<void> write(std::string_view /*bytes*/) override { return {}; }
};

namespace posix {

// One materialized stream for an external process. Either a concrete fd to
// dup2 onto the stream (file/null/fd/inherit), or a pump pipe whose read end
// the parent drains into `sink` (memory/sinklet targets).
struct Channel {
    int dup_fd {-1};     // fd to dup2 in child; -1 means inherit
    int pipe_write {-1}; // child writes here when pumping
    int pipe_read {-1};  // parent reads here when pumping
    std::shared_ptr<Writer> sink;
    RedirectMode mode {RedirectMode::truncate};
};

inline int open_flags_for(RedirectMode mode, RedirectStream stream) {
    if (stream == RedirectStream::stdin_stream || mode == RedirectMode::read) {
        return O_RDONLY;
    }
    int flags = O_WRONLY | O_CREAT;
    if (mode == RedirectMode::append) {
        flags |= O_APPEND;
    } else {
        flags |= O_TRUNC;
    }
    return flags;
}

// Resolve the redirection that applies to `stream` (last one wins, POSIX).
inline const Redirection* redirection_for(const std::vector<Redirection>& redirections, RedirectStream stream) {
    const Redirection* found = nullptr;
    for (const Redirection& redirection : redirections) {
        if (redirection.stream == stream) {
            found = &redirection;
        }
    }
    return found;
}

inline Result<Channel> materialize_channel(const ExecSpec& spec, RedirectStream stream) {
    Channel channel;
    const Redirection* redirection = redirection_for(spec.redirections, stream);
    StdioTargetKind kind = redirection ? redirection->target.kind : StdioTargetKind::inherit;
    channel.mode = redirection ? redirection->mode : RedirectMode::truncate;

    switch (kind) {
    case StdioTargetKind::inherit:
    case StdioTargetKind::pipe:
        channel.dup_fd = -1; // child keeps its inherited fd
        break;
    case StdioTargetKind::null_device:
        channel.dup_fd = ::open("/dev/null", stream == RedirectStream::stdin_stream ? O_RDONLY : (O_WRONLY | O_CREAT));
        if (channel.dup_fd < 0) {
            return Diagnostic {ErrorCode::io_error, "cannot open /dev/null", {}};
        }
        break;
    case StdioTargetKind::file: {
        const std::string& path = *redirection->target.file;
        channel.dup_fd = ::open(path.c_str(), open_flags_for(channel.mode, stream) | O_CLOEXEC, 0666);
        if (channel.dup_fd < 0) {
            return Diagnostic {ErrorCode::io_error, "cannot open redirection target", path};
        }
        break;
    }
    case StdioTargetKind::fd:
        channel.dup_fd = *redirection->target.fd;
        break;
    case StdioTargetKind::memory:
    case StdioTargetKind::sinklet:
        if (stream == RedirectStream::stdin_stream) {
            // Stdin pumping from a writer is not supported for external
            // processes in this revision; inherit instead.
            channel.dup_fd = -1;
            break;
        }
        int pfd[2];
        if (::pipe(pfd) < 0) {
            return Diagnostic {ErrorCode::io_error, "pipe creation failed", {}};
        }
        ::fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
        channel.pipe_write = pfd[1];
        channel.pipe_read = pfd[0];
        if (kind == StdioTargetKind::memory) {
            channel.sink = redirection->target.memory;
        } else {
            channel.sink = std::make_shared<SinkletWriter>(redirection->target.sinklet);
        }
        break;
    }
    return channel;
}

using Channels = std::array<Channel, 3>;

inline Result<Channels> materialize_channels(const ExecSpec& spec) {
    auto in = materialize_channel(spec, RedirectStream::stdin_stream);
    if (!in) {
        return in.error();
    }
    auto out = materialize_channel(spec, RedirectStream::stdout_stream);
    if (!out) {
        return out.error();
    }
    auto err = materialize_channel(spec, RedirectStream::stderr_stream);
    if (!err) {
        return err.error();
    }
    return Channels {{std::move(in).value(), std::move(out).value(), std::move(err).value()}};
}

inline void child_apply_streams(Channels& channels, bool merge_stderr) {
    for (int stream = 0; stream < 3; ++stream) {
        if (channels[stream].pipe_write >= 0) {
            ::dup2(channels[stream].pipe_write, stream);
        } else if (channels[stream].dup_fd >= 0) {
            ::dup2(channels[stream].dup_fd, stream);
        }
    }
    if (merge_stderr) {
        ::dup2(1, 2);
    }
    // Close materialized originals and pipe ends so they do not leak into exec.
    for (int stream = 0; stream < 3; ++stream) {
        Channel& channel = channels[stream];
        if (channel.dup_fd > 2) {
            ::close(channel.dup_fd);
        }
        if (channel.pipe_write >= 0) {
            ::close(channel.pipe_write);
        }
        if (channel.pipe_read >= 0) {
            ::close(channel.pipe_read);
        }
    }
}

// Build a NULL-terminated key=value vector from the Shell's exported
// environment table and point `environ` at it. Called in a forked child
// immediately before execvp; execvp passes `environ` to execve. `storage`
// and `c_ptrs` must outlive the call (they live on the child stack until the
// image is replaced).
inline void build_child_environment(const std::vector<EnvVar>& environment,
                                    std::vector<std::string>& storage,
                                    std::vector<const char*>& c_ptrs) {
    storage.clear();
    storage.reserve(environment.size());
    for (const EnvVar& entry : environment) {
        storage.push_back(entry.key + "=" + entry.value);
    }
    c_ptrs.clear();
    c_ptrs.reserve(storage.size() + 1);
    for (const std::string& s : storage) {
        c_ptrs.push_back(s.c_str());
    }
    c_ptrs.push_back(nullptr);
    ::environ = const_cast<char**>(c_ptrs.data());
}

// Parent closes its copies of child-bound fds; keeps pump read ends.
inline void parent_close_child_ends(Channels& channels) {
    for (int stream = 0; stream < 3; ++stream) {
        Channel& channel = channels[stream];
        if (channel.dup_fd > 2) {
            ::close(channel.dup_fd);
        }
        if (channel.pipe_write >= 0) {
            ::close(channel.pipe_write);
        }
    }
}

inline void apply_limits(const std::optional<ResourceLimits>& limits) {
    if (!limits) {
        return;
    }
    auto set = [](int resource, std::uint64_t value) {
        rlimit lim;
        lim.rlim_cur = value;
        lim.rlim_max = value;
        ::setrlimit(resource, &lim); // best-effort; ignore EPERM for non-root
    };
    if (limits->cpu_time_seconds) {
        set(RLIMIT_CPU, *limits->cpu_time_seconds);
    }
    if (limits->memory_bytes) {
#ifdef RLIMIT_AS
        set(RLIMIT_AS, *limits->memory_bytes);
#else
        set(RLIMIT_DATA, *limits->memory_bytes);
#endif
    }
    if (limits->file_size_bytes) {
        set(RLIMIT_FSIZE, *limits->file_size_bytes);
    }
    if (limits->open_files) {
        set(RLIMIT_NOFILE, *limits->open_files);
    }
    if (limits->processes) {
        set(RLIMIT_NPROC, *limits->processes);
    }
}

inline ExitStatus decode_status(int wstatus, bool timed_out, bool canceled) {
    ExitStatus status;
    if (canceled) {
        status.canceled = true;
        status.code = 130;
        return status;
    }
    if (timed_out) {
        status.timed_out = true;
        status.code = 124;
        return status;
    }
    if (WIFEXITED(wstatus)) {
        status.code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        int sig = WTERMSIG(wstatus);
        status.signaled = true;
        status.signal = sig;
        status.code = 128 + sig;
    }
    return status;
}

// Drain pump read ends into their sinks until EOF or deadline/cancel. Returns
// true if the deadline or cancel fired before all streams reached EOF.
inline bool pump_channels(Channels& channels, bool has_deadline, std::chrono::steady_clock::time_point deadline,
                          const std::shared_ptr<std::atomic<bool>>& cancel) {
    char buffer[8192];
    for (;;) {
        if (cancel && cancel->load()) {
            return true;
        }
        std::vector<std::pair<int, Writer*>> active;
        for (Channel& channel : channels) {
            if (channel.pipe_read >= 0) {
                active.emplace_back(channel.pipe_read, channel.sink.get());
            }
        }
        if (active.empty()) {
            return false;
        }

        pollfd fds[2];
        int nfds = 0;
        for (auto& [fd, writer] : active) {
            fds[nfds].fd = fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            ++nfds;
        }
        int timeout = -1;
        if (has_deadline) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return true;
            }
            timeout = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        }
        int ready = ::poll(fds, static_cast<nfds_t>(nfds), timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            return true; // timed out
        }
        for (int index = 0; index < nfds; ++index) {
            if (fds[index].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t n = ::read(fds[index].fd, buffer, sizeof(buffer));
                if (n > 0) {
                    if (active[index].second) {
                        (void)active[index].second->write(std::string_view(buffer, static_cast<std::size_t>(n)));
                    }
                } else {
                    ::close(fds[index].fd);
                    for (Channel& channel : channels) {
                        if (channel.pipe_read == fds[index].fd) {
                            channel.pipe_read = -1;
                        }
                    }
                }
            }
        }
    }
}

// Reap a child, honoring an optional wall-clock deadline and a cancel token.
inline ExitStatus reap(pid_t pid, bool has_deadline, std::chrono::steady_clock::time_point deadline,
                       const std::shared_ptr<std::atomic<bool>>& cancel) {
    int wstatus = 0;
    bool timed_out = false;
    bool canceled = false;
    for (;;) {
        if (cancel && cancel->load()) {
            canceled = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &wstatus, 0);
            break;
        }
        if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &wstatus, 0);
            break;
        }
        pid_t waited = ::waitpid(pid, &wstatus, WNOHANG);
        if (waited == pid) {
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return decode_status(wstatus, timed_out, canceled);
}

} // namespace posix

// Build an ExitStatus with a non-zero code without tripping
// -Wmissing-field-initializers on partial aggregate init.
inline ExitStatus with_code(int code) {
    ExitStatus status;
    status.code = code;
    return status;
}

// Registry of in-process builtins. Each builtin receives the expanded argv and
// borrowed shell state (env, cwd) plus materialized stdout/stderr writers.
class BuiltinRegistry {
public:
    using Fn = std::function<Result<ExitStatus>(const std::vector<std::string>&, Environment*, std::filesystem::path*, Writer*, Writer*)>;

    void add(std::string name, Fn fn) { entries_[std::move(name)] = std::move(fn); }

    [[nodiscard]] const Fn* find(std::string_view name) const {
        auto it = entries_.find(name);
        return it == entries_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] static std::shared_ptr<BuiltinRegistry> defaults() {
        auto registry = std::make_shared<BuiltinRegistry>();

        registry->add(":", [](const auto&, auto*, auto*, auto*, auto*) { return ExitStatus {}; });
        registry->add("true", [](const auto&, auto*, auto*, auto*, auto*) { return ExitStatus {}; });
        registry->add("false", [](const auto&, auto*, auto*, auto*, auto*) { return with_code(1); });

        registry->add("echo", [](const std::vector<std::string>& argv, Environment*, std::filesystem::path*, Writer* out, Writer*) -> Result<ExitStatus> {
            if (!out) {
                return ExitStatus {};
            }
            bool newline = true;
            std::size_t start = 1;
            while (start < argv.size() && argv[start] == "-n") {
                newline = false;
                ++start;
            }
            std::string buffer;
            for (std::size_t i = start; i < argv.size(); ++i) {
                if (i > start) {
                    buffer.push_back(' ');
                }
                buffer += argv[i];
            }
            if (newline) {
                buffer.push_back('\n');
            }
            if (auto r = out->write(buffer); !r) {
                return r.error();
            }
            return ExitStatus {};
        });

        registry->add("printf", [](const std::vector<std::string>& argv, Environment*, std::filesystem::path*, Writer* out, Writer*) -> Result<ExitStatus> {
            if (!out || argv.size() < 2) {
                return with_code(1);
            }
            const std::string& fmt = argv[1];
            std::size_t arg = 2;
            std::string buffer;
            // POSIX printf reuses the format string to convert each remaining
            // argument; a pass that consumes no conversion stops the loop so a
            // conversion-less format cannot spin forever.
            for (;;) {
                std::size_t consumed = 0;
                for (std::size_t i = 0; i < fmt.size(); ++i) {
                    char ch = fmt[i];
                    if (ch == '\\' && i + 1 < fmt.size()) {
                        char next = fmt[++i];
                        buffer.push_back(next == 'n' ? '\n' : next == 't' ? '\t' : next);
                        continue;
                    }
                    if (ch == '%' && i + 1 < fmt.size()) {
                        char spec = fmt[++i];
                        if (spec == 's') {
                            if (arg < argv.size()) {
                                buffer += argv[arg++];
                                ++consumed;
                            }
                        } else if (spec == 'd') {
                            if (arg < argv.size()) {
                                buffer += argv[arg++];
                                ++consumed;
                            } else {
                                buffer += "0";
                            }
                        } else if (spec == '%') {
                            buffer.push_back('%');
                        } else {
                            buffer.push_back('%');
                            buffer.push_back(spec);
                        }
                        continue;
                    }
                    buffer.push_back(ch);
                }
                if (consumed == 0 || arg >= argv.size()) {
                    break;
                }
            }
            if (auto r = out->write(buffer); !r) {
                return r.error();
            }
            return ExitStatus {};
        });

        registry->add("pwd", [](const std::vector<std::string>&, Environment*, std::filesystem::path* cwd, Writer* out, Writer*) -> Result<ExitStatus> {
            if (!out || !cwd) {
                return with_code(1);
            }
            std::string line = cwd->string();
            line.push_back('\n');
            if (auto r = out->write(line); !r) {
                return r.error();
            }
            return ExitStatus {};
        });

        registry->add("cd", [](const std::vector<std::string>& argv, Environment*, std::filesystem::path* cwd, Writer*, Writer* err) -> Result<ExitStatus> {
            if (!cwd) {
                return with_code(1);
            }
            std::filesystem::path target = argv.size() > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(".");
            std::filesystem::path resolved = target.is_absolute() ? target : (*cwd / target);
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(resolved, ec);
            if (ec || !std::filesystem::is_directory(canonical, ec)) {
                if (err) {
                    std::string name = argv.size() > 1 ? argv[1] : std::string(".");
                    std::string msg = "cd: not a directory: " + name + "\n";
                    (void)err->write(msg);
                }
                return with_code(1);
            }
            *cwd = canonical;
            return ExitStatus {};
        });

        registry->add("export", [](const std::vector<std::string>& argv, Environment* env, std::filesystem::path*, Writer*, Writer*) -> Result<ExitStatus> {
            if (!env) {
                return ExitStatus {};
            }
            for (std::size_t i = 1; i < argv.size(); ++i) {
                const std::string& token = argv[i];
                auto eq = token.find('=');
                if (eq != std::string::npos) {
                    env->set(token.substr(0, eq), token.substr(eq + 1), true);
                } else {
                    env->export_var(token);
                }
            }
            return ExitStatus {};
        });

        registry->add("unset", [](const std::vector<std::string>& argv, Environment* env, std::filesystem::path*, Writer*, Writer*) -> Result<ExitStatus> {
            if (!env) {
                return ExitStatus {};
            }
            for (std::size_t i = 1; i < argv.size(); ++i) {
                env->unset(argv[i]);
            }
            return ExitStatus {};
        });

        registry->add("set", [](const std::vector<std::string>& argv, Environment* env, std::filesystem::path*, Writer*, Writer*) -> Result<ExitStatus> {
            if (!env) {
                return ExitStatus {};
            }
            for (std::size_t i = 1; i < argv.size(); ++i) {
                const std::string& token = argv[i];
                auto eq = token.find('=');
                if (eq != std::string::npos) {
                    env->set(token.substr(0, eq), token.substr(eq + 1), false);
                }
            }
            return ExitStatus {};
        });

        return registry;
    }

private:
    std::map<std::string, Fn, std::less<>> entries_;
};

// Concrete POSIX executor. Launches external processes, drives kernel
// lifecycle, and dispatches builtins — all behind Executor. Borrowed shell
// state (env, cwd) is bound via bind_runtime.
class LocalExecutor final : public Executor {
public:
    LocalExecutor() : builtins_(BuiltinRegistry::defaults()), cancel_token_(std::make_shared<std::atomic<bool>>()) {}
    explicit LocalExecutor(std::shared_ptr<BuiltinRegistry> builtins)
        : builtins_(std::move(builtins)), cancel_token_(std::make_shared<std::atomic<bool>>()) {}

    void bind_runtime(Environment* environment, std::filesystem::path* cwd) override {
        env_ = environment;
        cwd_ = cwd;
        seed_environment_once();
    }

    void cancel() {
        if (cancel_token_) {
            cancel_token_->store(true);
        }
    }

    Result<ExecutionReport> run(const ExecSpec& spec) override {
        if (spec.resolved_kernel) {
            return run_kernel(spec);
        }
        if (builtins_ && !spec.argv.empty()) {
            if (auto* builtin = builtins_->find(spec.argv.front())) {
                return run_builtin(spec, *builtin);
            }
        }
        return run_external(spec);
    }

    Result<ExecutionReport> run_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail, bool merge_stderr) override {
        if (specs.empty()) {
            ExecutionReport report;
            return report;
        }
        const bool all_external = std::all_of(specs.begin(), specs.end(), [&](const ExecSpec& spec) { return is_external(spec); });
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
            std::string_view pair = *entry;
            const auto eq = pair.find('=');
            if (eq != std::string_view::npos) {
                env_->set(std::string(pair.substr(0, eq)),
                          std::string(pair.substr(eq + 1)),
                          /*exported=*/true);
            }
        }
        env_seeded_ = true;
    }

    bool is_external(const ExecSpec& spec) const {
        if (spec.resolved_kernel) {
            return false;
        }
        if (builtins_ && !spec.argv.empty() && builtins_->find(spec.argv.front())) {
            return false;
        }
        return true;
    }

    // ---- kernel lifecycle ----
    Result<ExecutionReport> run_kernel(const ExecSpec& spec) {
        kernel::Kernel& kernel = *spec.resolved_kernel;
        if (auto r = kernel.load(); !r) {
            return r.error();
        }
        Environment env;
        if (env_) {
            env = *env_;
        }
        for (const EnvVar& entry : spec.environment) {
            env.set(entry.key, entry.value, entry.exported);
        }
        if (auto r = kernel.initialize(env); !r) {
            (void)kernel.shutdown();
            return r.error();
        }

        BoundWriters writers;
        auto stdout_writer = writers.writer_for(spec, RedirectStream::stdout_stream);
        auto stderr_writer = writers.writer_for(spec, RedirectStream::stderr_stream);

        kernel::Invocation invocation;
        invocation.argv = spec.argv;
        invocation.environment = &env;
        invocation.stdout_writer = stdout_writer.get();
        invocation.stderr_writer = stderr_writer.get();

        auto result = kernel.execute(invocation);
        writers.close();
        (void)kernel.shutdown();
        if (!result) {
            return result.error();
        }
        ExecutionReport report;
        report.status = std::move(result).value();
        return report;
    }

    // ---- builtin dispatch ----
    Result<ExecutionReport> run_builtin(const ExecSpec& spec, const BuiltinRegistry::Fn& fn) {
        BoundWriters writers;
        auto stdout_writer = writers.writer_for(spec, RedirectStream::stdout_stream);
        auto stderr_writer = writers.writer_for(spec, RedirectStream::stderr_stream);
        auto result = fn(spec.argv, env_, cwd_, stdout_writer.get(), stderr_writer.get());
        writers.close();
        if (!result) {
            return result.error();
        }
        ExecutionReport report;
        report.status = std::move(result).value();
        return report;
    }

    // ---- external single process ----
    Result<ExecutionReport> run_external(const ExecSpec& spec) {
        auto channels_result = posix::materialize_channels(spec);
        if (!channels_result) {
            return channels_result.error();
        }
        posix::Channels channels = std::move(channels_result).value();

        std::vector<std::string> argv_storage = spec.argv;
        std::vector<const char*> argv_c;
        argv_c.reserve(argv_storage.size() + 1);
        for (const std::string& arg : argv_storage) {
            argv_c.push_back(arg.c_str());
        }
        argv_c.push_back(nullptr);

        bool has_deadline = spec.timeout && spec.timeout->duration.count() > 0;
        auto deadline = has_deadline ? std::chrono::steady_clock::now() + spec.timeout->duration : std::chrono::steady_clock::time_point {};

        pid_t pid = ::fork();
        if (pid < 0) {
            return Diagnostic {ErrorCode::execution_failed, "fork failed", {}};
        }
        if (pid == 0) {
            ::setsid();
            posix::apply_limits(spec.limits);
            posix::child_apply_streams(channels, false);
            if (spec.cwd && !spec.cwd->empty()) {
                if (::chdir(spec.cwd->c_str()) != 0) {
                    std::string msg = "libsh: cannot chdir to " + *spec.cwd + "\n";
                    ssize_t w = ::write(2, msg.data(), msg.size());
                    (void)w;
                    std::_Exit(126);
                }
            }
            // Replace the child environment with the Shell's exported table so
            // `export`ed and seeded-inherited variables reach the program. The
            // vectors live on the child stack and are consumed immediately:
            // execvp passes `environ` to execve, which replaces the image.
            std::vector<std::string> env_storage;
            std::vector<const char*> env_c;
            posix::build_child_environment(spec.environment, env_storage, env_c);
            ::execvp(argv_c[0], const_cast<char* const*>(argv_c.data()));
            int err = errno;
            std::string msg = "libsh: " + std::string(argv_c[0]) + ": " + std::strerror(err) + "\n";
            ssize_t w = ::write(2, msg.data(), msg.size());
            (void)w;
            std::_Exit(127);
        }

        posix::parent_close_child_ends(channels);
        bool timed_out = posix::pump_channels(channels, has_deadline, deadline, cancel_token_);
        if (timed_out) {
            ::kill(pid, SIGKILL);
        }
        ExecutionReport report;
        report.pid = pid;
        int wstatus = 0;
        for (;;) {
            pid_t waited = ::waitpid(pid, &wstatus, 0);
            if (waited == pid || waited < 0) {
                break;
            }
        }
        report.status = posix::decode_status(wstatus, timed_out, cancel_token_ && cancel_token_->load());
        if (report.status.code == 127 && !report.status.signaled) {
            report.diagnostics.push_back(Diagnostic {ErrorCode::not_found, "command not found", argv_storage.empty() ? "" : argv_storage.front()});
        }
        return report;
    }

    // ---- external pipeline with real OS pipes ----
    Result<ExecutionReport> run_external_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail, bool merge_stderr) {
        const std::size_t n = specs.size();
        std::vector<std::array<int, 2>> pipes(n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (::pipe(pipes[i].data()) < 0) {
                return Diagnostic {ErrorCode::io_error, "pipeline pipe creation failed", {}};
            }
        }

        bool has_deadline = specs.front().timeout && specs.front().timeout->duration.count() > 0;
        auto deadline = has_deadline ? std::chrono::steady_clock::now() + specs.front().timeout->duration : std::chrono::steady_clock::time_point {};

        std::vector<posix::Channel> head_channels;
        std::vector<posix::Channel> tail_channels;
        std::vector<std::vector<posix::Channel>> stderr_channels(n);

        if (n > 0) {
            auto head = posix::materialize_channel(specs.front(), RedirectStream::stdin_stream);
            if (!head) {
                return head.error();
            }
            head_channels.push_back(std::move(head).value());
            auto tail = posix::materialize_channel(specs.back(), RedirectStream::stdout_stream);
            if (!tail) {
                return tail.error();
            }
            tail_channels.push_back(std::move(tail).value());
        }
        for (std::size_t i = 0; i < n; ++i) {
            auto errc = posix::materialize_channel(specs[i], RedirectStream::stderr_stream);
            if (!errc) {
                return errc.error();
            }
            stderr_channels[i].push_back(std::move(errc).value());
        }

        std::vector<pid_t> pids(n, -1);
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<std::string> argv_storage = specs[i].argv;
            std::vector<const char*> argv_c;
            argv_c.reserve(argv_storage.size() + 1);
            for (const std::string& arg : argv_storage) {
                argv_c.push_back(arg.c_str());
            }
            argv_c.push_back(nullptr);

            pid_t pid = ::fork();
            if (pid < 0) {
                return Diagnostic {ErrorCode::execution_failed, "pipeline fork failed", {}};
            }
            if (pid == 0) {
                ::setsid();
                posix::apply_limits(specs[i].limits);

                if (i == 0 && !head_channels.empty() && head_channels.front().dup_fd >= 0) {
                    ::dup2(head_channels.front().dup_fd, 0);
                } else if (i > 0) {
                    ::dup2(pipes[i - 1][0], 0);
                }
                if (i + 1 == n && !tail_channels.empty()) {
                    if (tail_channels.front().pipe_write >= 0) {
                        ::dup2(tail_channels.front().pipe_write, 1);
                    } else if (tail_channels.front().dup_fd >= 0) {
                        ::dup2(tail_channels.front().dup_fd, 1);
                    }
                } else {
                    ::dup2(pipes[i][1], 1);
                }
                if (!stderr_channels[i].empty()) {
                    posix::Channel& errc = stderr_channels[i].front();
                    if (errc.pipe_write >= 0) {
                        ::dup2(errc.pipe_write, 2);
                    } else if (errc.dup_fd >= 0) {
                        ::dup2(errc.dup_fd, 2);
                    }
                }
                if (merge_stderr) {
                    ::dup2(1, 2);
                }

                for (std::size_t k = 0; k + 1 < n; ++k) {
                    ::close(pipes[k][0]);
                    ::close(pipes[k][1]);
                }
                if (!head_channels.empty() && head_channels.front().dup_fd > 2) {
                    ::close(head_channels.front().dup_fd);
                }
                if (!tail_channels.empty() && tail_channels.front().dup_fd > 2) {
                    ::close(tail_channels.front().dup_fd);
                }
                if (!stderr_channels[i].empty() && stderr_channels[i].front().dup_fd > 2) {
                    ::close(stderr_channels[i].front().dup_fd);
                }

                if (specs[i].cwd && !specs[i].cwd->empty() && ::chdir(specs[i].cwd->c_str()) != 0) {
                    std::_Exit(126);
                }
                std::vector<std::string> env_storage;
                std::vector<const char*> env_c;
                posix::build_child_environment(specs[i].environment, env_storage, env_c);
                ::execvp(argv_c[0], const_cast<char* const*>(argv_c.data()));
                std::_Exit(127);
            }
            pids[i] = pid;
        }

        // Parent closes all pipe ends and child-bound fds.
        for (std::size_t k = 0; k + 1 < n; ++k) {
            ::close(pipes[k][0]);
            ::close(pipes[k][1]);
        }
        if (!head_channels.empty() && head_channels.front().dup_fd > 2) {
            ::close(head_channels.front().dup_fd);
        }
        if (!tail_channels.empty() && tail_channels.front().dup_fd > 2) {
            ::close(tail_channels.front().dup_fd);
        }
        // The parent must release its copy of every pump-pipe write end so the
        // drain pump sees EOF once the child exits; otherwise a captured
        // stdout/stderr blocks forever (the child holds the only remaining
        // writer via its dup2'd fd).
        if (!tail_channels.empty() && tail_channels.front().pipe_write >= 0) {
            ::close(tail_channels.front().pipe_write);
            tail_channels.front().pipe_write = -1;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!stderr_channels[i].empty() && stderr_channels[i].front().dup_fd > 2) {
                ::close(stderr_channels[i].front().dup_fd);
            }
            if (!stderr_channels[i].empty() && stderr_channels[i].front().pipe_write >= 0) {
                ::close(stderr_channels[i].front().pipe_write);
                stderr_channels[i].front().pipe_write = -1;
            }
        }

        // Pump terminal memory/sinklet stdout and per-stage stderr captures.
        posix::Channels tail_array {};
        if (!tail_channels.empty()) {
            tail_array[1] = std::move(tail_channels.front());
        }
        bool timed_out = posix::pump_channels(tail_array, has_deadline, deadline, cancel_token_);
        for (std::size_t i = 0; i < n; ++i) {
            posix::Channels err_array {};
            if (!stderr_channels[i].empty()) {
                err_array[2] = stderr_channels[i].front();
            }
            (void)posix::pump_channels(err_array, false, {}, cancel_token_);
        }
        if (timed_out) {
            for (pid_t pid : pids) {
                if (pid > 0) {
                    ::kill(pid, SIGKILL);
                }
            }
        }

        ExecutionReport report;
        report.pipeline_statuses.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            int wstatus = 0;
            for (;;) {
                pid_t waited = ::waitpid(pids[i], &wstatus, 0);
                if (waited == pids[i] || waited < 0) {
                    break;
                }
            }
            report.pipeline_statuses.push_back(posix::decode_status(wstatus, timed_out, cancel_token_ && cancel_token_->load()));
        }
        report.status = detail::select_pipeline_status(report.pipeline_statuses, pipefail);
        return report;
    }

    // ---- buffered fallback for mixed builtin/kernel pipelines ----
    Result<ExecutionReport> run_buffered_pipeline(std::vector<ExecSpec> specs, PipefailPolicy pipefail) {
        ExecutionReport report;
        report.pipeline_statuses.reserve(specs.size());
        std::string stdin_payload;
        for (std::size_t i = 0; i < specs.size(); ++i) {
            ExecSpec spec = specs[i];
            auto capture = std::make_shared<MemoryWriter>();
            bool last = i + 1 == specs.size();
            if (!last) {
                StdioTarget target;
                target.kind = StdioTargetKind::memory;
                target.memory = capture;
                spec.redirections.push_back(Redirection {.stream = RedirectStream::stdout_stream, .mode = RedirectMode::truncate, .target = std::move(target)});
            }
            std::optional<std::filesystem::path> handoff;
            if (i > 0) {
                auto created = write_temp_handoff(spec, stdin_payload);
                if (!created) {
                    return created.error();
                }
                handoff = std::move(created).value();
            }
            auto sub = run(spec);
            if (handoff) {
                std::error_code ec;
                std::filesystem::remove(*handoff, ec);
            }
            if (!sub) {
                return sub.error();
            }
            report.pipeline_statuses.push_back(sub.value().status);
            if (!last) {
                stdin_payload = capture->bytes();
            }
        }
        report.status = detail::select_pipeline_status(report.pipeline_statuses, pipefail);
        return report;
    }

    static Result<std::filesystem::path> write_temp_handoff(ExecSpec& spec, std::string_view payload) {
        char path[] = "/tmp/libsh.XXXXXX";
        int fd = ::mkstemp(path);
        if (fd < 0) {
            return Diagnostic {ErrorCode::io_error, "pipeline handoff creation failed", {}};
        }
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const ssize_t written = ::write(fd, payload.data() + offset, payload.size() - offset);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(fd);
                ::unlink(path);
                return Diagnostic {ErrorCode::io_error, "pipeline handoff write failed", path};
            }
            offset += static_cast<std::size_t>(written);
        }
        ::close(fd);
        StdioTarget target;
        target.kind = StdioTargetKind::file;
        target.file = path;
        spec.redirections.push_back(Redirection {.stream = RedirectStream::stdin_stream, .mode = RedirectMode::read, .target = std::move(target)});
        return std::filesystem::path(path);
    }

    // RAII holder for builtin/kernel Writers (keeps ofstreams alive).
    class BoundWriters {
    public:
        std::shared_ptr<Writer> writer_for(const ExecSpec& spec, RedirectStream stream) {
            const Redirection* redirection = posix::redirection_for(spec.redirections, stream);
            StdioTargetKind kind = redirection ? redirection->target.kind : StdioTargetKind::inherit;
            std::shared_ptr<Writer> writer;
            switch (kind) {
            case StdioTargetKind::memory:
                writer = redirection->target.memory;
                break;
            case StdioTargetKind::sinklet:
                writer = std::make_shared<SinkletWriter>(redirection->target.sinklet);
                break;
            case StdioTargetKind::null_device:
                writer = std::make_shared<NullWriter>();
                break;
            case StdioTargetKind::fd:
                writer = std::make_shared<FdWriter>(*redirection->target.fd);
                break;
            case StdioTargetKind::file: {
                auto ofs = std::make_shared<std::ofstream>();
                std::ios_base::openmode mode = std::ios_base::out;
                if (redirection->mode == RedirectMode::append) {
                    mode |= std::ios_base::app;
                }
                ofs->open(*redirection->target.file, mode);
                writer = std::make_shared<OstreamWriter>(*ofs);
                streams_.push_back(std::move(ofs));
                break;
            }
            case StdioTargetKind::inherit:
            case StdioTargetKind::pipe:
            default:
                writer = std::make_shared<FdWriter>(stream == RedirectStream::stdin_stream ? 0 : (stream == RedirectStream::stderr_stream ? 2 : 1));
                break;
            }
            writers_.push_back(writer);
            return writer;
        }
        void close() {
            for (const auto& writer : writers_) {
                if (writer) {
                    (void)writer->close();
                }
            }
            writers_.clear();
            streams_.clear();
        }

    private:
        // File streams kept alive here for the duration of the call; memory
        // writers are owned by their redirection target.
        std::vector<std::shared_ptr<std::ofstream>> streams_;
        std::vector<std::shared_ptr<Writer>> writers_;
    };

    std::shared_ptr<BuiltinRegistry> builtins_;
    std::shared_ptr<std::atomic<bool>> cancel_token_;
    Environment* env_ {nullptr};
    std::filesystem::path* cwd_ {nullptr};
    bool env_seeded_ {false};
};

} // namespace lsh
