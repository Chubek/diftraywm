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

Both `diftray` and `diftraywm` are produced, along with `diftrayctl`, the
control utility for a running session, and `diftrayremap`, the system-wide key
remapper.

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

## Control a running session

`diftrayctl` sends Command Bar commands to a compositor that is already
running, over a Unix control socket at
`$XDG_RUNTIME_DIR/diftraywm-ctl-$UID.sock` (mode 0700).

```sh
./build/diftrayctl status              # session, cells, plugins, theme
./build/diftrayctl exit-session        # leave the session for the console
./build/diftrayctl restart-session     # re-exec the compositor in place
./build/diftrayctl plugin-load ./x.so  # load a native plugin (dynalo)
./build/diftrayctl extension-exec ./x.lua
./build/diftrayctl config-open
./build/diftrayctl source-script demo.tsc
./build/diftrayctl --help              # every subcommand
```

`DIFTRAYWM_CONTROL_SOCKET` overrides the socket path. See
[help/diftrayctl.1](help/diftrayctl.1) and the "Driving a running session"
section of [GUIDE.md](GUIDE.md).

## Key remapping

Every key is configured in one INI file, in `keyd`'s format. The config file
only names it; nothing about keys lives in `diftray.conf`.

```ini
general {
  keymap = keymap.ini
}
```

```ini
[init]
prefix = <C-g>              # the remapping layer's chord
action = Trigger(profile-1)

[meta]
prefix = <C-q>              # the compositor's Meta prefix

[default]
<C-x> = Diftray(workspace 3)  # a Command Bar command
<C-z> = Typeout(git status)   # type into the focused cell
<C-m> = Ignore()               # swallow the key
```

```sh
./build/diftrayctl keymap-show     # the loaded keymap and its bindings
./build/diftrayctl keymap-reload   # re-read it
./build/diftrayctl keymap-check ./new.ini   # validate without adopting
```

The compositor applies the file to the keys it already reads, which needs no
privileges. `diftrayremap` applies the same file to the whole machine, using
libudev to find the keyboards, libevdev to read them, and uinput to republish
them — so the remapping also reaches a TTY or another session. That needs
access to `/dev/input/event*` and `/dev/uinput`, so it is a separate program
rather than a compositor mode; run `diftrayremap check` first to see whether
this machine can do it, and why not if it cannot.

`[diftray] repeat_rate = 25` and `repeat_delay = 600` control held-key repeat
in characters per second and milliseconds. Set the rate to `0` to disable it.
Repeat applies to terminal/editor/navigation input; compositor action shortcuts
fire once. Command Bar and help search text support UTF-8 input and backspace.

See [help/keymap.1](help/keymap.1), [help/diftrayremap.1](help/diftrayremap.1)
and the "Key remapping" section of [GUIDE.md](GUIDE.md).

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
and transform. Configure per-monitor rotation, scale and position in any supported
config format; see [MONITORS.md](MONITORS.md). Rotation is counter-clockwise and
accepts 0, 90, 180 or 270 degrees. Live adjustments use `output rotate DP-1 90`,
`output scale DP-1 1.25`, and `output position DP-1 -1080 0`.

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
dimensions use nonnegative pixel values (or `0`). The shorthand colors `#RGB`
and `#RGBA` also work. Run `theme load <path>` in the command bar to apply a file
without restarting. CSS comments and root custom properties such as
`--accent: #59a6ff; border-color: var(--accent);` are supported.

Terminal styling uses `terminal-background-color`, `terminal-foreground-color`,
`terminal-cursor-color`, `terminal-cursor-thickness`, and `highlight-color`.
Themes affect default terminal colors while programs keep their explicit ANSI
colors. The light theme supplies a dark foreground for readability.

Opening cells use CSS opacity keyframes. The shipped themes specify
`animation: cell-appear 140ms ease-out;` and matching `@keyframes`; set
`animation: none;` to disable them. Longhand name, duration, delay, timing-function
and fill-mode settings are also supported. Timing functions include `linear`,
`ease`, `ease-in`, `ease-out` and `ease-in-out`. Transforms, transitions, repeated
animations, radii and shadows are not yet rendered.

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

To automate the active terminal cell, run `:terminal script path/to/file.tsc`
from its Command Bar. The script can use Termlib's Termscript standard library
and its `diftray.terminal` module:

```termscript
const terminal = G:load "diftray.terminal";
const text = terminal:screen;
G:puts text;
terminal:display "Hello from Termscript!";
```

`screen` captures the visible Unicode grid, `cursor` returns its zero-based
`row,column`, and `size` returns `rows,columns`. `display` renders content
without involving the shell; `send` writes input to the cell's PTY and returns
false if no PTY is attached. `running` reports whether the shell is active.
The compositor retains libtsm for the rendered VT grid and its color and
Unicode attributes; Termscript's `std.vterm` is also available as a separate
character-grid helper for scripts.

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

Configuration programs support typed keycodes and keysyms, variables, functions,
and lazy macros in DSL, TOML, and YAML. See [CONFIGURATION.md](CONFIGURATION.md)
for bindings, setting expressions, and Notelet macro examples.
