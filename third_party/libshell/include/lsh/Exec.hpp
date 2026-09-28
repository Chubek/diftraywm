#pragma once

// Layer 4 -- execution.
//
// Executor is the single boundary between the shell and the outside world.
// Everything platform-specific (fork/exec, pipe wiring, process groups, signals,
// rlimits) lives behind this interface; the Shell above it reasons only in terms
// of ExecSpec and ExecutionReport.

#include "Kernel.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lsh {

class Shell;

struct ExecutionReport {
    ExitStatus status;
    std::optional<int> pid;
    std::vector<ExitStatus> pipeline_statuses;
    std::optional<std::uint64_t> bytes_stdout;
    std::optional<std::uint64_t> bytes_stderr;
    std::vector<Diagnostic> diagnostics;
    // Set when a compound construct signalled a non-local transfer. The
    // enclosing loop or function consumes and clears it.
    ControlSignal signal {ControlSignal::none};
    // Status carried by `return`; meaningful only when signal == return_.
    int return_code {0};
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
    // Positional parameters for this invocation ($1, $2, ...).
    std::vector<std::string> positional;
    std::shared_ptr<kernel::Registry> kernels;
    std::shared_ptr<kernel::Kernel> resolved_kernel;
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

    // The Shell that owns this executor, for executors whose builtins need to
    // re-enter the runtime.
    virtual void bind_shell(Shell* /*shell*/) {}

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
        report.status = select_pipeline_status(report.pipeline_statuses, pipefail);
        (void)merge_stderr;
        return report;
    }

    // Cooperative cancellation for in-flight children.
    virtual void cancel() {}

    [[nodiscard]] static ExitStatus select_pipeline_status(const std::vector<ExitStatus>& statuses, PipefailPolicy policy) {
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
};

namespace detail {

[[nodiscard]] inline ExitStatus select_pipeline_status(const std::vector<ExitStatus>& statuses, PipefailPolicy policy) {
    return Executor::select_pipeline_status(statuses, policy);
}

} // namespace detail

// Records specs without executing anything. Used for plan inspection, dry runs,
// and to prove that graph construction never reaches the process layer.
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
        report.status = select_pipeline_status(report.pipeline_statuses, pipefail);
        return report;
    }

private:
    std::vector<ExecSpec> specs_;
};

} // namespace lsh
