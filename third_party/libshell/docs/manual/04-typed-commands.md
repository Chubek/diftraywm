# 04. Typed Commands {#manual_typed_commands}

## Command Model

A command is an `lsh::ir::Command` containing:

- typed `argv` fragments;
- `CommandSource` resolution policy;
- redirections;
- optional execution metadata.

## Builders

| Builder | Source | Use |
|---|---|---|
| `lsh::dsl::cmd` | `auto_resolve` | external/builtin/kernel resolution |
| `lsh::dsl::builtin` | `builtin` | builtin-only dispatch |
| `lsh::dsl::kernel` | `kernel` | kernel registry dispatch |

## Semantics

- `argv[0]` is explicit.
- Arguments are `lsh::Argument`, not joined text.
- Resolution is delayed until `Shell::run` lowers IR into `ExecSpec`.
- Empty argv is rejected as `ErrorCode::empty_argv`.

## Minimal Form

```cpp
auto program = lsh::dsl::cmd("grep", "-n", "TODO", "README.md").program();
```

## Constraint

Shell quoting is not an API boundary. Pass structured arguments and expansion nodes.
