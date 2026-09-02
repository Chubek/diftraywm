# DiftrayWM

DiftrayWM is a cell-oriented wlroots Wayland compositor with PTY-backed terminal
cells, xdg-shell application surfaces, scene rendering, output management,
keyboard focus, graphical borders, and a graphical command-bar strip.

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

Both `build/diftray` and the compatibility name `build/diftraywm` are produced.

The build compiles the vendored wlroots tree through `scripts/build-wlroots.sh`.
Meson and Ninja must either be available on `PATH` or installed in
`.tools/meson-env`.

## Run

Run `./build/diftray` from a Linux virtual terminal or from another compositor.
It creates a Wayland socket and selects an appropriate wlroots backend. Set
`DIFTRAYWM_WAYLAND_SOCKET` to request a specific socket name.

For a reproducible headless run:

```sh
XDG_RUNTIME_DIR=/tmp/diftray-runtime \
WLR_BACKENDS=headless \
WLR_HEADLESS_OUTPUTS=1 \
WLR_RENDERER=pixman \
./build/diftray
```

Meta+Escape stops the compositor. Meta+colon toggles the graphical command-bar
strip.

## Configuration

DiftrayWM loads `diftray.conf`, or the path in `DIFTRAYWM_CONFIG`. The PEGTL
configuration DSL supports:

```text
general {
  border_size = 3
  border_color = #59a6ff
  background_color = #090c11
  command_bar_height = 40
  command_bar_color = #141f2efa
}

terminal {
  shell = /bin/sh
}
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```
