# Installing Termlib

## Requirements

- CMake 3.15 or newer.
- A C11 compiler and linker.
- Perl, used to generate the Termscript parser.
- POSIX threads.
- The vendored third-party sources under `third_party/tomlc99` and `third_party/isocline` for the full standard library.

The core libraries can still be built with `-DDOMWEAVE_BUILD_TERMSCRIPT_STDLIB=OFF` when the optional standard-library sources are unavailable.

## Configure and build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The main outputs are:

- `build/libtermlib.a`
- `build/libtermscript_standalone.a`
- `build/libtermscript.a`
- `build/termscript/stdlib/libtermscript_stdlib.a`
- `build/bin/termscript`

## Install

```sh
cmake --install build --prefix "$HOME/.local"
```

Installation includes libraries, the `termscript` executable, `termlib.h`, `termlib.hpp`, Termscript headers and grammar data, standard-library modules, and the documentation under `share/doc/Termlib`.

Set `CMAKE_INSTALL_LIBDIR`, `CMAKE_INSTALL_INCLUDEDIR`, or `CMAKE_INSTALL_DATADIR` during configuration when your platform uses different locations.

## Building generated C

For a generated Termscript unit that uses standard modules, link the generated source with `libtermscript`, `libtermscript_stdlib`, `libtermscript_standalone`, `libtermlib`, and the platform thread library. A generated unit that uses only the standalone runtime does not require `libtermscript_stdlib`.

Use `termscript -c` to stop after emitting C. Without `-c`, the driver can invoke the configured C compiler and produce an executable.

## Verifying the installation

```sh
cmake --build build --target termlib-manual
cmake --install build --prefix /tmp/termlib-install
```

The manual chapters should appear below `/tmp/termlib-install/share/doc/Termlib/manual`.
