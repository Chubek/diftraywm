# Using DiftrayWM

Start `build/diftray` in a graphical session for a nested desktop, or from a TTY
for hardware outputs. Each monitor starts with an NCursor containing one shell.

Press `:` to open the cell Command Bar, or `Meta+:` for the NCursor's global bar.
`spawn above` and `spawn below` create cells. `Meta+Tab` enters cell selection;
arrow keys select cells and `Meta+Up/Down` reorder them. Enter focuses the
selection. `Meta+K` asks before removing it. `Meta+Return` expands the cell into a
TCursor and restores it on a second press.

`Meta+N` opens another NCursor. `Meta+Left/Right` selects tabs on the current
monitor. `Meta+1` through `Meta+0` switches the shared workspace. `Meta+[` and
`Meta+]` change monitor focus. The `output list`, `ncursor list`, and
`cursor list ids` commands expose identifiers for movement and scripting.

Run `output move <monitor>` to transfer the active NCursor. Run
`cell move <cell-id> <ncursor-id>` to move an individual terminal or Notelet.
GCursors owned by that cell follow it. Disconnecting a monitor migrates its
views to a surviving output.

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
individual overrides retain precedence. `theme load <file.css>` reloads supported
visual properties. `help` opens the manual pager; `help commands` lists commands.
