# 12. Dry Run and Validation {#manual_dry_run_validation}

## DryRunExecutor

`lsh::DryRunExecutor` consumes `ExecSpec` and produces execution reports without launching processes. It validates lowering and records planned commands.

## Validation Targets

- empty argv;
- invalid redirections;
- invalid graph structure;
- bad expansion fragments;
- missing runtime extension resolution;
- malformed command source selection.

## Use Cases

- tests that must not fork;
- CLI preview mode;
- policy inspection;
- documentation examples;
- graph serialization diagnostics.

## Contract

Dry run validates the LibShell plan. It does not validate host filesystem executability, `PATH` drift, child process behavior, or external command semantics.
