# XI Adapter Design

XI adapters describe external command-line tools with structured metadata rather than ad-hoc shell strings. An adapter captures discovery, argument schemas, normalization rules, and execution policy.

`.xi` specifications should compile into typed adapter objects before use. A shell runtime then converts validated adapter invocations into `ExecSpec` values, preserving the same validation and execution path as DSL and CLI commands.

Discovery mechanisms may include PATH lookup, pkg-config queries, explicit validation commands, or fixed paths. Results should be cached with enough context to invalidate when PATH, platform, or adapter version changes.

Argument schemas define flags, options, positional values, variadic sections, allowed values, and repeatability. Normalization rules convert high-level user input into argv while retaining diagnostics for unsupported combinations.

Execution policies cover timeout, retry, stderr routing, environment overlays, working directory, and resource limits. Policies integrate with `ShellOptions`, `ExecSpec`, and executor-specific capability checks.

Adapters must not expose third-party parser or probing library types through the public Lakposht API.
