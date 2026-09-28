# 07. Redirection and Stdio {#manual_redirection_stdio}

## Types

- `RedirectStream`: stdin, stdout, stderr.
- `RedirectMode`: read, truncate, append, clobber, duplicate.
- `StdioTargetKind`: inherit, null, pipe, file, fd, memory, sinklet.
- `Redirection`: stream, mode, target.

## Builders

| Builder | Effect |
|---|---|
| `in(path)` | stdin from file |
| `out(path)` | stdout truncate file |
| `append(path)` | stdout append file |
| `err(path)` | stderr truncate file |
| `err_append(path)` | stderr append file |
| `to_fd(stream, fd)` | duplicate existing fd |
| `to_null(stream)` | route to null device |
| `to_memory(stream, writer)` | route to `Writer` |
| `to_sinklet(stream, sinklet)` | route to `Sinklet` |

## Precedence

For a stream, the last redirection wins. POSIX materialization resolves one channel per standard stream.

## Constraints

- stdin sinklet/memory pumping is not a POSIX executor contract in this revision.
- fd targets assume caller-owned fd validity.
- file targets return `ErrorCode::io_error` on open failure.
