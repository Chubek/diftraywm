# Puppeteer Design

Puppeteer automates interactive terminal applications through PTY sessions. It is an expect-style subsystem layered over the runtime rather than a replacement for normal command execution.

The core model is a session that spawns a program, captures transcript output, waits for patterns, sends input, and reports timeout or process termination diagnostics.

Matchers should support literal strings and regular expressions. Each wait operation has an explicit timeout and returns the matched span, captured text, and current transcript offset.

Input injection is explicit: `send`, `send_line`, and future binary-safe variants. The subsystem must not implicitly answer prompts without a declared matcher and action.

Transcript logging is append-only and suitable for debugging. Sensitive data handling must be configurable so passwords or tokens can be redacted before logs are persisted.

Puppeteer sessions inherit shell environment and execution policy through validated runtime specs, while PTY allocation and terminal mode details remain encapsulated behind the Puppeteer API.
