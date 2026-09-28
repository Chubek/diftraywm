# stdkern

`stdkern` contains compiled, self-describing standard kernels. Kernels are runtime extensions, not shell scripts.

Contract:
- metadata: every kernel has `kernel::Metadata` with name, version, summary, and documentation;
- lifecycle: the executor calls `load`, `initialize`, `execute`, and `shutdown` through `kernel::Kernel`;
- I/O: kernels write only through `kernel::Invocation` writers;
- environment: kernels observe the invocation environment, not process globals;
- trust: this directory is compiled source; packaged third-party kernels require a `PackageLoader` policy.

Initial catalog:
- `:`
- `true`
- `false`
- `echo`
- `printf`
- `basename`
- `dirname`
- `env`
- `printenv`

Scope exclusions:
- no parser-owned shell semantics in kernels;
- no implicit command execution inside `env`;
- no special builtins requiring control-flow mutation until the IR exposes that contract.
