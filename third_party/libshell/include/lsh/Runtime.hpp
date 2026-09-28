#pragma once

// Layer 5 -- shell runtime.
//
// The Shell owns execution state: the environment, working directory, shell
// options, positional parameters, function definitions, and the loop/return
// signal stack. It translates IR into ExecSpec and drives an Executor, and it
// is the only layer that may mutate shell state.

#include "Exec.hpp"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace lsh {

class Shell;

struct ShellOptions {
    PipefailPolicy pipefail {PipefailPolicy::last};
    bool trace {false};
    bool sandboxed {false};
    std::optional<Timeout> timeout;
    std::optional<ResourceLimits> limits;
    // `set -e`: abort the enclosing list when a command fails.
    bool errexit {false};
    // `set -u`: referencing an unset variable is an error.
    bool nounset {false};
    // `set -x`: emit the expanded command line before execution.
    bool xtrace {false};
    // Shell name reported by $0.
    std::string name {"libsh"};
};

// A shell function. The body is IR rather than a native callback so it is
// validated, inspectable, and expanded by exactly the same rules as any other
// command. Binding is by shared_ptr so a function may capture its defining
// shell.
class ShellFunction {
public:
    ShellFunction(std::string name, ir::NodePtr body, std::shared_ptr<Shell> owner = {})
        : name_(std::move(name)), body_(std::move(body)), owner_(std::move(owner)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const ir::NodePtr& body() const noexcept { return body_; }
    [[nodiscard]] const std::shared_ptr<Shell>& owner() const noexcept { return owner_; }

private:
    std::string name_;
    ir::NodePtr body_;
    std::shared_ptr<Shell> owner_;
};

// Concrete ScriptingBackend wired to a Shell. Command substitution re-enters
// the runtime. Lua evaluation is delegated to a separate evaluator so this
// header stays free of any embedded-language dependency; see
// LibShell-Scripting.hpp for the QaMRpp binding.
class ShellScriptingBackend final : public ScriptingBackend {
public:
    using ParseHook = std::function<Result<ir::Program>(std::string_view)>;
    using LuaHook = std::function<Result<std::string>(std::string_view, const Environment&)>;

    explicit ShellScriptingBackend(Shell* shell) : shell_(shell) {}

    void set_parse_hook(ParseHook hook) { parse_hook_ = std::move(hook); }
    void set_lua_hook(LuaHook hook) { lua_hook_ = std::move(hook); }

    [[nodiscard]] const ParseHook& parse_hook() const noexcept { return parse_hook_; }

    Result<std::string> eval_command(std::string_view script, const Environment& environment) override;
    Result<std::string> eval_lua(std::string_view script, const Environment& environment) override;

private:
    Shell* shell_;
    ParseHook parse_hook_;
    LuaHook lua_hook_;
};

class Shell {
public:
    Shell() : Shell(std::make_shared<DryRunExecutor>(), std::make_shared<Expander>()) {}

    explicit Shell(std::shared_ptr<Executor> executor, std::shared_ptr<Expander> expander = std::make_shared<Expander>())
        : executor_(std::move(executor)), expander_(std::move(expander)) {
        install_runtime();
        // PWD and IFS are shell-defined, not inherited: they must be correct in
        // a shell that never touched the process environment.
        if (!environment_.contains("PWD")) {
            environment_.set("PWD", cwd_.string(), /*exported=*/true);
        }
        if (!environment_.contains("IFS")) {
            environment_.set("IFS", " \t\n", /*exported=*/false);
        }
    }

    [[nodiscard]] Environment& env() noexcept { return environment_; }
    [[nodiscard]] const Environment& env() const noexcept { return environment_; }

    [[nodiscard]] const std::filesystem::path& cwd() const noexcept { return cwd_; }

