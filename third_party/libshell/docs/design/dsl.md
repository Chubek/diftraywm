# DSL Design

The C++ DSL is a declarative frontend for the Lakposht IR. Operators and helper functions never execute commands directly; they construct `lsh::ir::Node` graphs that are later validated and run by `lsh::Shell`. This keeps the "build a graph" and "run a graph" phases separable: a program can be assembled, inspected, serialized, or rejected before any process, builtin, or kernel is consulted.

## The `Expr` Handle

The public DSL lives under `lsh::dsl`. Every builder returns an `Expr`, a thin owning handle around an `ir::NodePtr`:

- `Expr()` is an empty (null) expression. Conversion to `bool` tests whether a node is held.
- `Expr(ir::NodePtr)` and `Expr(ir::Command)` adopt a prebuilt node.
- `expr.node()` exposes the underlying `ir::NodePtr` for inspection.
- `expr.program()` wraps the node as an `ir::Program` ready for `Shell::run`.

Because `Expr` only holds a node pointer, composition operators are pure tree construction. They have no side effects on the shell, the environment, or the filesystem.

## Command Builders

- `cmd(executable, args...)` builds an `ir::Command` with `CommandSource::auto_resolve`. The executable becomes the first argv element as a raw literal; each trailing argument is converted via `detail::to_argument`, which accepts `Argument`, `std::string`, `std::string_view`, and `const char*`. This overloading lets callers mix pre-built expansion fragments with plain strings.
- `builtin(name)` builds a command with `CommandSource::builtin`. The executor resolves it against its builtin registry instead of `PATH`.
- `kernel(name)` builds a command with `CommandSource::kernel`. The executor binds it to a registered `kernel::Kernel` instance.

Arguments are converted into typed `lsh::Argument` values, preserving expansion intent instead of concatenating shell text. An `Argument` is an ordered vector of `Expansion` fragments; the expander resolves fragments at execution-planning time, after validation but before the executor receives an `ExecSpec`.

## Composition Operators

Composition operators have explicit semantics. Operator precedence is fixed by C++ grammar, not reinvented:

| Construct | Builder | Operator | IR node |
|---|---|---|---|
| Pipeline | `pipe(left, right, pipefail, merge_stderr)` | `a \| b` | `ir::Pipeline` |
| AND short-circuit | `sequence(left, right, and_if)` | `a && b` | `ir::Sequence` |
| OR short-circuit | `sequence(left, right, or_if)` | `a \|\| b` | `ir::Sequence` |
| Plain sequencing | `then(left, right)` | (none) | `ir::Sequence` (`sequence`) |
| Subshell | `subshell(body, inheritance)` | (none) | `ir::Subshell` |
| Redirection | `redirect(subject, redirection)` | (none) | in-place on `Command`, else `ir::Redirected` |

Plain sequential execution is expressed with `then(left, right)` to avoid overloading comma or semicolon-like behavior in C++. The `pipe` builder flattens: piping a pipeline into a command appends rather than nests, so `a | b | c` produces a single three-stage `ir::Pipeline`, not nested pipelines.

Because C++ overloads `|` and `&&`/`||` left-associatively with `|` binding tighter than `&&`/`||`, the DSL's operator precedence matches shell semantics: `a | b && c` parses as `(a | b) && c`. Callers must use `then` for unconditional sequencing since C++ comma cannot be overloaded to yield a meaningful sequence node here.

## Redirection Helpers

Redirections are represented with `lsh::Redirection` objects: a `RedirectStream`, a `RedirectMode`, and a `StdioTarget`. File helpers:

- `in(path)` — read stdin from a file.
- `out(path)` — truncate stdout to a file.
- `append(path)` — append stdout to a file.
- `err(path)` / `err_append(path)` — truncate/append stderr to a file.

Non-file targets:

- `to_fd(stream, fd)` — duplicate an existing file descriptor onto the stream.
- `to_null(stream)` — redirect to the null device.
- `to_memory(stream, writer)` — route the stream into a `std::shared_ptr<Writer>` (typically a `MemoryWriter`) for in-process capture.
- `to_sinklet(stream, sinklet)` — route the stream through a `Sinklet` stream processor before delivery.

`redirect(subject, redirection)` attaches a redirection in-place when `subject` is a `Command`, otherwise it wraps the subject in an `ir::Redirected` node. Multiple `redirect` calls accumulate. The runtime propagates outer redirections through compound nodes (sequences and subshells) so that, for example, capturing the stdout of `false || echo yes` captures the `echo` branch and not the terminal.

## Expansion Helpers

Expansion helpers construct single-fragment `Argument` values tagged with an `ExpansionKind`. The expander interprets each kind against shell state during execution planning:

- `literal(value)` — `ExpansionKind::raw`, no expansion.
- `single_quoted(value)` — literal text, no expansion, preserved verbatim.
- `double_quoted(value)` — text subject to variable expansion only.
- `variable(name, field_splitting)` — `ExpansionKind::variable`; optional field splitting.
- `arithmetic(expression)` — `ExpansionKind::arithmetic`, evaluated by the recursive-descent evaluator.
- `command_substitution(script, field_splitting)` — `ExpansionKind::command`; re-enters the runtime via the configured command-substitution parser.
- `lua(script)` — `ExpansionKind::lua`.
- `glob(pattern)` — `ExpansionKind::glob`.

Arguments may combine fragments: an `Argument` with a raw fragment followed by a `variable` fragment concatenates the literal prefix with the expanded value. Field splitting applies per-fragment and may turn one argument into several argv fields.

## Validation Interplay

The DSL intentionally accepts only command and pipeline operands for `operator|`. `pipe` flattens via `detail::append_pipeline_commands`, which refuses non-command/non-pipeline nodes; attempting to pipe an arbitrary compound expression yields an invalid pipeline node (`debug_name = "invalid-pipeline"`), which `ir::validate` rejects before execution. No DSL operator throws on shape mismatch; it produces a structurally invalid graph that validation reports as a diagnostic. This keeps construction total and defers rejection to the validation pass, where multiple diagnostics can be collected.

## Subshells

`subshell(body, inheritance)` wraps a body node in an `ir::Subshell` with an explicit `EnvironmentInheritance` policy (`copy`, `exported_only`, `empty`, or `shared_explicit`). The runtime derives a child environment from the policy before running the body, so mutations inside the subshell (such as `set VAR=value`) do not leak into the parent unless `shared_explicit` is chosen.

## Example

```cpp
using namespace lsh::dsl;

auto graph =
    (cmd("printf", "%s\\n", lsh::variable("PROJECT"))
     | cmd("grep", "Lakposht"))
    && redirect(cmd("wc", "-l"), lsh::out("count.txt"));

lsh::Shell shell;
auto report = shell.run(graph.program());
```

The graph builds an `ir::Sequence` whose left operand is a two-stage `ir::Pipeline` and whose right operand is a `wc` command carrying a file redirection. Nothing executes until `shell.run` validates, expands, and dispatches to the configured executor. The same node can be inspected first:

```cpp
const auto& node = graph.node();
const auto* seq = std::get_if<lsh::ir::Sequence>(&node->value);
// seq->left  -> ir::Pipeline{printf, grep}
// seq->right -> ir::Command{wc -l, redirections=[out("count.txt")]}
```
