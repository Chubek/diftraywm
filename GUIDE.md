# Using DiftrayWM

Start `build/diftray` in a graphical session for a nested desktop, or from a TTY
for hardware outputs. Each monitor starts with an NCursor containing one shell.

Press `:` to open the cell Command Bar, or `Meta+:` for the NCursor's global bar.
The bar draws as an overlay: opening it never resizes the terminal, so the
shell prompt and any half-typed line stay exactly where they are.
`spawn above` and `spawn below` create cells. `Meta+Tab` enters cell selection;
arrow keys select cells and `Meta+Up/Down` reorder them. Enter focuses the
selection. `Meta+K` asks before removing it. `Meta+Return` expands the cell into a
TCursor and restores it on a second press.

The Logo key is Meta, and `Ctrl+Q` works as Meta too: press and release it,
then press the next key, which behaves as if Meta were held. `Escape` cancels
an armed prefix. `Composite("Meta", "Shift")` builds composite modifier sets
for `bind()`.

Every key — the prefix chord, the help pager keys, the built-in bindings, and
any remapping you want — is configured in an INI file rather than in the
config file. See "Key remapping" below.

## The launcher taskbar

The launcher is a bar anchored under the status bar, drawn above the cells and
above graphical windows. It shows the live NCursors and GCursors of the current
workspace, with the active one in brackets, so it doubles as a taskbar. When
it has focus it also shows the `launch> ` prompt.

`Meta+D` opens the launcher there. It is otherwise hidden, unless it is
locked. Locking pins it to the top of the screen for the rest of the session:

```
:launcher lock        or   diftrayctl lock-launchbar
:launcher unlock      or   diftrayctl unlock-launchbar
:launcher toggle      or   diftrayctl toggle-launchbar
:launcher status      or   diftrayctl launchbar-status
```

`launcher lock` and `launcher unlock` report when the bar is already in the
requested state, and the current lock is remembered by `config reload`. Set
`launcher_locked = true` in `general` to start locked. The bar's height and
colour come from `launcher_bar_height` and `launcher_bar_color`, or from the
`launcher-bar-height` and `launcher-bar-color` properties in a CSS theme.

`Meta+N` opens another NCursor. `Meta+Left/Right` selects tabs on the current
monitor. `Meta+1` through `Meta+0` switches the shared workspace. `Meta+[` and
`Meta+]` change monitor focus. The `output list`, `ncursor list`, and
`cursor list ids` commands expose identifiers for movement and scripting.

Run `output move <monitor>` to transfer the active NCursor. Run
`cell move <cell-id> <ncursor-id>` to move an individual terminal or Notelet.
GCursors owned by that cell follow it. Disconnecting a monitor migrates its
views to a surviving output. `output rotate <monitor> 90` rotates counter-clockwise;
0, 180 and 270 are also supported. `output scale <monitor> 1.25` changes scale,
and `output position <monitor> <x> <y>` sets logical placement (`auto` restores
automatic placement). [MONITORS.md](MONITORS.md) documents persistent settings
in YAML, TOML and the DSL.

Graphical applications launched by a shell become GCursors. `Meta+D` opens the
launcher. `cursor dock <id>` hides a GCursor; `cursor restore <id>` returns to it.
`cursor assign <id> F1` binds `Meta+F1` to it. F2 through F4 work similarly.
`cursor move <id> cell <cell-id>` and `cursor move <id> ncursor <ncursor-id>`
change docking ownership.

Try `notelet open scratchpad` for transient notes and `notelet open desktop` for
monitor, workspace, and cursor information. `notelet refresh` refreshes a snapshot;
`notelet close` removes the cell. Notelet state stays with the cell through
moves and workspace changes, and disappears when the cell closes.

The default shell is embedded LibShell. `set shell libshell` restores it.
`set shell /path/to/shell` restarts the scoped shell. In the global Command Bar it
updates the active NCursor's inherited shell settings, including future cells;
individual overrides retain precedence.

The builtin multiplexer tiles one NCursor with terminal panes. `mux split
horizontal` stacks a new pane below (`Meta+S`); `mux split vertical` opens a
pane beside it (`Meta+V`). `mux focus next` and `mux focus prev` cycle panes
(`Meta+O` and `Meta+P`); `mux focus up|down|left|right` moves directionally
(`Meta+Ctrl` plus the arrow key). `mux kill` removes the focused pane and
`mux zoom` expands it to a TCursor and back (`Meta+Z`). `mux list` shows the
panes with the focused one marked `*`. All `Meta` shortcuts also work through
the `Ctrl+Q` prefix.

Themes are CSS property blocks. `theme load <file.css>` applies a file and
`theme show` lists the active properties; `set theme :root { ... }` applies
inline CSS. The properties `border-color`, `background-color`,
`command-bar-color`, `border-size`, `command-bar-height`, and
`status-bar-height` restyle the live compositor. `themes/default.css` is the
dark default and `themes/light.css` is a light example. `help` opens the
manual pager; `help commands` lists commands.

Terminal defaults can be styled with `terminal-background-color`,
`terminal-foreground-color`, `terminal-cursor-color`,
`terminal-cursor-thickness`, and `highlight-color`. Colors may include alpha.
CSS comments, root custom properties and `var(--name, fallback)` work; invalid
themes and configuration reloads retain the previous working appearance.

The shipped themes animate opening terminal cells with opacity keyframes:

