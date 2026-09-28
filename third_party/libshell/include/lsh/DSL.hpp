#pragma once

// Layer 6 -- native DSL.
//
// Builders over the IR. Every function is pure: it returns a new Expr and
// never mutates its arguments, so a partial expression can be reused and
// redirected without surprising aliasing. `pipe` and `redirect` are the only
// operators; control flow is expressed with named combinators.

#include "IR.hpp"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace lsh {
namespace detail {

inline StdioTarget file_target(std::string path) {
    StdioTarget target;
    target.kind = StdioTargetKind::file;
    target.file = std::move(path);
    return target;
}

inline Redirection make_file_redirection(RedirectStream stream, RedirectMode mode, std::string path) {
    Redirection redirection;
    redirection.stream = stream;
    redirection.mode = mode;
    redirection.target = file_target(std::move(path));
    return redirection;
}

inline Redirection make_target_redirection(RedirectStream stream, RedirectMode mode, StdioTarget target) {
    Redirection redirection;
    redirection.stream = stream;
    redirection.mode = mode;
    redirection.target = std::move(target);
    return redirection;
}

inline Argument to_argument(Argument argument) { return argument; }
inline Argument to_argument(std::string value) { return literal(std::move(value)); }
inline Argument to_argument(std::string_view value) { return literal(std::string(value)); }
inline Argument to_argument(const char* value) { return literal(value == nullptr ? std::string {} : std::string(value)); }
inline Argument to_argument(int value) { return literal(std::to_string(value)); }

} // namespace detail

// Redirection constructors ---------------------------------------------------

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

// `>|` — truncate even where the redirection would otherwise be refused.
inline Redirection clobber(std::string path) {
    return detail::make_file_redirection(RedirectStream::stdout_stream, RedirectMode::clobber, std::move(path));
}

inline Redirection in_out(std::string path) {
    return detail::make_file_redirection(RedirectStream::stdin_stream, RedirectMode::read_write, std::move(path));
}

inline Redirection to_fd(RedirectStream stream, int fd) {
    StdioTarget target;
    target.kind = StdioTargetKind::fd;
    target.fd = fd;
    return detail::make_target_redirection(stream, RedirectMode::duplicate, std::move(target));
}

// `>&-` / `<&-`: detach the stream entirely.
inline Redirection close_stream(RedirectStream stream) {
    StdioTarget target;
    target.kind = StdioTargetKind::closed;
    return detail::make_target_redirection(stream, RedirectMode::close, std::move(target));
}

inline Redirection to_null(RedirectStream stream) {
    StdioTarget target;
    target.kind = StdioTargetKind::null_device;
    return detail::make_target_redirection(stream, RedirectMode::truncate, std::move(target));
}

inline Redirection to_memory(RedirectStream stream, std::shared_ptr<Writer> memory) {
    StdioTarget target;
    target.kind = StdioTargetKind::memory;
    target.memory = std::move(memory);
    return detail::make_target_redirection(stream, RedirectMode::truncate, std::move(target));
}

// Feed a synthesized payload to a command's stdin (a here-string, in shell terms).
inline Redirection from_memory(std::string payload) {
    StdioTarget target;
    target.kind = StdioTargetKind::memory;
    target.input = std::make_shared<MemoryReader>(std::move(payload));
    return detail::make_target_redirection(RedirectStream::stdin_stream, RedirectMode::read, std::move(target));
}

// A here-string: the payload is an unexpanded word, materialized at run time.
inline Redirection from_word(Argument word) {
    StdioTarget target;
    target.kind = StdioTargetKind::here_string;
    target.here_string = std::move(word);
    return detail::make_target_redirection(RedirectStream::stdin_stream, RedirectMode::read, std::move(target));
}

inline Redirection from_reader(std::shared_ptr<Reader> reader) {
    StdioTarget target;
    target.kind = StdioTargetKind::memory;
    target.input = std::move(reader);
    return detail::make_target_redirection(RedirectStream::stdin_stream, RedirectMode::read, std::move(target));
}