    // Changing the working directory keeps $PWD and $OLDPWD coherent, so
    // `~+`, `~-`, and any reader of $PWD observe the same state as `pwd`.
    void set_cwd(std::filesystem::path cwd) {
        const std::string previous = environment_.contains("PWD") ? environment_.get("PWD").value_or(std::string {}) : std::string {};
        cwd_ = std::move(cwd);
        environment_.set("PWD", cwd_.string(), /*exported=*/true);
        if (!previous.empty() && previous != cwd_.string()) {
            environment_.set("OLDPWD", previous, /*exported=*/true);
        }
    }

    [[nodiscard]] ShellOptions& options() noexcept { return options_; }
    [[nodiscard]] const ShellOptions& options() const noexcept { return options_; }

    [[nodiscard]] std::shared_ptr<kernel::Registry> kernels() const noexcept { return kernels_; }
    void set_kernels(std::shared_ptr<kernel::Registry> kernels) { kernels_ = std::move(kernels); }

    [[nodiscard]] std::shared_ptr<Expander> expander() const noexcept { return expander_; }
    void set_expander(std::shared_ptr<Expander> expander) {
        expander_ = std::move(expander);
        if (scripting_) {
            expander_->set_scripting(scripting_);
        }
    }

    [[nodiscard]] std::shared_ptr<Executor> executor() const noexcept { return executor_; }
    void set_executor(std::shared_ptr<Executor> executor) {
        executor_ = std::move(executor);
        executor_->bind_runtime(&environment_, &cwd_);
        executor_->bind_shell(this);
    }

    // Tilde expansion policy. Defaults to $HOME/$PWD/$OLDPWD; a platform
    // resolver (see LibShell-Posix.hpp) additionally resolves `~user`.
    void set_tilde_resolver(std::shared_ptr<detail::TildeResolver> resolver) { tilde_ = std::move(resolver); }
    [[nodiscard]] const std::shared_ptr<detail::TildeResolver>& tilde_resolver() const noexcept { return tilde_; }

    // Wire a full grammar parser into command substitution ($(...)). Without it,
    // substitution bodies are whitespace-split into a single command.
    void set_command_substitution_parser(ShellScriptingBackend::ParseHook hook) {
        if (scripting_) {
            scripting_->set_parse_hook(std::move(hook));
        }
    }

    // Wire the embedded-Lua evaluator. Without it, $(lua ...) reports a
    // diagnostic instead of silently expanding to nothing.
    void set_lua_evaluator(ShellScriptingBackend::LuaHook hook) {
        if (scripting_) {
            scripting_->set_lua_hook(std::move(hook));
        }
    }

    [[nodiscard]] ShellScriptingBackend* scripting() const noexcept { return scripting_.get(); }

    // Positional parameters ($1 .. $9, $@, $*, $#).
    void set_positional(std::vector<std::string> positional) { positional_ = std::move(positional); }
    [[nodiscard]] const std::vector<std::string>& positional() const noexcept { return positional_; }

    // Status of the most recently completed command, as reported by $?.
    [[nodiscard]] int last_status() const noexcept { return last_status_.code; }
    [[nodiscard]] const ExitStatus& last_exit_status() const noexcept { return last_status_; }