```css
:root { animation: cell-appear 140ms ease-out; }
@keyframes cell-appear {
  from { opacity: 0; }
  to { opacity: 1; }
}
```

Use `animation: none;` to disable this. Keyframes can include intermediate
percentages, and duration, delay, timing-function and fill-mode longhands are
available. Supported timing functions are `linear`, `ease`, `ease-in`,
`ease-out` and `ease-in-out`. Other CSS animation types remain unsupported;
see [IMPLEMENTATION.md](IMPLEMENTATION.md) for the current limits.

## Key remapping

All key configuration lives in one INI file, in the format `keyd` uses. The
config file only names it:

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
<C-x> = Diftray(workspace 3) # a Command Bar command
<C-z> = Typeout(git status)   # type into the focused cell
<C-m> = Ignore()              # swallow the key

[profile-1]
<C-f8> = Exec(kitty -m tmux)  # run a program
<M-q>  = Typeout(foobar)      # type text
```

A chord is a key plus exactly the modifiers written: `<C-q>`, `<C-S-q>`,
`<M-q>` (Meta), `<G-q>` (Logo), `<f8>`, `<space>`, `<leftbrace>`, or a bare
`q` / `;` / `5`. The actions are `Diftray(command)`, `Exec(command)`,
`Typeout(text)`, `Trigger(profile)`, `Remap(<chord>)` and `Ignore()`.

There are two prefixes because they answer different questions. `[init] prefix`
switches the active profile — a device-level idea, so a system-wide remapper
honours it too. `[meta] prefix` lends Meta to the next key — a session-level
idea only the compositor has. Giving them the same chord would be ambiguous, so
keep them distinct.

The shipped `keymap.ini` has the compositor's built-in bindings written out as
commented-out lines in `[default]`, so you can read, change or delete them. An
unknown chord or action is an error with a line number, never a silently
ignored line.

The `[diftray]` section also accepts `repeat_rate` (0–100 characters per second)
and `repeat_delay` (0–5000 milliseconds). Defaults are 25 and 600; a rate of 0
disables repeat. Terminal keys, text editors and pager navigation can repeat.
Launch, kill and workspace shortcuts execute once. Changing focus or releasing
the key stops pending repeat. Command Bar and help search text accept UTF-8;
backspace removes the last Unicode code point.

```
:keymap show          or   diftrayctl keymap-show
:keymap reload        or   diftrayctl keymap-reload
:keymap check <path>  or   diftrayctl keymap-check <path>
:keymap profile <n>   or   diftrayctl keymap-profile <n>
:keymap reset         or   diftrayctl keymap-reset
:keymap chord <spec>  or   diftrayctl keymap-chord <spec>
```

`keymap check` validates a file without adopting it, which is how to diagnose an
edit before reloading it. A broken keymap is reported and the previous one kept,
so a typo cannot lock you out.

Bindings written with `bind()` in a configuration program still win over a
keymap profile, and the `[meta]` prefix is checked before the profiles, so a
profile cannot swallow the one chord that makes Meta reachable.

### System-wide

The compositor applies the keymap to the keys it already reads, which needs no
privileges and affects only the DiftrayWM session. `diftrayremap` applies the
same file to the whole machine: it grabs the physical keyboard and republishes
it through `uinput`, so the remapping also reaches a TTY or another session.

```
diftrayremap check     # can it run, and why not if it cannot
diftrayremap list      # the keyboards [devices] resolves to
diftrayremap run       # remap until interrupted
```

It needs access to `/dev/input/event*` and `/dev/uinput`, which normally means
running it as root. It is a separate program on purpose: a compositor that
grabbed the keyboard would take it from the session it is nested inside, and
would require every user to run their window manager as root. Run `check`
first — it prints the exact reason rather than failing halfway. See
`diftrayremap(1)` and `diftraywm-keymap(5)`.

## Driving a running session with diftrayctl

`diftrayctl` manages a compositor that is already running. It talks to a Unix
control socket the compositor publishes at
`$XDG_RUNTIME_DIR/diftraywm-ctl-$UID.sock` (mode 0700), and every command it
sends is a Command Bar command, so anything you can do after `:` you can also
do from a shell.

```sh
diftrayctl status                        # session, cells, plugins, theme
diftrayctl exit-session                  # leave the session for the console
diftrayctl restart-session               # re-exec the compositor in place
diftrayctl plugin-load ./libpanel.so     # load a native plugin with dynalo
diftrayctl plugin-list                   # what is loaded
diftrayctl extension-exec ./panel.lua    # load and run a Lua extension
diftrayctl config-open                   # open the active config in $EDITOR
diftrayctl config-reload                 # re-read the config file
diftrayctl source-script demo.tsc        # run a Termscript file
diftrayctl mux-split vertical            # split the focused pane
diftrayctl workspace 3                   # switch workspace
diftrayctl bar "curses escape"           # any Command Bar command, verbatim
```

`restart-session` re-executes the compositor in place, so the process id, the
environment and the original arguments are preserved. `config-reload` is
atomic: a broken file is refused and the running configuration is kept. Paths
containing spaces are handled correctly, and `source-script` prints whatever
the script emits with `G:puts`.

The client exits 0 when the command ran, 1 when the compositor rejected it, 2
on a usage error, and 3 when no session is reachable; handlers also report
their own failures in the printed text. `diftrayctl --help` lists every
subcommand and `help diftrayctl` in the in-session pager documents it in full.
