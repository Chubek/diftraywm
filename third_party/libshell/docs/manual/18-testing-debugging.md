# 18. Testing and Debugging {#manual_testing_debugging}

## Test Layers

| Layer | Test Strategy |
|---|---|
| DSL | inspect IR nodes |
| Expansion | deterministic environment fixtures |
| Validation | assert `Diagnostic` codes |
| Execution | use `DryRunExecutor` first |
| POSIX | isolate filesystem, fd, and signal behavior |
| Kernels | test via registry dispatch and direct metadata |

## Debugging Artifacts

- `ExecutionReport` for command status aggregation.
- `Diagnostic` for validation/runtime failure.
- dry-run plans for argv/redirection inspection.
- sinklet captures for stdout/stderr assertions.

## Failure Classification

- API failure: failed `Result<T>`.
- command failure: successful report with non-zero `ExitStatus`.
- policy rejection: diagnostic before executor launch.
- host failure: POSIX diagnostic or signal/timeout status.
