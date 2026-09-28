# 06. Environment {#manual_environment}

## Model

`lsh::Environment` stores `EnvVar` entries with value and export bit. It is shell-visible state, not host process global state.

## Operations

- `get(key)` returns an optional value.
- `set(key, value, exported)` inserts or replaces.
- `unset(key)` removes.
- exported views feed child environments.

## Inheritance

`EnvironmentInheritance` controls subshell and child environment construction:

| Mode | Contract |
|---|---|
| `copy` | independent copy |
| `exported_only` | exported entries only |
| `empty` | no inherited variables |
| `shared_explicit` | explicit shared shell state |

## POSIX Import

`LibShell-Posix.hpp` can import `environ` into a shell environment for local execution.

## Constraint

Environment mutation belongs to `Shell` and explicit builtins/kernels. Expansion reads environment snapshots; it must not mutate them.
