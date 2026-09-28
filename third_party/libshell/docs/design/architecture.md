# LibShell/Lakposht Runtime Architecture

This document defines the first concrete architecture for the Lakposht shell runtime. The runtime is a typed shell execution engine, not a string command launcher. Every frontend lowers into the same validated intermediate representation before execution.

## Naming

The repository currently uses `LibShell` in file names and `lsh` for the short C++ namespace. `Lakposht` is the product/project name used by manifests and design prose. Public compatibility headers may expose both names, but implementation types should live in `lsh` and may be aliased through `lakposht`.

## Subsystem Map

| Subsystem | Primary Components | Contract |
|---|---|---|
| IR | `ir::Program`, `ir::Node`, `ir::Command`, `ir::Pipeline`, `ir::Sequence`, `ir::Redirection` | Stores inspectable execution graphs with no side effects. |
| Validation | `ir::validate` | Rejects malformed graphs before any process, builtin, or kernel runs. |
| Environment | `Environment`, `EnvironmentView`, `EnvVar` | Owns variables, export state, inheritance, and subshell snapshots. |
| Expansion | `Expansion`, `Argument`, `Expander` | Converts typed argument fragments into argv using shell state. |
| Execution | `Executor`, `ExecSpec`, `ExecutionReport` | Launches external processes, builtins, and kernels behind one runtime contract. |
| Shell | `Shell`, `ShellOptions` | Coordinates environment, expansion, validation, executor, sink routing, and options. |
| Sink | `Writer`, `Sinklet`, `SinkRoute` | Routes stdout/stderr into inherited streams, files, memory, or stream processors. |
| DSL | Future builders/operators | Constructs IR nodes only; never executes implicitly. |
| CLI Parser | Future PEGTL AST/lowering | Parses text into AST, lowers AST into IR, then calls `Shell`. |
| Kernels | `Kernel`, `KernelRegistry`, `.lakk` loader | Provides runtime commands that participate in normal pipelines. |
| XI | Future adapter specs | Converts structured external-tool metadata into safe command IR. |
| Puppeteer | Future PTY sessions | Automates interactive programs while using runtime policies and sinks. |

## Execution Flow

```text
DSL / Programmatic API / CLI parser
  -> frontend AST or builders
  -> lsh::ir::Program
  -> lsh::ir::validate
  -> expansion and execution planning
  -> lsh::Executor
  -> lsh::ExecutionReport / lsh::ExitStatus
```

No frontend may bypass validation. No parser may launch commands directly. No DSL operator may perform execution as an operator side effect.

## Intermediate Representation

The IR is the shared contract between all frontends and the runtime. It intentionally represents shell semantics before platform execution details are chosen.

Core IR nodes:

- `Command`: typed argv fragments, environment overlay, working directory, redirections, and command source classification.
- `Pipeline`: ordered command nodes plus pipefail/stderr policy.
- `Sequence`: boolean and sequencing connectives over child nodes.
- `Subshell`: isolated child program with explicit environment inheritance policy.
- `Redirection`: stream, mode, and target metadata attached to commands or compound nodes.

The IR must remain inspectable. Debuggers and tests should be able to enumerate argv fragments, redirections, connectives, and policies without parsing command strings.

## Validation Contracts

Validation is a pure pass over the IR and runtime-independent options. It returns structured diagnostics and never mutates shell state.

Initial validation rules:

- command argv cannot be empty,
- executable position cannot expand from an empty argument list,
- pipelines must contain at least one command,
- redirections must target a valid file, fd, memory buffer, sinklet, pipe, null, or inherited stream,
- read redirections cannot target stdout/stderr streams,
- truncate/append/clobber redirections require file-like output targets,
- boolean connectives require both operands,
- background nodes cannot be nested where the selected executor cannot represent jobs.

Validation should collect multiple diagnostics where practical so users get actionable feedback.

## Environment Model

`Environment` stores shell variables as `(key, value, exported)`. `EnvironmentView` is the mutable facade exposed by `Shell` and `Subshell`.

Execution inheritance rules:

- exported variables are inherited by external processes,
- non-exported variables remain shell-local,
- command overlays apply only to the command or compound node they decorate,
- subshells receive a copy-on-create environment unless configured for explicit shared mutation,
- runtime kernels observe the same environment view as builtins unless sandbox policy narrows it.

## Expansion Model

Arguments are structured as ordered `Expansion` fragments. A command argument is not a pre-quoted string. The expander is responsible for variable, arithmetic, command, Lua, glob, and raw literal semantics according to shell options.

Expansion must run after validation but before executor invocation. Expansion failures produce diagnostics with the originating IR location when available.

## Executor Contract

`Executor` is the only component that performs launch or command invocation. It receives an `ExecSpec` containing argv, cwd, environment, resource limits, timeout, and stream routes.

Executor implementations may support:

- POSIX process spawning,
- C++ builtins,
- Lua-compatible runtime kernels,
- adapter-backed external command invocation,
- dry-run or trace execution for tests.

The shell runtime chooses an executor but does not expose executor implementation types through the high-level DSL.

## Sink Contract

Sinks and sinklets are stream-routing primitives. A `Writer` consumes bytes. A `Sinklet` has deterministic `begin`, `write`, and `end` phases and may forward transformed data to another writer.

Sinklets must not implicitly mutate global shell state. If a sinklet needs shell context, it receives an explicit `SinkletContext` and must document any state it reads.

## Shell Runtime

`Shell` owns the long-lived runtime state:

- environment,
- default cwd,
- shell options,
- expander,
- executor,
- kernel registry,
- adapter registry,
- sink routing defaults.

`Shell::run` validates the IR program, expands executable specs, invokes the executor, aggregates pipeline status according to pipefail, and returns a structured report.

## Implementation Order

1. Define the public IR, result, diagnostic, environment, sink, executor, and shell coordination types in headers.
2. Implement validation for commands, pipelines, sequences, and redirections.
3. Add a dry-run executor for tests and examples.
4. Add process execution behind `Executor` without changing the IR.
5. Add DSL builders that produce IR only.
6. Add PEGTL parser AST and lower it into IR.
7. Add kernel registry and `.lakk` loading behind `Executor`.
8. Add sinklet batteries, XI adapters, and Puppeteer on top of the stable runtime contracts.

This order keeps the project testable before full process, parser, and kernel integration exist.
