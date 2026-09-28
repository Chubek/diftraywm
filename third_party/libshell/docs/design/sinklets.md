# Sinklets Design

Sinklets are programmable stream processors attached to runtime sinks. They consume incremental byte chunks and may forward, transform, aggregate, or inspect output without changing the command graph. A sinklet sits between a command's stdout/stderr and the final `Writer`, so transformations like line filtering, tee, or JSON encoding happen in flight without forking extra processes.

## Lifecycle

Every sinklet follows a deterministic lifecycle: `begin(context)`, zero or more `write(chunk, context)` calls, and `end(context)`. The base `lsh::Sinklet` provides no-op defaults for `begin` and `end`; only `write` is abstract. Implementations must tolerate chunk boundaries that do not align with lines, UTF-8 code points, or records — a single `write` may carry a partial line, and a logical record may span many calls.

`SinkletContext` carries the stream `source` name and an optional `Environment*`. Sinklets report failures with `Result<void>` so downstream routing can stop deterministically on the first error rather than silently corrupting output.

## Writers vs. Sinklets

Writers are separated from sinklets. `lsh::Writer` handles concrete output destinations (`write`, `flush`, `close`); sinklets focus on stream processing. The provided writers are small reference implementations:

- `OstreamWriter` — wraps a `std::ostream`.
- `MemoryWriter` — accumulates bytes in a string for in-process capture (`bytes()`, `take()`).

The bridge between the two worlds is `SinkletWriter`, a `Writer` that adapts a `Sinklet`. It runs `begin` on the first `write`, forwards each chunk to `Sinklet::write`, and runs `end` on `close`. The runtime uses this adapter when a redirection targets `StdioTargetKind::sinklet`.

## Wiring

Sinklets attach through the DSL via `to_sinklet(stream, sinklet)`, which builds a `Redirection` with `StdioTargetKind::sinklet`. The executor materializes that redirection as a `SinkletWriter` and routes the command's stream into it. A sinklet's downstream `Writer` is whatever the caller constructed it with — typically a `MemoryWriter` for capture or an `OstreamWriter` for pass-through. Because sinklets are just `Redirection` targets, they compose with file, fd, and memory redirections and participate in pipelines like any other sink.

## Provided Sinklets

- `LambdaSinklet` — wraps three `std::function` callbacks (`begin`, `write`, `end`) for ad-hoc processors. Returns a diagnostic if `write` is unset.
- `TeeSinklet` — forwards every chunk verbatim to two downstream writers, enabling fan-out without a separate `tee` process.
- `LineSinklet` — abstract base that buffers partial lines, invokes `on_line` per complete newline-terminated line, and flushes the remainder on `end`. This absorbs chunk-boundary handling for line-oriented processors.
- `GrepSinklet` — `LineSinklet` that forwards only lines containing a needle.
- `JsonLinesSinklet` — `LineSinklet` that emits each line as `{"line": "<escaped>"}\n`, escaping backslash and quote.

New line-oriented processors should derive from `LineSinklet` and implement `on_line` rather than reimplementing buffering.

## Example

```cpp
auto mem = std::make_shared<lsh::MemoryWriter>();
auto grep = std::make_shared<lsh::GrepSinklet>("beta", mem);

lsh::Shell shell(std::make_shared<lsh::LocalExecutor>());
auto expr = lsh::dsl::redirect(
    lsh::dsl::cmd("printf", "%s\\n", "alpha", "beta", "gamma"),
    lsh::to_sinklet(lsh::RedirectStream::stdout_stream, grep));
shell.run(expr.program());
// mem->bytes() == "beta\n"
```

The `printf` builtin emits three lines; the `GrepSinklet` receives them as chunks via its `SinkletWriter`, buffers and splits them on newlines, and forwards only the matching line to the `MemoryWriter`.

## Concurrency

Thread safety is implementation-defined unless a sinklet documents stronger guarantees. The runtime may call a single sinklet's `write` sequentially for a stream; the current executors do not deliver parallel chunks to one sinklet. A future executor that advertises parallel stream delivery would require sinklets to document their own synchronization. Sinklets must not implicitly mutate global shell state; if they need context, they read it through the `SinkletContext` they are handed.