inline Redirection to_sinklet(RedirectStream stream, std::shared_ptr<Sinklet> sinklet) {
    StdioTarget target;
    target.kind = StdioTargetKind::sinklet;
    target.sinklet = std::move(sinklet);
    return detail::make_target_redirection(stream, RedirectMode::truncate, std::move(target));
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

[[nodiscard]] inline Expr external(std::string name) {
    ir::Command command;
    command.source = CommandSource::external;
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

[[nodiscard]] inline Expr background(Expr left) {
    return Expr {ir::node(
        ir::Sequence {
            .left = left.node(),
            .right = ir::command(ir::make_command({":"}), "background"),
            .connective = Connective::background,
        },
        "background")};
}

[[nodiscard]] inline Expr subshell(Expr body, EnvironmentInheritance inheritance = EnvironmentInheritance::copy) {
    ir::Subshell subshell_node;
    subshell_node.body = body.node();
    subshell_node.inheritance = inheritance;
    return Expr {ir::node(std::move(subshell_node), "subshell")};
}

[[nodiscard]] inline Expr brace_group(Expr body) {
    ir::BraceGroup group;
    group.body = body.node();
    return Expr {ir::node(std::move(group), "brace-group")};
}

[[nodiscard]] inline Expr assignment(std::string name, Argument value, bool exported = false) {
    ir::Assignment assignment_node;
    assignment_node.name = std::move(name);
    assignment_node.value = std::move(value);
    assignment_node.exported = exported;
    return Expr {ir::node(std::move(assignment_node), "assignment")};
}

[[nodiscard]] inline Expr if_then_else(Expr condition, Expr body, std::optional<Expr> alternative = std::nullopt) {
    ir::IfClause clause;
    clause.condition = condition.node();
    clause.body = body.node();
    if (alternative) {
        clause.alternative = alternative->node();
    }
    return Expr {ir::node(std::move(clause), "if")};
}

[[nodiscard]] inline Expr while_loop(Expr condition, Expr body) {
    ir::WhileClause clause;
    clause.condition = condition.node();
    clause.body = body.node();
    return Expr {ir::node(std::move(clause), "while")};
}

[[nodiscard]] inline Expr until_loop(Expr condition, Expr body) {
    ir::WhileClause clause;
    clause.condition = condition.node();
    clause.body = body.node();
    clause.until = true;
    return Expr {ir::node(std::move(clause), "until")};
}

[[nodiscard]] inline Expr for_loop(std::string variable, std::vector<Argument> words, Expr body) {
    ir::ForClause clause;
    clause.variable = std::move(variable);
    clause.words = std::move(words);
    clause.body = body.node();
    return Expr {ir::node(std::move(clause), "for")};
}

[[nodiscard]] inline Expr case_clause(Argument subject, std::vector<ir::CaseItem> items) {
    ir::CaseClause clause;
    clause.subject = std::move(subject);
    clause.items = std::move(items);
    return Expr {ir::node(std::move(clause), "case")};
}

[[nodiscard]] inline Expr function(std::string name, Expr body) {
    ir::FunctionDefinition definition;
    definition.name = std::move(name);
    definition.body = body.node();
    return Expr {ir::node(std::move(definition), "function")};
}

// The vector overload is defined first so the single-redirection form can
// delegate to it without an extra declaration.
[[nodiscard]] inline Expr redirect(Expr subject, std::vector<Redirection> redirections) {
    // A redirect on a simple command folds into the command itself, which keeps
    // the graph shallow and lets the executor see the stream list in one place.
    if (subject.node()) {
        if (auto* command = std::get_if<ir::Command>(&subject.node()->value)) {
            auto copy = *command;
            copy.redirections.insert(copy.redirections.end(), redirections.begin(), redirections.end());
            return Expr {ir::command(std::move(copy), subject.node()->debug_name)};
        }
    }
    return Expr {ir::node(ir::Redirected {.subject = subject.node(), .redirections = std::move(redirections)}, "redirect")};
}

[[nodiscard]] inline Expr redirect(Expr subject, Redirection redirection) {
    std::vector<Redirection> redirections;
    redirections.push_back(std::move(redirection));
    return redirect(std::move(subject), std::move(redirections));
}

// Attaches redirections to any node kind, wrapping when the subject is not a
// simple command.
[[nodiscard]] inline Expr with_redirections(Expr subject, std::vector<Redirection> redirections) {
    if (redirections.empty()) {
        return subject;
    }
    if (auto* command = std::get_if<ir::Command>(&subject.node()->value)) {
        auto copy = *command;
        copy.redirections.insert(copy.redirections.end(), redirections.begin(), redirections.end());
        return Expr {ir::command(std::move(copy), subject.node()->debug_name)};
    }
    if (auto* group = std::get_if<ir::BraceGroup>(&subject.node()->value)) {
        auto copy = *group;
        copy.redirections.insert(copy.redirections.end(), redirections.begin(), redirections.end());
        return Expr {ir::node(std::move(copy), subject.node()->debug_name)};
    }
    if (auto* subshell = std::get_if<ir::Subshell>(&subject.node()->value)) {
        auto copy = *subshell;
        copy.redirections.insert(copy.redirections.end(), redirections.begin(), redirections.end());
        return Expr {ir::node(std::move(copy), subject.node()->debug_name)};
    }
    return Expr {ir::node(ir::Redirected {.subject = subject.node(), .redirections = std::move(redirections)}, "redirect")};
}

[[nodiscard]] inline Expr with_cwd(Expr subject, std::string directory) {
    if (auto* command = std::get_if<ir::Command>(&subject.node()->value)) {
        auto copy = *command;
        copy.cwd = std::move(directory);
        return Expr {ir::command(std::move(copy), subject.node()->debug_name)};
    }
    return subject;
}

// Marks the next command's environment overlay. `NAME=value` prefixes.
[[nodiscard]] inline Expr with_env(Expr subject, std::vector<EnvVar> overlay) {
    if (auto* command = std::get_if<ir::Command>(&subject.node()->value)) {
        auto copy = *command;
        copy.environment_overlay = std::move(overlay);
        return Expr {ir::command(std::move(copy), subject.node()->debug_name)};
    }
    return subject;
}

} // namespace dsl
} // namespace lsh
