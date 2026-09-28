# 13. Sinklets {#manual_sinklets}

## Model

A sinklet is a programmable output endpoint. It receives byte streams through LibShell writer routing and can transform, capture, filter, or forward records.

## Core Types

- `Writer`: byte-oriented output interface.
- `Sinklet`: programmable sink abstraction.
- `SinkletWriter`: adapts sinklets to writer routing.

## Routing

Use `to_sinklet(RedirectStream::stdout_stream, sinklet)` or stderr equivalent. External processes route through POSIX pump pipes; in-process builtins and kernels write through runtime writers.

## Contracts

- Sinklets process output, not shell state.
- Backpressure and buffering are executor concerns.
- Sinklet diagnostics must propagate as runtime diagnostics where possible.

## Failure Modes

- writer failure: `ErrorCode::io_error`;
- malformed sink target: `ErrorCode::invalid_redirection`;
- application-level rejection: sinklet-defined diagnostic.
