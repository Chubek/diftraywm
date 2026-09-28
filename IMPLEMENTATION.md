# Implementation status

Implemented and exercised in this change:

- Per-output logical geometry, separate visible NCursor/GCursor views, per-output
  tab focus, keyboard monitor focus, output movement and unplug migration.
  Workspace selection is shared across outputs. Per-monitor rotation, fractional
  scaling and explicit/automatic placement are configured in YAML, TOML or the
  DSL, with live Command Bar updates and retained overrides on reconnection.
  Notelets automatically refresh their monitor snapshots on output changes.
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
- YAML/libcyaml and TOML/tomlc99 configuration, transactional validation,
  deterministic discovery, and retained PEGTL DSL support.
- Embedded LibShell with its supplied parser, expansions, builtins, pipelines
  and POSIX executor, adapted to retain NTerm's controlling terminal.
- Lua/Kaguya scripts with owned command registrations, bounded execution,
  deferred commands and input/view/frame hooks; native dynalo plugins with
  scoped commands, event subscriptions and failed-initialization rollback.

Validation: twelve CTest targets cover existing behavior, multiple-output
navigation/movement, three actual wlroots headless outputs with startup/live rotation, fractional
scaling, placement, focus preservation and Notelet output events, asynchronous Notelet
rendering/cancellation, Unicode editing, terminal display and input backpressure,
configuration formats, extension lifecycle/failure handling, and real PTY shell commands and interruption.
Physical monitors, GPU backends and interactive input were not verified in this
session. `wm_dev_mcp` reported no available X11/Sway backend.

The full AGENTS.md specification is **not complete**. Known remaining work:

- LibShell now runs commands inside the PTY session, but full interactive job
  control and advanced line editing are not implemented. External shell
  overrides remain available. The supplied Lua is 5.5 rather than the 5.4 named
  in AGENTS.md; a local adapter provides Kaguya compatibility.
- The terminal still feeds both libtsm and libvterm, and the renderer reads the
  libtsm grid. A decoded-event adapter is required to establish the exact
  libtsm-parser/libvterm-buffer contract without duplicate parsing.
- Terminal text is shaped per glyph; run-level terminal ligatures and complex
  script layout remain incomplete. Chrome is shaped as a full text run.
- CSS keyframes, transitions, radii and shadows are not rendered. Several
  terminal styling constants still need conversion to theme properties.
- Docking hides clients but does not yet suspend their process trees. Launcher
  ancestry tracking and grouping additional graphical surfaces need expansion.
- The external output-management protocol and independent workspace selection
  per monitor are not implemented. Configuration and Command Bar output controls
  are available.

This file records outstanding functionality; it is not a completion claim.


Configuration programs now provide immutable typed variables, distinct physical
keycodes and layout keysyms, eager functions, lazy expression macros, and scoped
bindings in all three formats. Setting expressions also cover monitor rotation.
The Command Bar exposes config vars/eval/run. Parser, evaluator, and command
recursion limits are enforced; examples and semantics are in CONFIGURATION.md.
The 13-test suite includes config evaluation, format equivalence, physical-key
precedence, scope restoration, Notelet macros, and headless multi-output tests.
