#pragma once

// Layer 2 -- intermediate representation.
//
// A typed, validated graph describing what a shell program means. The IR is
// data-only: it carries no process-launch or platform semantics, and the
// validator rejects malformed graphs before an executor is ever consulted.
//
// Node kinds are a fixed variant so a single exhaustive `std::visit` covers
// validation and execution dispatch; adding a kind forces both to be updated.

#include "Expansion.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lsh {
namespace ir {

struct Node;
using NodePtr = std::shared_ptr<Node>;

// A single simple command: an argument list, an environment overlay, an
// optional working directory, redirections, and a resolution hint.
struct Assignment {
    std::string name;
    Argument value;
    bool exported {false};
};

// A single simple command: an argument list, an environment overlay, an
// optional working directory, redirections, and a resolution hint.
// `assignments` holds the `NAME=value` prefixes; they are overlaid onto the
// child's environment, and applied to the shell environment when argv is empty.
struct Command {
    std::vector<Argument> argv;
    std::vector<Assignment> assignments;
    std::vector<EnvVar> environment_overlay;
    std::optional<std::string> cwd;
    std::vector<Redirection> redirections;
    CommandSource source {CommandSource::auto_resolve};
    // `command NAME ...`: suppress shell-function lookup for this invocation.
    bool skip_functions {false};
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

// Compound forms. Each retains its own redirection list so a redirect applied
// to a compound command covers the whole construct.
struct BraceGroup {
    NodePtr body;
    std::vector<Redirection> redirections;
};

struct IfClause {
    NodePtr condition;
    NodePtr body;
    std::optional<NodePtr> alternative; // else / elif branch
};

struct WhileClause {
    NodePtr condition;
    NodePtr body;
    bool until {false};
    std::vector<Redirection> redirections;
};

struct ForClause {
    std::string variable;
    std::vector<Argument> words; // empty means "$@"
    NodePtr body;
    std::vector<Redirection> redirections;
};

struct CaseItem {
    std::vector<Argument> patterns; // empty matches the default branch
    NodePtr body;
    // `;&` — run this body, then continue matching the following items
    // unconditionally instead of stopping at the first match.
    bool fallthrough {false};
};

struct CaseClause {
    Argument subject;
    std::vector<CaseItem> items;
    std::vector<Redirection> redirections;
};

struct FunctionDefinition {
    std::string name;
    NodePtr body;
};

// A non-local transfer: `break`, `continue`, or `return`. Modelled as a node so
// it flows through validation and dispatch like everything else instead of
// hiding in executor state.
struct Control {
    ControlSignal signal {ControlSignal::break_};
    int code {0};
};

// `! pipeline`. Inversion is a node rather than a synthesized command pair so
// the runtime applies it to the pipeline's status instead of to a `false` exit.
struct Negate {
    NodePtr subject;
};

using NodeValue = std::variant<
    Command,
    Pipeline,
    Sequence,
    Subshell,
    Redirected,
    Assignment,
    BraceGroup,
    IfClause,
    WhileClause,
    ForClause,
    CaseClause,
    FunctionDefinition,
    Control,
    Negate>;

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
    case ExpansionKind::tilde:
    case ExpansionKind::special:
    case ExpansionKind::positional:
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
        if (fragment.has_operand && has_nul_byte(fragment.operand)) {
            diagnose(report, ErrorCode::bad_expansion, "parameter operand contains NUL byte", fragment_path);
        }
        if (fragment.has_pattern && has_nul_byte(fragment.pattern)) {
            diagnose(report, ErrorCode::bad_expansion, "trim pattern contains NUL byte", fragment_path);
        }
        // The parameter operator and the trim form are mutually exclusive: a
        // fragment may not both substitute and rewrite a value.
        if (fragment.op != ParameterOp::none && fragment.trim != TrimOp::none) {
            diagnose(report, ErrorCode::bad_expansion, "parameter expansion combines an operator and a trim", fragment_path);
        }
        if (fragment.op == ParameterOp::none && fragment.has_operand) {
            diagnose(report, ErrorCode::bad_expansion, "parameter operand without an operator", fragment_path);
        }
    }
}

