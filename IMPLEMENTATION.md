# Implementation status

DiftrayWM runs with real wlroots headless outputs and has 24 CTest targets. The
complete AGENTS.md specification is still in progress; passing tests does not
establish that every desktop application or physical GPU backend works.

Implemented behavior includes per-output geometry, navigation, ownership
migration, workspace switching, hot-unplug recovery, Notelet workers, PTY shells,
configuration programs, native plugins, Lua extensions, and the diftrayctl control
socket. YAML, TOML and PEGTL configuration formats remain supported. The vendored
dependency chain supplies the compositor and terminal libraries.

## Responsiveness and rendering fixes

- Control requests and responses use nonblocking event-loop callbacks. Reads,
  writes and accepts have bounded batches; silent readers and writers expire.
  A second session cannot unlink a live control socket, and failed startup
  preserves unrelated files. Socket shutdown only removes the owned inode.
- PTY reads yield after 64 KiB. Reaping a shell does not close its output watcher
  before buffered output drains. Restarting a terminal releases its previous
  PTY, and hidden terminals avoid unnecessary rasterisation on output events.
- Fonts reload transactionally and invalidate old glyphs. HarfBuzz shapes
  compatible terminal runs, including ligatures and combining sequences, before
  FreeType rasterisation. Synthetic bold and italic, underline, reverse video,
  hidden cursors, wide-character continuation cells, and Unicode fallback for
  compositor chrome are handled. Raster-cache memory is bounded.
- Terminal backgrounds, foregrounds, cursor color/thickness and cell highlights
  are theme properties. Alpha is premultiplied for scene rectangles and pixel
  buffers. Application-specified ANSI colors retain their own values.
- CSS comments, custom properties and variable fallbacks work. Invalid syntax,
  metrics, variable cycles and oversized themes fail without changing the live
  theme. Configuration reload stages the theme and font before committing.
  Failed keymap reloads retain the previous bindings and profile.
- Opening cells animate through actual scene opacity, including their borders.
  CSS opacity keyframes, named timing functions, delays and fill modes are
  supported. Reloading or disabling animations resets opacity, and closing cells
  removes their animation callbacks. The default and light themes enable a
  short opening animation.
- Layout rounding assigns leftover pixels to the last cell/view. Duplicate cell
  insertion is rejected. Removing a cell from another stack preserves the
  selected stack's index.
- Unused view rendering/input hooks and placeholder protocol/damage functions
  were removed. Compositor input/focus dispatch and the wlroots scene remain the
  active implementations.
- Launcher processes inherit their originating NCursor identity. Delayed and
  additional windows retain that owner after workspace changes; background
  windows do not switch workspaces or outputs. Map-time keyboard focus follows
  per-surface visibility and the compositor's selected window, and switching
  away clears the client's activated state. Reloading the word pool updates new
  identifiers while preserving live identifiers and uniqueness.
- Input paths use Linux key codes consistently, adding XKB's offset only when
  querying XKB. Terminal and editor keys repeat with INI-configured rate/delay;
  releases, focus changes, cell removal and reload stop pending repeat. Command
  Bar and help searches accept bounded UTF-8 text and erase whole code points.

## Validation

The suite covers three real headless outputs, output rotation/scale/placement,
scene animation start/completion, focus and ownership migration, Notelet worker
cancellation, configuration and extension lifecycle, real shell PTYs, terminal
input/output backpressure, and diftrayctl commands against a running compositor.
New regressions exercise large partial control replies, stalled clients, socket
ownership, font reloads, text attributes, transparency, CSS variables/keyframes,
transactional theme/config reloads and retained keymaps.

Physical monitors, GPU rendering, real keyboard devices and subjective animation
smoothness require interactive testing on the intended hardware.

## Remaining specification gaps

- LibShell's complete interactive job control and advanced line editing remain
  incomplete; external shell overrides are available.
- NTerm currently uses libtsm for parsing and its rendered grid. The specified
  libtsm decoded-event adapter into a libvterm-owned buffer is not implemented.
- Run shaping does not implement a complete bidirectional layout engine or a
  fallback chain that splits a mixed-font run. Color emoji and terminal blink
  animations also need further work.
- CSS rendering currently supports root properties and opacity animations for
  opening terminal cells. Arbitrary selector styling, transforms, transitions,
  repeated/reversed animations, radii and shadows remain unsupported. Animation
  durations/delays are bounded to 60 seconds; unsupported animation options
  report an error. Graphical-window opening/docking animations are not connected.
- Docking hides graphical clients but does not suspend their process trees.
  Grouping multiple graphical surfaces and ownership for applications that
  delegate window creation to an existing service still need expansion.
- The external output-management protocol is not implemented. Configuration and
  Command Bar controls manage outputs; workspace switching is global.
- The supplied Lua runtime is 5.5 rather than the 5.4 named in AGENTS.md; Kaguya
  uses a local compatibility adapter.

This list records remaining work, not a claim that all undiscovered bugs have
been eliminated.
