# Diftray Notelets

A Notelet is a `.notelet` **ustar** archive named `[a-z0-9_-]+.notelet`.
It contains `main.tsc` (your Termscript program), `notelet.tsc` (the shipped
authoring library), and optional text files under `assets/`. The archive is
generated with the repository's packer:

```sh
python3 domutils/diftray/scripts/bundle-notelet.py \
  domutils/diftray/notelets/examples/hello /tmp/hello.notelet
DIFTRAY_NOTELETS_PATH=/tmp ./domutils/diftray/build/diftray
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

See [examples/hello/main.tsc](examples/hello/main.tsc) for a working program.
Termscript's `G:load "std.*"` and `G:import` give Notelets access to
DomTERM and the standard Termscript libraries. Scripts run on the compositor
event loop, so avoid blocking standard library calls. The VM bounds execution
steps, but blocking I/O in scripts can still stall the compositor.

Bundles are limited to 4 MiB, entries to 1 MiB, and rendered output to
64 KiB. `notelet:set` allows 128 keys with values up to 4 KiB. Bundles may
contain only the three entry types above, and are validated before use.
