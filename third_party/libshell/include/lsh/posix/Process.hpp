#pragma once

// Layer 7a -- POSIX process primitives.
//
// Everything that touches the process model lives here: channel materialization
// (files, fds, /dev/null, pipes, in-memory capture), fork/exec, the pump that
// drains captured streams, and bounded reaping. Nothing above this header knows
// about pids or descriptors.

#include "../Exec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <fcntl.h>
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

namespace lsh::posix {

// POSIX defines no standard header for the process environment vector; it is
// exposed by <unistd.h> only under extension feature macros. Declare it
// explicitly so importing the inherited environment does not depend on a
// specific feature-test setting.
extern "C" char** environ;

// ---- streams ---------------------------------------------------------------

// Writer over a raw file descriptor. Used to back inherited streams (fd 1/2)
// so in-process builtins and kernels emit to the real terminal, and so
// `>&3`-style duplication writes through the right descriptor.
class FdWriter final : public Writer {
public:
    explicit FdWriter(int fd, bool own = false) : fd_(fd), own_(own) {}
    ~FdWriter() override {
        if (own_ && fd_ >= 0) {
            ::close(fd_);
        }
    }
    FdWriter(const FdWriter&) = delete;
    FdWriter& operator=(const FdWriter&) = delete;

    Result<void> write(std::string_view bytes) override {
        const char* data = bytes.data();
        std::size_t remaining = bytes.size();
        while (remaining > 0) {
            const ssize_t written = ::write(fd_, data, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return failure(ErrorCode::io_error, std::string("fd write failed: ") + std::strerror(errno));
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

// Reader over a raw file descriptor; backs `< file` and `>&` for in-process
// commands, which previously had no way to observe stdin at all.
class FdReader final : public Reader {
public:
    explicit FdReader(int fd, bool own = false) : fd_(fd), own_(own) {}
    ~FdReader() override {
        if (own_ && fd_ >= 0) {
            ::close(fd_);
        }
    }
    FdReader(const FdReader&) = delete;
    FdReader& operator=(const FdReader&) = delete;

    Result<std::size_t> read(std::string& out, std::size_t max) override {
        for (;;) {
            const ssize_t count = ::read(fd_, buffer_.data(), std::min(max, buffer_.size()));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return failure(ErrorCode::io_error, std::string("fd read failed: ") + std::strerror(errno));
            }
            out.assign(buffer_.data(), static_cast<std::size_t>(count));
            return static_cast<std::size_t>(count);
        }
    }

private:
    int fd_;
    bool own_;
    std::array<char, 8192> buffer_ {};
};

// Discards all output (>/dev/null equivalent for in-process writers).
class NullWriter final : public Writer {
public:
    Result<void> write(std::string_view /*bytes*/) override { return {}; }
};

class NullReaderFd final : public Reader {
public:
    Result<std::size_t> read(std::string& out, std::size_t) override {
        out.clear();
        return std::size_t {0};
    }
};

// RAII holder for an anonymous temporary file. tmpfile() creates a file that
// is unlinked immediately, so a here-document or a pipeline hand-off payload
// needs no cleanup path and cannot be observed by another process.
class TempFile {
public:
    TempFile() = default;
    ~TempFile() {
        if (file_) {
            std::fclose(file_);
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept : file_(other.file_) { other.file_ = nullptr; }
    TempFile& operator=(TempFile&& other) noexcept {
        if (this != &other) {
            if (file_) {
                std::fclose(file_);
            }
            file_ = other.file_;
            other.file_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] static Result<TempFile> create() {
        TempFile handle;
        std::FILE* file = std::tmpfile();
        if (file == nullptr) {
            return failure(ErrorCode::io_error, "cannot create a temporary file");
        }
        handle.file_ = file;
        return handle;
    }

    [[nodiscard]] static Result<TempFile> from_text(std::string_view text) {
        auto handle = create();
        if (!handle) {
            return handle.error();
        }
        if (auto written = handle.value().write(text); !written) {
            return written.error();
        }
        if (auto rewound = handle.value().rewind(); !rewound) {
            return rewound.error();
        }
        return handle;
    }

    Result<void> write(std::string_view text) {
        const int fd = this->fd();
        const char* data = text.data();
        std::size_t remaining = text.size();
        while (remaining > 0) {
            const ssize_t written = ::write(fd, data, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return failure(ErrorCode::io_error, "cannot write a temporary file");
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        return {};
    }

    Result<void> rewind() {
        if (::lseek(fd(), 0, SEEK_SET) < 0) {
            return failure(ErrorCode::io_error, "cannot rewind a temporary file");
        }
        return {};
    }

    [[nodiscard]] int fd() const noexcept { return file_ ? ::fileno(file_) : -1; }

private:
    std::FILE* file_ {nullptr};
};

// ---- channel materialization ----------------------------------------------

// One materialized stream for an external process. Either a concrete fd to
// dup2 onto the stream (file/null/fd/inherit/close), or a pump pipe whose read
// end the parent drains into `sink` (memory/sinklet output targets, or a Reader
// fed to stdin).
struct Channel {
    int dup_fd {-1};      // fd to dup2 in the child; -1 means inherit
    int pipe_write {-1};  // child writes here when pumping output
    int pipe_read {-1};   // parent reads here when pumping
    bool close_stream {false}; // `>&-`: detach the stream in the child
    std::shared_ptr<Writer> sink;
    RedirectMode mode {RedirectMode::truncate};
};

[[nodiscard]] inline int open_flags_for(RedirectMode mode, RedirectStream stream) {
    if (stream == RedirectStream::stdin_stream || mode == RedirectMode::read) {
        return O_RDONLY;
    }
    if (mode == RedirectMode::read_write) {
        return O_RDWR | O_CREAT;
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
[[nodiscard]] inline const Redirection* redirection_for(const std::vector<Redirection>& redirections, RedirectStream stream) {
    const Redirection* found = nullptr;
    for (const Redirection& redirection : redirections) {
        if (redirection.stream == stream) {
            found = &redirection;
        }
    }
    return found;
}

// Materializes the descriptor one of a process's three standard streams will
// be dup2'd from. `owned` keeps temporary files alive until after the fork, so
// a here-document or here-string payload survives into the child image.
inline Result<Channel> materialize_channel(
    const ExecSpec& spec, RedirectStream stream, std::vector<std::shared_ptr<TempFile>>& owned) {
    Channel channel;
    const Redirection* redirection = redirection_for(spec.redirections, stream);
    StdioTargetKind kind = redirection ? redirection->target.kind : StdioTargetKind::inherit;
    channel.mode = redirection ? redirection->mode : RedirectMode::truncate;

    if (redirection && redirection->mode == RedirectMode::close) {
        channel.close_stream = true;
        channel.dup_fd = -1;
        return channel;
    }

    switch (kind) {
    case StdioTargetKind::inherit:
    case StdioTargetKind::pipe:
        channel.dup_fd = -1; // child keeps its inherited fd
        break;
    case StdioTargetKind::deferred:
    case StdioTargetKind::here_string:
        // The Shell resolves both before the spec reaches an executor; either
        // arriving here means that step was skipped, and inheriting is the safe
        // reading.
        channel.dup_fd = -1;
        break;
    case StdioTargetKind::closed:
        channel.close_stream = true;
        break;
    case StdioTargetKind::null_device:
        channel.dup_fd = ::open("/dev/null", stream == RedirectStream::stdin_stream ? O_RDONLY : O_WRONLY);
        if (channel.dup_fd < 0) {
            return failure(ErrorCode::io_error, "cannot open /dev/null");
        }
        break;
    case StdioTargetKind::file: {
        const std::string& path = *redirection->target.file;
        channel.dup_fd = ::open(path.c_str(), open_flags_for(channel.mode, stream) | O_CLOEXEC, 0666);
        if (channel.dup_fd < 0) {
            return failure(ErrorCode::io_error, std::string("cannot open redirection target: ") + std::strerror(errno), path);
        }
        break;
    }
    case StdioTargetKind::fd:
        channel.dup_fd = *redirection->target.fd;
        break;
    case StdioTargetKind::memory:
    case StdioTargetKind::sinklet:
        if (stream == RedirectStream::stdin_stream) {
            // A here-document, here-string, or in-memory hand-off is a Reader,
            // not a descriptor: spill it to an anonymous temporary file the
            // child can inherit. Without this, in-process commands and children
            // received an empty stdin.
            if (redirection->target.input) {
                auto temp = TempFile::create();
                if (!temp) {
                    return temp.error();
                }
                std::string chunk;
                for (;;) {
                    auto count = redirection->target.input->read(chunk, 8192);
                    if (!count) {
                        return count.error();
                    }
                    if (count.value() == 0) {
                        break;
                    }
                    if (auto written = temp.value().write(chunk); !written) {
                        return written.error();
                    }
                }
                if (auto rewound = temp.value().rewind(); !rewound) {
                    return rewound.error();
                }
                owned.push_back(std::make_shared<TempFile>(std::move(temp).value()));
                channel.dup_fd = ::dup(owned.back()->fd());
                if (channel.dup_fd < 0) {
                    return failure(ErrorCode::io_error, "cannot duplicate redirected input");
                }
                break;
            }
            channel.dup_fd = ::open("/dev/null", O_RDONLY);
            if (channel.dup_fd < 0) {
                return failure(ErrorCode::io_error, "cannot open /dev/null");
            }
            break;
        }
        {
            int fds[2] = {-1, -1};
            if (::pipe(fds) < 0) {
                return failure(ErrorCode::io_error, "cannot create a capture pipe");
            }
            ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
            channel.pipe_write = fds[1];
            channel.pipe_read = fds[0];
            if (kind == StdioTargetKind::memory) {
                channel.sink = redirection->target.memory;
            } else {
                channel.sink = std::make_shared<SinkletWriter>(redirection->target.sinklet);
            }
        }
        break;
    }
    return channel;
}

using Channels = std::array<Channel, 3>;

inline Result<Channels> materialize_channels(const ExecSpec& spec, std::vector<std::shared_ptr<TempFile>>& owned) {
    Channels channels {};
    for (std::size_t index = 0; index < 3; ++index) {
        const RedirectStream stream = index == 0   ? RedirectStream::stdin_stream
            : index == 1                        ? RedirectStream::stdout_stream
                                               : RedirectStream::stderr_stream;
        auto channel = materialize_channel(spec, stream, owned);
        if (!channel) {
            return channel.error();
        }
        channels[index] = std::move(channel).value();
    }
    return channels;
}

// Descriptor a redirection ultimately writes to. `3>file` targets descriptor 3;
// an absent source descriptor defaults to the stream's own number.
[[nodiscard]] inline int target_descriptor(const Redirection& redirection) {
    return redirection.source_fd.value_or(static_cast<int>(*stream_index(redirection.stream)));
}

inline void child_apply_streams(Channels& channels, bool merge_stderr, const std::vector<Redirection>& redirections) {
    for (int stream = 0; stream < 3; ++stream) {
        Channel& channel = channels[static_cast<std::size_t>(stream)];
        const Redirection* redirection = redirection_for(redirections, static_cast<RedirectStream>(stream));
        const int descriptor = redirection != nullptr ? target_descriptor(*redirection) : stream;
        if (channel.close_stream) {
            ::close(descriptor);
            continue;
        }
        if (channel.pipe_write >= 0) {
            ::dup2(channel.pipe_write, descriptor);
        } else if (channel.dup_fd >= 0) {
            ::dup2(channel.dup_fd, descriptor);
        }
    }
    if (merge_stderr) {
        ::dup2(1, 2);
    }
    // Close materialized originals and pipe ends so they do not leak into exec.
    for (int stream = 0; stream < 3; ++stream) {
        Channel& channel = channels[static_cast<std::size_t>(stream)];
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

// Build a NULL-terminated key=value vector from the exported environment table
// and point `environ` at it. Called in a forked child immediately before
// execvp; execvp passes `environ` to execve. `storage` and `c_ptrs` must outlive
// the call (they live on the child stack until the image is replaced).
inline void build_child_environment(const std::vector<EnvVar>& environment, std::vector<std::string>& storage, std::vector<const char*>& c_ptrs) {
    storage.clear();
    storage.reserve(environment.size());
    for (const EnvVar& entry : environment) {
        if (entry.key.empty() || entry.key.find('=') != std::string::npos) {
            continue;
        }
        storage.push_back(entry.key + "=" + entry.value);
    }
    c_ptrs.clear();
    c_ptrs.reserve(storage.size() + 1);
    for (const std::string& value : storage) {
        c_ptrs.push_back(value.c_str());
    }
    c_ptrs.push_back(nullptr);
    ::environ = const_cast<char**>(c_ptrs.data());
}

// Parent closes its copies of child-bound fds; keeps pump read ends.
inline void parent_close_child_ends(Channels& channels) {
    for (Channel& channel : channels) {
        if (channel.dup_fd > 2) {
            ::close(channel.dup_fd);
            channel.dup_fd = -1;
        }
        if (channel.pipe_write >= 0) {
            ::close(channel.pipe_write);
            channel.pipe_write = -1;
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

// A child must start from default signal semantics. The parent shell may have
// ignored SIGPIPE (a common library posture) and the interactive front end may
// have installed handlers; neither should leak into the program being run.
inline void child_reset_signals() {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGINT, SIGQUIT, SIGPIPE, SIGHUP, SIGTERM}) {
        ::sigaction(sig, &action, nullptr);
    }
    ::signal(SIGPIPE, SIG_DFL);
}

[[nodiscard]] inline ExitStatus decode_status(int wstatus, bool timed_out, bool canceled) {
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
        const int sig = WTERMSIG(wstatus);
        status.signaled = true;
        status.signal = sig;
        status.code = 128 + sig;
    }
    return status;
}

// ---- pumping ---------------------------------------------------------------

struct PumpTarget {
    int fd {-1};
    Writer* writer {nullptr};
    std::uint64_t* counter {nullptr};
};

enum class PumpOutcome {
    completed,     // every target reached EOF
    deadline,      // the wall-clock deadline expired
    canceled,      // the cancel token fired
};

// Drains every target concurrently until all of them reach EOF, the deadline
// expires, or cancellation is requested. Draining serially deadlocks whenever a
// stage fills a pipe the pump is not currently reading, which is the common case
// for a pipeline that captures both stdout and stderr.
inline PumpOutcome pump_targets(
    std::vector<PumpTarget>& targets,
    bool has_deadline,
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<std::atomic<bool>>& cancel) {
    char buffer[8192];
    for (;;) {
        if (cancel && cancel->load()) {
            return PumpOutcome::canceled;
        }

        if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
            return PumpOutcome::deadline;
        }

        if (targets.empty()) {
            return PumpOutcome::completed;
        }

        std::vector<pollfd> fds(targets.size());
        for (std::size_t index = 0; index < targets.size(); ++index) {
            fds[index].fd = targets[index].fd;
            fds[index].events = POLLIN;
            fds[index].revents = 0;
        }

        int timeout = -1;
        if (has_deadline) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            timeout = remaining > 0 ? static_cast<int>(remaining) : 0;
        }

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PumpOutcome::completed;
        }
        if (ready == 0) {
            return PumpOutcome::deadline;
        }

        for (std::size_t index = 0; index < fds.size(); ++index) {
            if ((fds[index].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            const ssize_t count = ::read(fds[index].fd, buffer, sizeof(buffer));
            if (count > 0) {
                if (targets[index].writer) {
                    (void)targets[index].writer->write(std::string_view(buffer, static_cast<std::size_t>(count)));
                }
                if (targets[index].counter) {
                    *targets[index].counter += static_cast<std::uint64_t>(count);
                }
                continue;
            }
            // EOF or error: retire the target.
            ::close(fds[index].fd);
            targets[index] = targets.back();
            targets.pop_back();
        }
    }
}

// Convenience wrapper: every pump in a Channels array.
inline PumpOutcome pump_channels(
    Channels& channels,
    bool has_deadline,
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<std::atomic<bool>>& cancel,
    std::uint64_t* stdout_bytes = nullptr,
    std::uint64_t* stderr_bytes = nullptr) {
    std::vector<PumpTarget> targets;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (channels[index].pipe_read < 0) {
            continue;
        }
        PumpTarget target;
        target.fd = channels[index].pipe_read;
        target.writer = channels[index].sink.get();
        if (index == 1) {
            target.counter = stdout_bytes;
        } else if (index == 2) {
            target.counter = stderr_bytes;
        }
        targets.push_back(target);
    }
    return pump_targets(targets, has_deadline, deadline, cancel);
}

// ---- reaping ---------------------------------------------------------------

// Reap a child, honoring an optional wall-clock deadline and a cancel token.
// Reports which bound fired so the caller can distinguish a timeout from a
// cancellation instead of reporting both as a timeout.
inline ExitStatus reap(
    pid_t pid,
    bool has_deadline,
    std::chrono::steady_clock::time_point deadline,
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
        const pid_t waited = ::waitpid(pid, &wstatus, WNOHANG);
        if (waited == pid) {
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return decode_status(wstatus, timed_out, canceled);
}

} // namespace lsh::posix
