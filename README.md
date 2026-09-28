# DiftrayWM

DiftrayWM is a cell-based Wayland compositor. The desktop is organised into
typed views rather than freely floating windows:

- **NCursor** — stacked NTerm cells (keyboard-only notebook layout)
- **GCursor** — fullscreen graphical xdg-shell clients, dockable by word-id
- **TCursor** — a single NTerm cell promoted to fill the output

## Build

The build compiles the vendored dependency chain into `build/vendor`; see
[INSTALL.md](INSTALL.md) for host build tools. From this directory:

```sh
cmake -S . -B build
cmake --build build -j
```

From the Domweave root (enabled with `-DDOMWEAVE_BUILD_DIFTRAY=ON`):

```sh
cmake -S . -B build -DDOMWEAVE_BUILD_DIFTRAY=ON
cmake --build build -j --target diftray
```

Both `diftray` and `diftraywm` are produced.

## Run

Run from a TTY or nested inside another compositor. A Wayland socket is
advertised as `WAYLAND_DISPLAY`.

```sh
./build/diftray
```

Headless:

```sh
XDG_RUNTIME_DIR=/tmp/diftray-runtime \
WLR_BACKENDS=headless \
WLR_HEADLESS_OUTPUTS=1 \
WLR_RENDERER=pixman \
./build/diftray
```

## Bindings

- `Meta+Escape` quit
- `Meta+Tab` cell select mode
- `Meta+Up` / `Meta+Down` reorder cells
- `Meta+K` request cell removal; `y`/`Enter` confirms, `n`/`Escape` cancels
- `:` cell command bar (`spawn above`, `spawn below`, `kill`, …)
- `Meta+:` NCursor-global command bar
- `Meta+D` launcher
- `Meta+N` additional NCursor
- `Meta+Return` toggle TCursor
- `Meta+F1`–`Meta+F4` quick-restore docked GCursors
- `Meta+Left` / `Meta+Right` cycle NCursor or GCursor tabs
- `Meta+1`–`Meta+9` switch workspace
- `Meta+0` workspace 10
- `Meta+[` / `Meta+]` focus the previous / next monitor
- `:help` / `:h` opens the built-in Unix-manpage-style Help Pager

Graphical clients launched from a cell or the launcher are promoted to a
GCursor. The default `gcursor_mode = tab` shows one at a time;
`gcursor_mode = stack` splits the output into equal width columns. Click a
stacked window to give it keyboard focus. Minimising docks it into its
launching cell's Cursor Area.

## Multiple monitors

Every connected output gets its own NCursor. Tabs, terminal geometry, graphical
views, and remembered focus are per monitor. Workspace switching is global and
creates a terminal on each output the first time that workspace is visited.
Hot-unplug migrates views to a surviving monitor; reconnecting after all outputs
were removed restores the desktop. Logical output coordinates account for scale
and transform.

```
output list
output focus next
output focus HDMI-A-1
output move DP-1
ncursor list
cell move <cell-id> <ncursor-id>
cursor move <cursor-id> cell <cell-id>
cursor move <cursor-id> ncursor <ncursor-id>
```

`output move` moves the active NCursor and its owned GCursors. The source monitor
gets a replacement NCursor if needed. The command/status bar follows keyboard
focus. `ncursor list` lists cell IDs as well as monitor/workspace ownership.

## Appearance

`diftray.yaml`, `diftray.toml`, or the original `diftray.conf` sets terminal font,
window layout, and initial colors. See [INSTALL.md](INSTALL.md) for discovery
and [examples](examples) for YAML/TOML configurations. Its
`theme` setting names a CSS file relative to the configuration file. The
shipped `themes/default.css` defines `border-size`, `command-bar-height`,
`status-bar-height`, `border-color`, `background-color`, and
`command-bar-color` under `:root`. Colors use `#RRGGBB` or `#RRGGBBAA`;
dimensions use positive pixel values. Run `theme load <path>` in the command
bar to apply a file without restarting. Other CSS properties and animations
are not currently rendered by the compositor.

## Notelets

Notelets are small Termscript applications displayed in NCursor cells. Use
`:notelet list`, `:notelet open hello`, `:notelet refresh`, and `:notelet close`. The bundled
`hello` example is built into `build/notelets/hello.notelet`. Also included are
`scratchpad`, a Unicode text notebook, and `desktop`, a monitor/workspace/cursor
inspector. Notelet scripts run in asynchronous worker processes with a one-second
execution deadline; failures retain the previous successful frame and state. Set
`DIFTRAY_NOTELETS_PATH` to colon-separated directories and/or `.notelet`
archives to replace the default search path (user data directory followed by
the built-in bundles). Earlier directories take precedence for duplicate
names. Empty entries are ignored.

See [notelets/README.md](notelets/README.md) for packaging and the Termscript
authoring API.

## Help

`:help` opens the help index; `:help help-pager` explains pager navigation and
`:help notelets` documents Notelets. `:help find <regex>` searches the open
page with Oniguruma regular expressions. While reading, `/` opens an in-page
search, `n` and `?` move between results, and `q` or `Escape` closes the pager.
`help_key_*` entries in `diftray.conf` rebind these keys; `help_path` can point
at colon-separated directories of additional `page.1` manual pages.

## Implementation status

The compositor runs with multiple headless outputs and the repository test suite
covers navigation, ownership migration, Notelet workers, terminal display and
input backpressure. See [IMPLEMENTATION.md](IMPLEMENTATION.md) for the remaining
specification gaps and dependency constraints. This is not yet a complete
implementation of every contract in AGENTS.md.

## Shell and extensions

NTerm defaults to the embedded LibShell. `set shell /path/to/shell` selects an
external shell, and `set shell libshell` restores the embedded implementation.
Lua/Kaguya extensions and dynalo native plugins can register Command Bar
commands and subscribe to input, view and frame events. See
[EXTENSIONS.md](EXTENSIONS.md) and [examples/notebook.lua](examples/notebook.lua)
for Notelet command shortcuts.
