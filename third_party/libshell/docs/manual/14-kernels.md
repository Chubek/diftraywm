# 14. Kernels {#manual_kernels}

## ABI

`include/LibShell-Kernel.hpp` defines the kernel extension contract. Kernels are command providers registered with a shell and dispatched by name.

## Responsibilities

A kernel implementation owns:

- metadata declaration;
- argument validation;
- execution against supplied context;
- stdout/stderr writes through LibShell writers;
- deterministic status reporting.

## Resolution

`CommandSource::kernel` requires kernel lookup. `CommandSource::auto_resolve` can select a kernel according to shell resolution order.

## Package Boundary

Kernel packages must remain inspectable before activation. Loader policy should validate metadata, compatibility, archive shape, and trust constraints before registration.

## Constraint

Kernels are runtime extensions, not parser extensions. They consume typed argv and runtime context after IR validation.
