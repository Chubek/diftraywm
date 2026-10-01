# Termlib Guide

## 1. Build the project

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

CMake generates the Termscript parser from `termscript/Termscript.g` with `scripts/aurocks.pl`. The standard library is built from the sources in `third_party/tomlc99` and `third_party/isocline`.

## 2. Run Termscript

A Termscript program uses module calls such as `G:puts`:

```termscript
const message = "hello from Termlib";
G:puts message;
```

Run it with:

```sh
build/bin/termscript hello.ts
```

The VM captures `G:puts` output and writes it to standard output through the driver.

## 3. Generate and link C

Emit C without compiling it:

```sh
build/bin/termscript -c -o hello.c hello.ts
```

The generated source contains ordinary C control flow and calls to the compiled runtime. A typical static link uses:

```sh
cc -I. -Itermscript hello.c \
  build/libtermscript.a \
  build/termscript/stdlib/libtermscript_stdlib.a \
  build/libtermscript_standalone.a \
  build/libtermlib.a -lpthread -o hello
```

The driver can perform this compilation and link step directly when invoked without `-c` and with an output executable option.

## 4. Use the C API

```c
#include "termlib.h"

DT_Error error;
DT_PTYOptions options;
dt_error_clear(&error);
dt_pty_options_init(&options);

/* Fill options.argv, then call dt_pty_spawn(&options, &error). */
```

Initialize every options structure with its initializer. Check the returned `DT_Status`, and release every owned handle with its matching cleanup function.

`DT_ERR_EOF`, `DT_ERR_TIMEOUT`, and `DT_ERR_WOULD_BLOCK` describe normal I/O conditions. Do not collapse them into `DT_ERR_IO` in application code.

## 5. Use the C++ wrapper

```cpp
#include "termlib.hpp"

termlib::ErrorState error;
DT_TIDB *raw = nullptr; // C handles remain available when needed.
auto database = termlib::TIDB::open_default(error.get());
if (!database) {
    // error.value.message contains the diagnostic.
}
```

C++ wrapper objects are non-copyable and movable. Factory functions return `std::unique_ptr`; destruction closes the underlying C handle.

## 6. Terminfo and replay

Terminfo profiles expose typed capability lookup and bounded parameter expansion. Recordings use a versioned little-endian format with timestamps and CRCs. Replayers support real-time, fast, step, and paused modes.

For detailed contracts, see the corresponding chapters in [`manual/`](manual/).
