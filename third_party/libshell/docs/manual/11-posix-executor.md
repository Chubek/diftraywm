# 11. POSIX Executor {#manual_posix_executor}

## Surface

`include/LibShell-Posix.hpp` provides local POSIX execution through `lsh::LocalExecutor` and support types.

## Responsibilities

- materialize redirection channels;
- fork child processes;
- apply `dup2` stream mapping;
- build child environment vectors;
- apply resource limits;
- pump memory and sinklet outputs;
- wait, timeout, and decode status.

## Builtins

`BuiltinRegistry` stores in-process builtin handlers. Builtins use LibShell writers for stdout/stderr routing.

## Resource Limits

`ResourceLimits` maps portable runtime intent onto POSIX `setrlimit` where supported.

## Constraints

- POSIX executor is platform-specific.
- It is not required for pure IR construction or dry-run validation.
- Host `PATH` lookup and process semantics are intentionally outside the core header contract.
