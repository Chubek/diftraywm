# Kernel Design

Kernels are runtime extensions that behave like built-in commands while remaining loadable and inspectable. Unlike ad-hoc builtins compiled into the executor, kernels are self-describing objects registered at runtime and resolved by name or alias. Kernel-facing contracts live in `include/LibShell-Kernel.hpp`.

## Metadata and Identity

Each kernel exposes `kernel::Metadata`:

- `name` — canonical invocation name (required; registration refuses an empty name).
- `aliases` — additional lookup keys that resolve back to the same instance.
- `version` — a `kernel::Version` (`major`, `minor`, `patch`, `label`) for compatibility checks.
- `summary` — short human-readable description.
- `documentation` — longer usage text.

The `kernel::Registry` indexes both canonical names and aliases in two maps. `find(name)` first tries a direct lookup, then resolves through the alias map back to the canonical name, returning the shared `Kernel` instance. `list()` enumerates metadata for discovery and help output. Registry lookups are read-only and return an empty `shared_ptr` for unknown names rather than a diagnostic, so callers can fall back to `PATH` resolution or report their own error.

## Lifecycle

The lifecycle is deterministic and ordered: `load`, `initialize`, `execute`, `shutdown`.

- `load()` — one-time setup that does not depend on shell state (e.g. opening resources, validating self).
- `initialize(Environment&)` — per-run setup receiving the shell environment view.
- `execute(const Invocation&)` — the per-invocation entry point, called once per command occurrence. Returns `Result<ExitStatus>`.
- `shutdown()` — release resources acquired in `load`/`initialize`.

`Kernel` provides no-op defaults for `load`, `initialize`, and `shutdown`; only `execute` is abstract. Implementations must keep `execute` side-effect-free with respect to global shell state unless they mutate through the `Environment` they are handed.

## Invocation

`execute` receives a `kernel::Invocation`:

- `argv` — already-expanded argument vector, including the kernel's own name at position 0.
- `environment` — pointer to the `Environment` view the kernel shares with the rest of the runtime.
- `stdin_writer`, `stdout_writer`, `stderr_writer` — `Writer*` sinks the kernel writes to; null when a stream is not routed.

Kernels write output through these writers rather than touching the real stdio directly. This lets the runtime capture, redirect, or pipe kernel output through the same sink machinery used by external processes and builtins, including `to_memory` capture and `to_sinklet` stream processing.

## `FunctionKernel`

`FunctionKernel` is the reference adapter for simple C++ kernels. It wraps an `ExecuteFn` (`std::function<Result<ExitStatus>(const Invocation&)>`) and delegates `execute` to it, returning a diagnostic if no handler is set. It is sufficient for stateless kernels and tests. Larger kernels should derive from `kernel::Kernel` directly and keep argument parsing, stream handling, and state initialization explicit in overridden lifecycle methods rather than cramming them into a single lambda.

## `.lakk` Packages

`.lakk` package support is represented by `PackageManifest` and `PackageLoader`.

- `PackageManifest` carries `Metadata`, `entrypoints`, and `permissions`. Permissions declare what a package is allowed to touch (filesystem paths, network, environment), giving the runtime a contract to enforce.
- `PackageLoader` is an abstract interface with `inspect(path)` (returning a manifest without executing) and `load(path)` (returning a kernel instance). Separating inspection from loading lets the runtime apply trust and sandbox policy between the two steps.

Loaders must enforce trust policy, path validation, and sandbox constraints outside the kernel implementation. A kernel loaded from a `.lakk` package is indistinguishable from a compiled-in kernel once registered: it participates in the registry and executor path identically.

## Execution Integration

Kernels participate in pipelines through the normal executor path. The runtime marks kernel commands with `CommandSource::kernel`; concrete executors decide how to bind those specs to registered kernel instances. The `Shell` holds a `shared_ptr<kernel::Registry>` (set via `set_kernels`); an unregistered kernel name surfaces as an execution diagnostic rather than a `PATH` lookup. Kernel stdout/stderr feed downstream pipeline stages or sinks exactly like builtin output, so a kernel can sit in the middle of a pipeline (`... | greet | ...`) without special handling.

## Example

```cpp
lsh::kernel::Metadata metadata;
metadata.name = "greet";
metadata.summary = "greeting kernel";

auto kernel = std::make_shared<lsh::kernel::FunctionKernel>(
    metadata,
    [](const lsh::kernel::Invocation& inv) -> lsh::Result<lsh::ExitStatus> {
        if (inv.stdout_writer) {
            if (auto r = inv.stdout_writer->write("hello from kernel\n"); !r) {
                return r.error();
            }
        }
        return lsh::ExitStatus {};
    });

auto registry = std::make_shared<lsh::kernel::Registry>();
registry->add(kernel);

lsh::Shell shell(std::make_shared<lsh::LocalExecutor>());
shell.set_kernels(registry);

auto mem = std::make_shared<lsh::MemoryWriter>();
auto expr = lsh::dsl::redirect(
    lsh::dsl::kernel("greet"),
    lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
shell.run(expr.program());
// mem->bytes() == "hello from kernel\n"
```
