# 08. Pipelines {#manual_pipelines}

## Model

`lsh::ir::Pipeline` composes commands or graph nodes with typed pipe edges. It is not a shell string containing `|`.

## Construction

```cpp
auto p = lsh::dsl::cmd("printf", "a\\nb\\n") | lsh::dsl::cmd("wc", "-l");
```

## Status Selection

`PipefailPolicy` selects pipeline status:

| Policy | Result |
|---|---|
| `last` | status of final command |
| `any_failed` | first non-zero failure semantics |
| `none` | pipeline status suppression |

## Stderr Merge

Pipeline metadata can request stderr merge into stdout before downstream routing.

## Failure Modes

- disconnected pipeline node: `ErrorCode::invalid_graph`;
- command expansion failure: `ErrorCode::bad_expansion`;
- child launch failure: `ErrorCode::execution_failed` or `io_error`.
