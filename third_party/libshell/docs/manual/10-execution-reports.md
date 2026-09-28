# 10. Execution Reports {#manual_execution_reports}

## Result Channel

Runtime APIs return `Result<T>`. Diagnostics are structured:

- `ErrorCode code`;
- `message`;
- optional `path`.

## Status Channel

`ExitStatus` records process-level outcome:

- exit code;
- signal status;
- timeout flag;
- cancellation flag.

## Report Channel

`ExecutionReport` aggregates status, command reports, diagnostics, and captured output where configured.

## Interpretation

- A successful `Result<ExecutionReport>` means runtime dispatch completed.
- A non-zero `ExitStatus` means command failure, not API failure.
- A failed `Result` means validation, expansion, IO, policy, or launch failure.

## Constraint

Do not encode diagnostics as stderr parsing. Use the structured report path.