inline void validate_redirection(const Redirection& redirection, ValidationReport& report, const std::string& path) {
    const bool is_input = redirection.stream == RedirectStream::stdin_stream;
    switch (redirection.mode) {
    case RedirectMode::read:
    case RedirectMode::read_write:
        if (!is_input) {
            diagnose(report, ErrorCode::invalid_redirection, "read redirection is only valid for stdin", path);
        }
        break;
    case RedirectMode::truncate:
    case RedirectMode::append:
    case RedirectMode::clobber:
        if (is_input) {
            diagnose(report, ErrorCode::invalid_redirection, "output redirection is not valid for stdin", path);
        }
        break;
    case RedirectMode::duplicate:
    case RedirectMode::close:
        break;
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
        if (redirection.mode == RedirectMode::close && redirection.target.fd) {
            diagnose(report, ErrorCode::invalid_redirection, "close redirection has no descriptor operand", path);
        }
        break;
    case StdioTargetKind::memory:
        if (!redirection.target.memory && !redirection.target.input) {
            diagnose(report, ErrorCode::invalid_redirection, "memory redirection requires a stream", path);
        }
        break;
    case StdioTargetKind::deferred:
        // A redirection path the Shell has not expanded yet.
        if (!redirection.target.deferred) {
            diagnose(report, ErrorCode::invalid_redirection, "deferred redirection requires a path", path);
        }
        break;
    case StdioTargetKind::here_string:
        if (!redirection.target.here_string) {
            diagnose(report, ErrorCode::invalid_redirection, "a here-string requires a word", path);
        }
        if (!is_input) {
            diagnose(report, ErrorCode::invalid_redirection, "a here-string is only valid for stdin", path);
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
    case StdioTargetKind::closed:
        break;
    }
}

inline void validate_redirections(
    const std::vector<Redirection>& redirections, ValidationReport& report, const std::string& path) {
    for (std::size_t index = 0; index < redirections.size(); ++index) {
        std::string child_path = path;
        append_path(child_path, "redir" + std::to_string(index));
        validate_redirection(redirections[index], report, child_path);
    }
}

inline void validate_command(const Command& command_node, ValidationReport& report, const std::string& path) {
    // A command with no argv is legal when it is nothing but assignments, which
    // is how `FOO=bar` sets a variable in the shell.
    if (command_node.argv.empty()) {
        if (command_node.assignments.empty()) {
            diagnose(report, ErrorCode::empty_argv, "command must contain an executable argument or an assignment", path);
        }
    } else if (argument_is_empty(command_node.argv.front())) {
        diagnose(report, ErrorCode::empty_argv, "command argv must contain an executable argument", path);
    }

    for (std::size_t index = 0; index < command_node.assignments.size(); ++index) {
        const Assignment& assignment = command_node.assignments[index];
        if (!valid_variable_name(assignment.name)) {
            diagnose(report, ErrorCode::bad_expansion, "assignment target is not a valid name", path + ".assign" + std::to_string(index));
        }
        validate_argument(assignment.value, report, path + ".assign" + std::to_string(index) + ".value");
    }

    for (std::size_t index = 0; index < command_node.argv.size(); ++index) {
        std::string arg_path = path;
        append_path(arg_path, "argv" + std::to_string(index));
        validate_argument(command_node.argv[index], report, arg_path);
    }

    validate_redirections(command_node.redirections, report, path);
}

inline void validate_node(const NodePtr& node_ptr, ValidationReport& report, const std::string& path) {
    if (!node_ptr) {
        diagnose(report, ErrorCode::invalid_graph, "IR node pointer is null", path);
        return;
    }

    std::visit(
        [&](const auto& node_value) {
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
                validate_redirections(node_value.redirections, report, path);
            } else if constexpr (std::is_same_v<T, Redirected>) {
                if (!node_value.subject) {
                    diagnose(report, ErrorCode::invalid_graph, "redirected node requires a subject", path);
                }
                validate_node(node_value.subject, report, path + ".subject");
                validate_redirections(node_value.redirections, report, path);
            } else if constexpr (std::is_same_v<T, Assignment>) {
                if (!valid_variable_name(node_value.name)) {
                    diagnose(report, ErrorCode::bad_expansion, "assignment target is not a valid name", path + ".name");
                }
                validate_argument(node_value.value, report, path + ".value");
            } else if constexpr (std::is_same_v<T, BraceGroup>) {
                if (!node_value.body) {
                    diagnose(report, ErrorCode::invalid_graph, "brace group requires a body", path);
                }
                validate_node(node_value.body, report, path + ".body");
                validate_redirections(node_value.redirections, report, path);
            } else if constexpr (std::is_same_v<T, IfClause>) {
                if (!node_value.condition || !node_value.body) {
                    diagnose(report, ErrorCode::invalid_graph, "if clause requires a condition and a body", path);
                }
                validate_node(node_value.condition, report, path + ".cond");
                validate_node(node_value.body, report, path + ".body");
                if (node_value.alternative) {
                    validate_node(*node_value.alternative, report, path + ".else");
                }
            } else if constexpr (std::is_same_v<T, WhileClause>) {
                if (!node_value.condition || !node_value.body) {
                    diagnose(report, ErrorCode::invalid_graph, "loop clause requires a condition and a body", path);
                }
                validate_node(node_value.condition, report, path + ".cond");
                validate_node(node_value.body, report, path + ".body");
                validate_redirections(node_value.redirections, report, path);
            } else if constexpr (std::is_same_v<T, ForClause>) {
                if (!valid_variable_name(node_value.variable)) {
                    diagnose(report, ErrorCode::bad_expansion, "for clause requires a valid loop variable", path + ".var");
                }
                for (std::size_t index = 0; index < node_value.words.size(); ++index) {
                    validate_argument(node_value.words[index], report, path + ".word" + std::to_string(index));
                }
                if (!node_value.body) {
                    diagnose(report, ErrorCode::invalid_graph, "for clause requires a body", path);
                }
                validate_node(node_value.body, report, path + ".body");
                validate_redirections(node_value.redirections, report, path);
            } else if constexpr (std::is_same_v<T, CaseClause>) {
                validate_argument(node_value.subject, report, path + ".subject");
                for (std::size_t index = 0; index < node_value.items.size(); ++index) {
                    const CaseItem& item = node_value.items[index];
                    std::string child_path = path;
                    append_path(child_path, "item" + std::to_string(index));
                    for (std::size_t pattern = 0; pattern < item.patterns.size(); ++pattern) {
                        validate_argument(item.patterns[pattern], report, child_path + ".pat" + std::to_string(pattern));
                    }
                    if (!item.body) {
                        diagnose(report, ErrorCode::invalid_graph, "case item requires a body", child_path);
                    }
                    validate_node(item.body, report, child_path + ".body");
                }
                validate_redirections(node_value.redirections, report, path);
            } else if constexpr (std::is_same_v<T, FunctionDefinition>) {
                if (!valid_variable_name(node_value.name)) {
                    diagnose(report, ErrorCode::bad_expansion, "function definition requires a valid name", path + ".name");
                }
                if (!node_value.body) {
                    diagnose(report, ErrorCode::invalid_graph, "function definition requires a body", path);
                }
                validate_node(node_value.body, report, path + ".body");
            } else if constexpr (std::is_same_v<T, Control>) {
                if (node_value.signal == ControlSignal::none) {
                    diagnose(report, ErrorCode::invalid_graph, "control node carries no signal", path);
                }
            } else if constexpr (std::is_same_v<T, Negate>) {
                if (!node_value.subject) {
                    diagnose(report, ErrorCode::invalid_graph, "negation requires a subject", path);
                }
                validate_node(node_value.subject, report, path + ".subject");
            }
        },
        node_ptr->value);
}

} // namespace detail

inline ValidationReport validate(const Program& program) {
    ValidationReport report;
    detail::validate_node(program.root, report, "program");
    return report;
}

} // namespace ir
} // namespace lsh
