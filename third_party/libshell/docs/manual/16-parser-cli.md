# 16. Parser and CLI {#manual_parser_cli}

## Components

- `cli/Shell.syntax`: grammar-facing syntax artifact.
- `cli/libsh-parse.cpp`: parse frontend.
- `cli/libsh-cli.cpp`: command-line runtime frontend.

## Contract

The parser produces LibShell IR-compatible structures. It must not embed process-launch semantics.

## CLI Responsibilities

- accept user shell text or scripts;
- parse into typed runtime forms;
- configure shell options;
- select executor;
- render reports and diagnostics.

## Separation

```text
text input -> parser -> IR -> Shell -> Executor
```

## Failure Modes

- syntax error: parser diagnostic;
- invalid graph: runtime validation diagnostic;
- failed command: execution report status;
- runtime IO failure: `Result` diagnostic.
