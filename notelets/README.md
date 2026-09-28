# Diftray Notelets

A Notelet is a `.notelet` **ustar** archive named `[a-z0-9_-]+.notelet`.
It contains `main.tsc` (your Termscript program), `notelet.tsc` (the shipped
authoring library), and optional text files under `assets/`. The archive is
generated with the repository's packer:

```sh
python3 scripts/bundle-notelet.py notelets/examples/hello /tmp/hello.notelet
DIFTRAY_NOTELETS_PATH=/tmp ./build/diftray
```

In Diftray, run `:notelet list`, `:notelet open hello`, then press keys in
the Notelet cell. `:notelet close` removes it. Other cell and workspace
commands still work. A Notelet cell does not spawn a shell or a PTY.

Each render executes the bundled library followed by `main.tsc` in a fresh
DomTERM Termscript VM. `G:puts` writes lines to the cell; the previous frame
is replaced. The authoring library binds `notelet` to the native module,
`notelet_key` to the last key, and `notelet_name` to the bundle name.

| Call | Result |
|---|---|
| `notelet:key` | Last key: printable character, `Enter`, `Backspace`, `Escape`, or arrow name; empty on opening |
| `notelet:name` | Bundle name |
| `notelet:resource "message.txt"` | Text in `assets/message.txt`, or `nil` |
| `notelet:get "name"` | Persisted string, or `nil` |
| `notelet:set "name" "value"` | Persist a string for later renders |
| `notelet:erase "name"` | Remove a state entry; returns whether it existed |
| `notelet:event` | `open`, `key`, `resize`, `refresh`, `move`, or `outputs` |
| `notelet:context "columns"` / `"rows"` | Cell dimensions as strings |
| `notelet:context "cell"` / `"workspace"` / `"output"` | Instance location |
| `notelet:context "rotation"` / `"scale"` | Owning monitor rotation in counter-clockwise degrees and scale, as strings |
| `notelet:context "output_width"` / `"output_height"` | Owning monitor logical dimensions as strings |
| `notelet:context "outputs"` / `"cursors"` | Desktop snapshots |
| `notelet:edit "name"` | Edit a state string using the current key; supports Unicode, Enter, Tab, and Backspace |

See [examples/hello/main.tsc](examples/hello/main.tsc) for a working program.
Termscript's `G:load "std.*"` and `G:import` give Notelets access to
DomTERM and the standard Termscript libraries. The compositor queues input and
runs each frame in a worker process, with a one-second wall-clock deadline and a
CPU limit. A failed or timed-out render leaves the last successful frame and
state intact. The input queue holds up to 64 waiting events. Workers and their
process groups are cleaned up after each render or when the cell closes.

Workers are an execution boundary, **not a security sandbox**. Install trusted
Notelets: the standard library can access files and run commands as your user.
Do not start long-lived processes from a frame; use the Command Bar's `launch`
command for graphical applications. State persists for the lifetime of the
Notelet cell, including workspace switches, output moves and hotplug. Closing
the cell discards it. `:notelet refresh` requests a fresh context snapshot.

The bundled `scratchpad` uses `notelet:edit` for transient notes, while `desktop`
uses the context API as an inspector. `hello` demonstrates resources and state.

Bundles are limited to 4 MiB, entries to 1 MiB, and rendered output to
64 KiB. `notelet:set` allows 128 keys with values up to 4 KiB. Bundles may
contain only the three entry types above, and are validated before use.

The `outputs` event refreshes every open Notelet after monitor geometry, rotation,
scale or connection changes, including a 180-degree rotation without a resize.


Configuration macros can compose Notelet commands with workspace and NCursor
operations. For example, define
`macro notebook(name) = commands("ncursor new", "notelet open " + name);`
and call `config run notebook("scratchpad")` from a Command Bar, or use
`notebook("scratchpad")` as a binding action. See
[configuration programs](../CONFIGURATION.md) for all three file formats.
