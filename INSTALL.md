# Building and installing

Build on Linux with a C/C++20 compiler, CMake, pkg-config, Python 3, Perl,
Meson, Ninja, Autoconf, Automake, Libtool, gettext, gperf, and Make. The build
also needs development files for unvendored platform dependencies such as
pixman, libdrm, libseat, libudev, libdisplay-info, EGL/GBM, ncursesw, libffi,
expat, libxml2, uuid, mtdev, XCB, zlib, libpng, bzip2, Brotli and libyaml
(`yaml-0.1`, the unvendored backend dependency of libcyaml).

```
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Configuration builds Wayland, wayland-protocols, libxkbcommon, libinput,
FreeType, HarfBuzz and Fontconfig from `third_party/` into `build/vendor`.
Fontconfig's Autotools files are generated in a build-local source copy.
wlroots builds against that prefix. No third-party source files are modified,
and configuration does not fetch dependencies. See `build/vendor-build.log`
if configuration fails. `DIFTRAY_BUILD_JOBS` controls dependency build parallelism.

A repository-local Meson/Ninja environment at `.tools/meson-env/bin` is detected
automatically; otherwise install those tools on PATH. An optional local gperf
executable can live at `.tools/gperf/usr/bin/gperf`.

The headless test creates a local Wayland socket and requires access to PTYs.
A sandbox that denies socket creation cannot run that test. Other tests use
logic-only mode or isolated terminal/Notelet components.

```
cmake --install build --prefix "$HOME/.local"
```

Installation includes private shared libraries in `lib/diftraywm`, with relative
runtime search paths, plus configuration, themes, help pages and Notelet bundles
under `share/diftraywm`. Font files and kernel/device support remain host-provided.

Configuration discovery checks the explicit `DIFTRAYWM_CONFIG` file first.
Otherwise it searches `$XDG_CONFIG_HOME/diftraywm` (or `~/.config/diftraywm`),
the working directory, then the executable's sibling `share/diftraywm` directory.
Within each directory, preference is `diftray.yaml`, `diftray.yml`,
`diftray.toml`, then `diftray.conf`. An invalid selected file is reported rather
than skipped. If no file exists, built-in defaults apply. Theme and help paths
resolve relative to the selected configuration file.

YAML is parsed by vendored libcyaml; TOML by vendored tomlc99; the original DSL
by PEGTL. All three share validation. Unknown fields, duplicate settings,
unsupported structures, unreadable files and malformed values are errors;
failed loads leave the previous configuration intact. Files are limited to
1 MiB. Copy `examples/diftray.yaml` or `examples/diftray.toml` into your config
directory. In YAML, quote hex colors because `#` introduces a comment.

LibShell is the default terminal shell. Set `terminal.shell` to an executable
path for an override, or use `set shell libshell` to restore the embedded shell.
Lua and native plugin setup is documented in `EXTENSIONS.md`.

For a headless development session, create a private runtime directory first:

```
mkdir -p /tmp/diftray-runtime
chmod 700 /tmp/diftray-runtime
XDG_RUNTIME_DIR=/tmp/diftray-runtime WLR_BACKENDS=headless \
  WLR_HEADLESS_OUTPUTS=3 WLR_RENDERER=pixman ./build/diftray
```

Use a TTY or a running Wayland/X11 session for interactive display testing.
SIGINT and SIGTERM shut down the compositor and its terminal workers.
