# 20. Extension Roadmap {#manual_extension_roadmap}

## Stable Extension Points

- sinklets for output processing;
- kernels for command implementations;
- scripting backend for command/Lua expansion;
- executor implementations for alternate launch substrates;
- parser frontend for shell text ingestion.

## Deferred Work

- typed control-flow effects for POSIX special builtins;
- serialized IR format;
- declarative kernel package manifests;
- sandbox policy integration;
- richer glob and field-splitting policy;
- async executor backend;
- structured trace output.

## Compatibility Rules

- Preserve typed IR boundaries.
- Add fields with explicit defaults.
- Version package manifests.
- Keep parser extensions separate from runtime extensions.
- Keep host-specific executor behavior outside `LibShell.hpp`.

## Rejection Criteria

Reject extensions that require shell-string reconstruction, hidden global state, parser/runtime coupling, or uninspectable side effects.
