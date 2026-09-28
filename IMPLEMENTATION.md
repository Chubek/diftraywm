# Implementation status

Implemented and exercised in this change:

- Per-output logical geometry, separate visible NCursor/GCursor views, per-output
  tab focus, keyboard monitor focus, output movement and unplug migration.
  Workspace selection is shared across outputs.
- Command-based NCursor listing/creation and cell/GCursor ownership transfer.
  Cursor-area references are removed when clients disappear, and graphical
  clients can outlive their launching cell.
- DomTERM Notelet workers, queued events, execution deadlines, transactional
  state/frame updates, Unicode input, context snapshots and refresh/move/resize
  events. Bundled scratchpad and desktop-inspector applications.
- Vendored builds for the available mandatory libraries, private installed
  runtime libraries, shaped compositor chrome, and terminal font fallback.
- PTY backpressure handling, child reaping, shutdown signals, bounded shell
  termination, cell-removal confirmation and inherited shell scoping.

Validation: eight CTest targets cover existing behavior, multiple-output
navigation/movement, three actual wlroots headless outputs, asynchronous Notelet
rendering/cancellation, Unicode editing, terminal display and input backpressure.
Physical monitors, GPU backends and interactive input were not verified in this
session. `wm_dev_mcp` reported no available X11/Sway backend.

The full AGENTS.md specification is **not complete**. Known remaining work:

- `third_party/libshell`, `third_party/lua`, `third_party/kaguya`, and
  `third_party/dynalo` are absent. The current shell defaults to `/bin/sh`.
  Lua execution and native plugin command/event wiring remain placeholders.
  Implementing the prescribed integrations requires restoring these source
  trees or an explicit change to the dependency constraints.
- The terminal still feeds both libtsm and libvterm, and the renderer reads the
  libtsm grid. A decoded-event adapter is required to establish the exact
  libtsm-parser/libvterm-buffer contract without duplicate parsing.
- Terminal text is shaped per glyph; run-level terminal ligatures and complex
  script layout remain incomplete. Chrome is shaped as a full text run.
- CSS keyframes, transitions, radii and shadows are not rendered. Several
  terminal styling constants still need conversion to theme properties.
- Docking hides clients but does not yet suspend their process trees. Launcher
  ancestry tracking and grouping additional graphical surfaces need expansion.
- Monitor placement is automatic; output-management protocol configuration and
  independent workspace selection per monitor are not implemented.

This file records outstanding functionality; it is not a completion claim.
