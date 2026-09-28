# 15. Standard Kernels {#manual_standard_kernels}

## Catalog

`stdkern` is the in-tree standard-kernel catalog. It is compiled C++ content with explicit registration and inspectable metadata.

## Access

- `lsh::stdkern::registry()` returns kernel registry content.
- `lsh::stdkern::catalog()` returns metadata without exposing implementation objects.

## Implemented Surface

| Kernel | Contract |
|---|---|
| `:` | zero status, no output |
| `true` | zero status, no output |
| `false` | non-zero status, no output |
| `echo` | space-joined operands; repeated `-n` suppresses newline |
| `printf` | deterministic subset: `%s`, `%d`, `%%`, `\n`, `\t` |
| `basename` | final pathname component plus optional suffix removal |
| `dirname` | directory component, `.` for relative leaf names |
| `env` | exported environment listing |
| `printenv` | selected variables or exported listing |

## Policy

Standard kernels resolve before host `PATH` when registered. This makes standard behavior independent of host utility drift.

## Deferred Surface

Stateful POSIX special builtins remain deferred until typed shell-control effects are represented in the IR.
