# 02. Installation {#manual_installation}

## Requirements

- C++20 compiler.
- CMake 3.16 or newer.
- POSIX host for `LibShell-Posix.hpp` execution.
- Doxygen for generated API/manual HTML.

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Install

```sh
cmake --install build --prefix /usr/local
```

Installed surface:

- headers: `${prefix}/include`
- pkg-config file: `${prefix}/lib/pkgconfig/libshell.pc` or platform libdir equivalent
- documentation: `${prefix}/share/docs/libshell`

## Options

| Option | Default | Effect |
|---|---:|---|
| `LIBSH_BUILD_TESTS` | `ON` | builds tests |
| `LIBSH_BUILD_EXAMPLES` | `ON` | builds examples |
| `LIBSH_BUILD_STDKERN` | `ON` | exposes standard-kernel interface target |
| `LIBSH_BUILD_CLI` | `ON` | builds CLI tools |
| `LIBSHELL_GENERATE_DOCS` | `ON` | enables `docs/` subdirectory and install rules |

## Header-Only Use

Use `include/LibShell.hpp` for portable DSL/runtime types. Include `include/LibShell-Posix.hpp` only when local POSIX process execution is required.
