# 09. Sequencing and Subshells {#manual_sequencing_subshells}

## Connectives

`lsh::ir::Sequence` uses `Connective`:

| Connective | Contract |
|---|---|
| `sequence` | always run right after left |
| `and_if` | run right when left succeeds |
| `or_if` | run right when left fails |
| `background` | background execution marker |

## DSL Forms

- `a && b` maps to `and_if`.
- `a || b` maps to `or_if`.
- `then(a, b)` maps to plain sequence.
- `subshell(body, inheritance)` isolates or shares environment by policy.

## Subshell Semantics

A subshell executes an IR body under an `EnvironmentInheritance` mode. Copy isolation is the default safe form.

## Constraint

Connective validation occurs before execution. Invalid connective ordering is diagnostic, not deferred host behavior.
