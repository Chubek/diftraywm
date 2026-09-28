# 01. Architecture {#manual_architecture}

## Contract

LibShell separates shell syntax, typed command construction, expansion, graph validation, execution planning, process launch, stream routing, and extension dispatch.

## Layers

| Layer | Primary types | Responsibility |
|---|---|---|
| DSL | `lsh::dsl::Expr`, `lsh::ir::Node` | declarative graph construction |
| IR | `lsh::ir::Command`, `Pipeline`, `Sequence`, `Subshell` | validated execution graph |
| Expansion | `lsh::Argument`, `lsh::Expansion`, `lsh::Expander` | argv materialization |
| Runtime | `lsh::Shell`, `lsh::Executor`, `lsh::ExecSpec` | execution planning and dispatch |
| POSIX | `lsh::LocalExecutor` | fork/exec, redirection, waits, timeouts |
| Extensions | `lsh::Sinklet`, `lsh::kernel::Kernel` | output and command extension points |

## Invariants

- DSL construction is side-effect-free.
- Expansion occurs before executor dispatch.
- Execution consumes `ExecSpec`, not shell text.
- Redirections are typed `Redirection` objects.
- Pipeline status is selected by `PipefailPolicy`.
- Diagnostics use `ErrorCode`, `Diagnostic`, and `Result<T>`.

## Execution Path

```text
C++ DSL / parser
  -> lsh::ir::Program
  -> Shell validation
  -> expansion
  -> ExecSpec
  -> Executor
  -> ExecutionReport
```

## Boundaries

- No stringly command concatenation in core execution paths.
- No parser-owned runtime semantics.
- No sinklet mutation of shell state without explicit host code.
- No kernel bypass of `Shell` registry and execution policy.