    // Shell functions.
    void define_function(std::string name, ir::NodePtr body);
    [[nodiscard]] std::shared_ptr<ShellFunction> find_function(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> function_names() const;

    // Non-local transfer raised by the most recent run. The Shell consumes it so
    // a break inside a pipeline stage does not leak past its loop.
    [[nodiscard]] ControlSignal pending_signal() const noexcept { return pending_signal_; }
    void clear_signal() noexcept { pending_signal_ = ControlSignal::none; }

    // Non-const: the Expander needs a mutable Environment so ${x:=word} can
    // write back. The pointer is borrowed, never retained.
    [[nodiscard]] ExpandContext context() noexcept {
        ExpandContext context;
        context.environment = &environment_;
        context.positional = &positional_;
        context.shell_name = options_.name;
        context.last_status = last_status_.code;
        context.last_background_pid = last_background_pid_;
        context.shell_pid = static_cast<int>(::getpid());
        context.cwd = &cwd_;
        context.tilde = tilde_;
        context.scripting = scripting_;
        return context;
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

    // Applies a `return`/`break`/`continue` to the current shell level.
    void set_return_code(int code) noexcept { return_code_ = code; }
    [[nodiscard]] int return_code() const noexcept { return return_code_; }
    void set_background_pid(int pid) noexcept { last_background_pid_ = pid; }

    // xtrace output goes to fd 2 without depending on the platform executor.
    static void trace_line(const std::vector<std::string>& argv) {
        std::string line = "+";
        for (const std::string& arg : argv) {
            line.push_back(' ');
            line += arg;
        }
        line.push_back('\n');
        (void)::write(2, line.data(), line.size());
    }

    // Wire command-substitution and shell-state binding into the executor. The
    // scripting backend borrows this Shell; it is owned here and dies with it.
    void install_runtime() {
        scripting_ = std::make_shared<ShellScriptingBackend>(this);
        expander_->set_scripting(scripting_);
        executor_->bind_runtime(&environment_, &cwd_);
        executor_->bind_shell(this);
        tilde_ = std::make_shared<detail::EnvironmentTildeResolver>(environment_);
    }

    Result<ExecutionReport> run_node(const ir::NodePtr& node) {
        if (!node) {
            return failure(ErrorCode::invalid_graph, "null IR node");
        }
        auto report = std::visit(
            [&](const auto& node_value) -> Result<ExecutionReport> {
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
                } else if constexpr (std::is_same_v<T, ir::Assignment>) {
                    return run_assignment(node_value);
                } else if constexpr (std::is_same_v<T, ir::BraceGroup>) {
                    return run_with_redirections(node_value.body, node_value.redirections);
                } else if constexpr (std::is_same_v<T, ir::IfClause>) {
                    return run_if(node_value);
                } else if constexpr (std::is_same_v<T, ir::WhileClause>) {
                    return run_while(node_value);
                } else if constexpr (std::is_same_v<T, ir::ForClause>) {
                    return run_for(node_value);
                } else if constexpr (std::is_same_v<T, ir::CaseClause>) {
                    return run_case(node_value);
                } else if constexpr (std::is_same_v<T, ir::FunctionDefinition>) {
                    return run_function_definition(node_value);
                } else if constexpr (std::is_same_v<T, ir::Control>) {
                    return run_control(node_value);
                } else if constexpr (std::is_same_v<T, ir::Negate>) {
                    return run_negate(node_value);
                }
            },
            node->value);
        if (report) {
            commit_status(report.value().status, report.value().signal);
        }
        return report;
    }

    void commit_status(const ExitStatus& status, ControlSignal signal) {
        if (signal == ControlSignal::none) {
            last_status_ = status;
        }
        if (signal == ControlSignal::return_) {
            last_status_.code = return_code_;
        }
        if (signal != ControlSignal::none) {
            pending_signal_ = signal;
        }
    }

    // `set -e` and `set -x` applied uniformly to every completed command.
    void apply_errexit(const ExecutionReport& report) {
        if (options_.errexit && !report.status.success() && report.signal == ControlSignal::none) {
            pending_signal_ = ControlSignal::break_;
        }
    }

    Result<ExecutionReport> run_command(const ir::Command& command, std::vector<Redirection> inherited_redirections) {
        // `FOO=bar` with no command word assigns in the current shell.
        if (command.argv.empty() && !command.assignments.empty()) {
            return run_assignments(command.assignments);
        }
        auto spec = make_exec_spec(command, std::move(inherited_redirections));
        if (!spec) {
            return spec.error();
        }
        if (options_.xtrace) {
            trace_line(spec.value().argv);
        }
        auto report = executor_->run(std::move(spec).value());
        if (report) {
            apply_errexit(report.value());
        }
        return report;
    }

    // `NAME=value` prefixes are expanded here and overlaid onto the child's
    // environment. They never reach the shell's own environment: that only
    // happens for a command consisting solely of assignments.
    Result<std::vector<EnvVar>> expand_assignments(const std::vector<ir::Assignment>& assignments) {
        std::vector<EnvVar> overlay;
        overlay.reserve(assignments.size());
        for (const ir::Assignment& assignment : assignments) {
            auto value = expander_->expand_argv({assignment.value}, context());
            if (!value) {
                return value.error();
            }
            // An assignment whose value expands to zero words still defines the
            // variable, with the empty string.
            std::string joined;
            for (std::size_t index = 0; index < value.value().size(); ++index) {
                if (index > 0) {
                    joined.push_back(' ');
                }
                joined += value.value()[index];
            }
            EnvVar entry;
            entry.key = assignment.name;
            entry.value = std::move(joined);
            // A prefix assignment exports the name for this command only; the
            // shell's own export attribute is restored implicitly because the
            // overlay is discarded afterwards.
            entry.exported = true;
            overlay.push_back(std::move(entry));
        }
        return overlay;
    }

    // A redirection target may hold an unexpanded word. Only this layer can
    // expand it, so `> $file` and `<<< $text` are resolved before the spec
    // reaches an executor: an output target becomes a concrete path, a stdin
    // target becomes an in-memory payload.
    static Result<std::vector<Redirection>> resolve_deferred(
        std::vector<Redirection> redirections, const Expander& expander, const ExpandContext& context) {
        for (Redirection& redirection : redirections) {
            const bool is_here_string = redirection.target.here_string.has_value();
            if (!is_here_string && !redirection.target.deferred) {
                continue;
            }
            const Argument word = is_here_string ? *redirection.target.here_string : *redirection.target.deferred;
            auto expanded = expander.expand_argv({word}, context);
            if (!expanded) {
                return expanded.error();
            }
            if (expanded.value().empty()) {
                return failure(ErrorCode::bad_expansion, "a redirection target expanded to nothing");
            }
            // Several fields is ambiguous for a path and meaningless for a
            // payload; joining keeps the behaviour predictable and matches how
            // the rest of the runtime treats multi-field expansions.
            std::string text = expanded.value().front();
            for (std::size_t index = 1; index < expanded.value().size(); ++index) {
                text.push_back(' ');
                text += expanded.value()[index];
            }

            redirection.target.deferred.reset();
            redirection.target.here_string.reset();
            redirection.target.file.reset();
            if (is_here_string) {
                // A here-string supplies stdin as data, newline-terminated.
                redirection.target.kind = StdioTargetKind::memory;
                redirection.target.input = std::make_shared<MemoryReader>(text + "\n");
            } else {
                redirection.target.kind = StdioTargetKind::file;
                redirection.target.file = std::move(text);
            }
        }
        return redirections;
    }

    // `! pipeline` inverts the pipeline's status and leaves everything else --
    // including any non-local transfer -- untouched.
    Result<ExecutionReport> run_negate(const ir::Negate& negate) {
        auto report = run_node(negate.subject);
        if (!report) {
            return report.error();
        }
        ExecutionReport inverted = report.value();
        if (inverted.signal == ControlSignal::none) {
            inverted.status = exit_status(inverted.status.success() ? 1 : 0);
            inverted.status.signaled = false;
            inverted.status.timed_out = false;
            inverted.status.canceled = false;
        }
        return inverted;
    }

    Result<ExecSpec> make_exec_spec(const ir::Command& command, std::vector<Redirection> inherited_redirections) {
        auto argv = expander_->expand_argv(command.argv, context());
        if (!argv) {
            return argv.error();
        }
        if (argv.value().empty() && !command.argv.empty()) {
            // Every argument was removed by expansion. POSIX removes the whole
            // command rather than running it with an empty argv.
            return failure(ErrorCode::empty_argv, "command expanded to no arguments");
        }

        auto overlay = expand_assignments(command.assignments);
        if (!overlay) {
            return overlay.error();
        }
        std::vector<EnvVar> environment = environment_.exported_entries();
        for (const EnvVar& entry : command.environment_overlay) {
            // A later prefix assignment wins over an exported variable of the
            // same name, matching POSIX overlay semantics.
            auto existing = std::find_if(environment.begin(), environment.end(), [&](const EnvVar& candidate) {
                return candidate.key == entry.key;
            });
            if (existing == environment.end()) {
                environment.push_back(entry);
            } else {
                *existing = entry;
            }
        }
        for (const EnvVar& entry : overlay.value()) {
            auto existing = std::find_if(environment.begin(), environment.end(), [&](const EnvVar& candidate) {
                return candidate.key == entry.key;
            });
            if (existing == environment.end()) {
                environment.push_back(entry);
            } else {
                *existing = entry;
            }
        }

        inherited_redirections.insert(inherited_redirections.end(), command.redirections.begin(), command.redirections.end());
        auto resolved = resolve_deferred(std::move(inherited_redirections), *expander_, context());
        if (!resolved) {
            return resolved.error();
        }
        inherited_redirections = std::move(resolved).value();
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
        // Positional parameters for this invocation. For a function they are
        // the call's operands, which is what makes `$1` inside a function body
        // refer to its first argument rather than the shell's own.
        if (spec.argv.size() > 1) {
            spec.positional.assign(spec.argv.begin() + 1, spec.argv.end());
        }

        // A shell function shadows external resolution for the same name, unless
        // the command was invoked through `command`.
        if (!spec.argv.empty() && !command.skip_functions && command.source != CommandSource::kernel) {
            if (auto function = find_function(spec.argv.front())) {
                spec.source = CommandSource::kernel;
                spec.resolved_kernel = make_function_kernel(function);
            }
        }

        // Resolve auto/kernel sources against the kernel registry so executors
        // receive a bound kernel instance instead of an unhandled source tag.
        if (!spec.resolved_kernel && !spec.argv.empty() && kernels_
            && (command.source == CommandSource::auto_resolve || command.source == CommandSource::kernel)) {
            if (auto resolved = kernels_->find(spec.argv.front())) {
                spec.source = CommandSource::kernel;
                spec.resolved_kernel = resolved;
            } else if (command.source == CommandSource::kernel) {
                return failure(ErrorCode::not_found, "kernel not registered", spec.argv.front());
            }
        }
        return spec;
    }

    // Wraps a shell function body as a kernel so it flows through the ordinary
    // kernel lifecycle (load/initialize/execute/shutdown) rather than needing a
    // parallel execution path.
    std::shared_ptr<kernel::Kernel> make_function_kernel(const std::shared_ptr<ShellFunction>& function) {
        kernel::Metadata metadata;
        metadata.name = function->name();
        metadata.summary = "shell function";
        Shell* self = this;
        // Weak, so a redefinition during the call is observed rather than masked
        // by a kernel that kept the old body alive.
        std::weak_ptr<ShellFunction> weak = function;
        return std::make_shared<kernel::FunctionKernel>(
            std::move(metadata),
            [self, weak](const kernel::Invocation& invocation) -> Result<ExitStatus> {
                auto held = weak.lock();
                if (!held) {
                    return failure(ErrorCode::not_found, "shell function was redefined or removed");
                }
                auto report = self->run_in_function(held.get(), invocation);
                if (!report) {
                    return report.error();
                }
                // A `return` inside the body is consumed here; `break` and
                // `continue` belong to an enclosing loop of the caller, so they
                // travel back on the report.
                if (report.value().signal == ControlSignal::return_) {
                    return Result<ExitStatus> {report.value().status};
                }
                ExecutionReport unwrapped = report.value();
                return Result<ExitStatus> {unwrapped.status};
            });
    }

    Result<ExecutionReport> run_in_function(const ShellFunction* function, const kernel::Invocation& invocation) {
        // A function body observes the caller's positional parameters.
        std::vector<std::string> saved = positional_;
        positional_ = invocation.positional;

        // `return` inside a function stops the function, not the caller.
        int saved_return = return_code_;
        clear_signal();

        auto report = run_node(function->body());
        if (!report) {
            positional_ = std::move(saved);
            return_code_ = saved_return;
            return report.error();
        }

        ExitStatus status = report.value().status;
        ControlSignal signal = report.value().signal;
        if (signal == ControlSignal::return_) {
            status = exit_status(return_code_);
            signal = ControlSignal::none;
            return_code_ = saved_return;
        }

        positional_ = std::move(saved);
        ExecutionReport result;
        result.status = status;
        result.signal = signal;
        return result;
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
        auto report = executor_->run_pipeline(std::move(specs), pipeline.pipefail, pipeline.merge_stderr);
        if (report) {
            apply_errexit(report.value());
        }
        return report;
    }

    Result<ExecutionReport> run_sequence(const ir::Sequence& sequence) {
        auto left = run_node(sequence.left);
        if (!left) {
            return left.error();
        }
        // A non-local transfer from the left operand preempts the connective.
        if (left.value().signal != ControlSignal::none) {
            return left;
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

    Result<ExecutionReport> run_assignments(const std::vector<ir::Assignment>& assignments) {
        ExecutionReport report;
        for (const ir::Assignment& assignment : assignments) {
            auto value = expander_->expand_argv({assignment.value}, context());
            if (!value) {
                return value.error();
            }
            // An assignment whose value expands to zero words still assigns the
            // empty string; joining the fields keeps `X=$EMPTY` defined.
            std::string joined;
            for (std::size_t index = 0; index < value.value().size(); ++index) {
                if (index > 0) {
                    joined.push_back(' ');
                }
                joined += value.value()[index];
            }
            // An assignment without `export` keeps the variable's existing export
            // attribute rather than forcing it one way or the other.
            const bool exported = environment_.is_exported(assignment.name);
            auto result = environment_.assign(assignment.name, joined, exported);
            if (!result) {
                report.status.code = 1;
                report.diagnostics.push_back(result.error());
                return report;
            }
        }
        report.status.code = 0;
        return report;
    }

    Result<ExecutionReport> run_assignment(const ir::Assignment& assignment) {
        return run_assignments({assignment});
    }

    Result<ExecutionReport> run_if(const ir::IfClause& clause) {
        auto condition = run_node(clause.condition);
        if (!condition) {
            return condition.error();
        }
        if (condition.value().signal != ControlSignal::none) {
            return condition;
        }
        if (condition.value().status.success()) {
            return run_node(clause.body);
        }
        if (clause.alternative) {
            return run_node(*clause.alternative);
        }
        ExecutionReport report;
        report.status.code = 0;
        return report;
    }

    Result<ExecutionReport> run_while(const ir::WhileClause& clause) {
        // Loop status is the last body status, or zero when the loop never ran.
        ExecutionReport report;
        report.status.code = 0;
        for (;;) {
            if (pending_signal_ == ControlSignal::break_ || pending_signal_ == ControlSignal::return_) {
                break;
            }
            clear_signal();
            auto condition = run_node(clause.condition);
            if (!condition) {
                return condition.error();
            }
            if (condition.value().signal != ControlSignal::none) {
                return condition;
            }
            const bool proceed = clause.until ? !condition.value().status.success() : condition.value().status.success();
            if (!proceed) {
                break;
            }
            auto body = run_node(clause.body);
            if (!body) {
                return body.error();
            }
            report.status = body.value().status;
            if (body.value().signal == ControlSignal::break_) {
                clear_signal();
                break;
            }
            if (body.value().signal == ControlSignal::continue_) {
                clear_signal();
                continue;
            }
            if (body.value().signal == ControlSignal::return_) {
                return body;
            }
        }
        clear_signal();
        return report;
    }

    Result<ExecutionReport> run_for(const ir::ForClause& clause) {
        std::vector<std::string> words;
        if (clause.words.empty()) {
            // `for x` with no list iterates the positional parameters.
            words = positional_;
        } else {
            auto expanded = expander_->expand_argv(clause.words, context());
            if (!expanded) {
                return expanded.error();
            }
            words = std::move(expanded).value();
        }

        ExecutionReport report;
        report.status.code = 0;
        for (const std::string& word : words) {
            if (pending_signal_ == ControlSignal::break_ || pending_signal_ == ControlSignal::return_) {
                break;
            }
            clear_signal();
            auto assigned = environment_.assign(clause.variable, word, /*exported=*/false);
            if (!assigned) {
                ExecutionReport failure_report;
                failure_report.status.code = 1;
                failure_report.diagnostics.push_back(assigned.error());
                return failure_report;
            }
            auto body = run_node(clause.body);
            if (!body) {
                return body.error();
            }
            report.status = body.value().status;
            if (body.value().signal == ControlSignal::break_) {
                clear_signal();
                break;
            }
            if (body.value().signal == ControlSignal::continue_) {
                clear_signal();
                continue;
            }
            if (body.value().signal == ControlSignal::return_) {
                return body;
            }
        }
        clear_signal();
        return report;
    }

    Result<ExecutionReport> run_case(const ir::CaseClause& clause) {
        auto subject = expander_->expand_argv({clause.subject}, context());
        if (!subject) {
            return subject.error();
        }
        const std::string value = subject.value().empty() ? std::string {} : subject.value().front();

        // `;&` continues into the following items regardless of their patterns,
        // so matching starts at the first hit and then follows fallthrough links.
        std::size_t start = clause.items.size();
        for (std::size_t index = 0; index < clause.items.size(); ++index) {
            const ir::CaseItem& item = clause.items[index];
            if (item.patterns.empty()) {
                start = index;
                break;
            }
            bool matched = false;
            for (const Argument& pattern : item.patterns) {
                if (matches_case_pattern(value, pattern)) {
                    matched = true;
                    break;
                }
            }
            if (matched) {
                start = index;
                break;
            }
        }
        if (start == clause.items.size()) {
            ExecutionReport report;
            report.status.code = 0;
            return report;
        }

        ExecutionReport report;
        for (std::size_t index = start; index < clause.items.size(); ++index) {
            auto body = run_node(clause.items[index].body);
            if (!body) {
                return body.error();
            }
            report = std::move(body).value();
            if (!clause.items[index].fallthrough) {
                break;
            }
        }
        return report;
    }

    // Case patterns are glob patterns matched against the subject. They are
    // expanded but never field-split or pathname-expanded: '*' is a pattern
    // metacharacter, not a request to list a directory, and a `*` pattern must
    // still match the literal subject "x".
    [[nodiscard]] bool matches_case_pattern(const std::string& subject, const Argument& pattern) {
        Argument literal_pattern;
        for (Expansion fragment : pattern.fragments) {
            fragment.field_splitting = false;
            fragment.globbable = false;
            literal_pattern.fragments.push_back(std::move(fragment));
        }
        auto expanded = expander_->expand_argv({std::move(literal_pattern)}, context());
        if (!expanded || expanded.value().empty()) {
            return false;
        }
        for (const std::string& candidate : expanded.value()) {
            if (detail::glob_component_matches(subject, candidate, std::vector<bool>(candidate.size(), true))) {
                return true;
            }
        }
        return false;
    }

    Result<ExecutionReport> run_function_definition(const ir::FunctionDefinition& definition) {
        define_function(definition.name, definition.body);
        ExecutionReport report;
        report.status.code = 0;
        return report;
    }

    // `break`, `continue`, and `return` are surfaced as a signal on the report
    // rather than as executor state, so an enclosing construct can consume it.
    Result<ExecutionReport> run_control(const ir::Control& control) {
        if (control.signal == ControlSignal::return_) {
            return_code_ = control.code;
        }
        ExecutionReport report;
        report.status.code = control.code;
        report.signal = control.signal;
        report.return_code = control.code;
        return report;
    }

    // Subshells isolate shell state per their inheritance policy. copy /
    // exported_only / empty run the body against a derived child environment
    // whose mutations are discarded on exit; shared_explicit runs against the
    // parent environment so mutations persist. Redirections apply to the body.
    Result<ExecutionReport> run_subshell(const ir::Subshell& subshell) {
        if (subshell.inheritance == EnvironmentInheritance::shared_explicit) {
            return run_with_redirections(subshell.body, subshell.redirections);
        }

        Environment saved = environment_;
        auto saved_tilde = tilde_;
        environment_ = derive_environment(subshell.inheritance);
        tilde_ = std::make_shared<detail::EnvironmentTildeResolver>(environment_);
        auto result = run_with_redirections(subshell.body, subshell.redirections);
        environment_ = std::move(saved);
        tilde_ = std::move(saved_tilde);
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

    // Runs an already-expanded argv with shell-function lookup suppressed. This
    // is what the `command` builtin needs: `command f` must reach the program
    // even when `f` is a shell function.
    Result<ExecutionReport> run_resolved(const std::vector<std::string>& argv) {
        if (argv.empty()) {
            return failure(ErrorCode::empty_argv, "command requires a name");
        }
        ir::Command command;
        command.argv.reserve(argv.size());
        for (const std::string& arg : argv) {
            command.argv.push_back(Argument::raw(arg));
        }
        command.skip_functions = true;
        return run_node(ir::command(std::move(command), "command-builtin"));
    }

    // Re-parses text with the front end's grammar and runs it. Without a parser
    // hook this reports a diagnostic instead of doing nothing.
    Result<ExecutionReport> eval(std::string_view script) {
        if (!scripting_ || !scripting_->parse_hook()) {
            return failure(ErrorCode::invalid_graph, "eval requires a command parser");
        }
        auto program = scripting_->parse_hook()(script);
        if (!program) {
            return program.error();
        }
        return run_node(program.value().root);
    }

    [[nodiscard]] bool exit_requested() const noexcept { return exit_requested_; }
    [[nodiscard]] int exit_code() const noexcept { return exit_code_; }
    void request_exit(int code) noexcept {
        exit_requested_ = true;
        exit_code_ = code;
    }
    void cancel_exit() noexcept { exit_requested_ = false; }

    // Applies a function definition on behalf of the `set -f`-style builtins and
    // the CLI's shell-function surface.
    [[nodiscard]] std::shared_ptr<Expander> expander_backend() const noexcept { return expander_; }

private:
    Environment environment_;
    std::filesystem::path cwd_ {std::filesystem::current_path()};
    ShellOptions options_;
    std::shared_ptr<Executor> executor_;
    std::shared_ptr<Expander> expander_;
    std::shared_ptr<kernel::Registry> kernels_;
    std::shared_ptr<ShellScriptingBackend> scripting_;
    std::shared_ptr<detail::TildeResolver> tilde_;
    std::map<std::string, std::shared_ptr<ShellFunction>, std::less<>> functions_;
    std::vector<std::string> positional_;
    ExitStatus last_status_ {exit_status(0)};
    ControlSignal pending_signal_ {ControlSignal::none};
    int last_background_pid_ {0};
    int return_code_ {0};
    bool exit_requested_ {false};
    int exit_code_ {0};
};

inline void Shell::define_function(std::string name, ir::NodePtr body) {
    auto function = std::make_shared<ShellFunction>(name, std::move(body));
    functions_[std::move(name)] = std::move(function);
}

inline std::shared_ptr<ShellFunction> Shell::find_function(std::string_view name) const {
    auto found = functions_.find(name);
    return found == functions_.end() ? std::shared_ptr<ShellFunction> {} : found->second;
}

inline std::vector<std::string> Shell::function_names() const {
    std::vector<std::string> names;
    names.reserve(functions_.size());
    for (const auto& [name, _] : functions_) {
        names.push_back(name);
    }
    return names;
}

// Function storage is shared_ptr so a recursive call, or a function invoked
// while it is being redefined, observes a live definition rather than a copy.
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
    Redirection redirect;
    redirect.stream = RedirectStream::stdout_stream;
    redirect.mode = RedirectMode::truncate;
    redirect.target = std::move(target);
    ir::Redirected redirected;
    redirected.subject = body;
    redirected.redirections.push_back(std::move(redirect));

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
    if (lua_hook_) {
        return lua_hook_(script, environment);
    }
    return failure(ErrorCode::bad_expansion, "Lua backend is unavailable", std::string(script));
}

} // namespace lsh
