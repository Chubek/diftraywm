# Extensions

Lua scripts are loaded in filename order from
`$XDG_CONFIG_HOME/diftraywm/extensions` (default
`~/.config/diftraywm/extensions`). `DIFTRAYWM_EXTENSION_PATH` overrides this
directory. Files are read at startup. Each script has an isolated Lua state and
uses the supplied Lua 5.5 runtime through Kaguya. No vendored files are patched;
`src/lua/KaguyaCompat.hpp` adapts Kaguya's older garbage collector API.

```lua
diftray.register_command("scratchpad", function(tokens, scope)
  diftray.command("notelet open scratchpad")
end)
diftray.on("view", function(event)
  diftray.status("Desktop layout changed")
end)
```

`register_command(name, callback)` adds a Command Bar command. `tokens` is a
one-based array including the command name; `scope` is `cell` or `global`.
Built-in commands and existing registrations cannot be replaced.
`diftray.status(text)` updates the status message. `diftray.command(text)` queues
a global Command Bar command for the compositor event loop. The queue holds up
to 64 commands and drains up to 16 per tick, preventing recursive dispatch.
Use explicit cell IDs to target a cell; an untargeted queued command uses the
focus at dispatch time.

`diftray.on(event, callback)` subscribes to `view` (layout updates), `input`
(keyboard events) or `frame` (successful output frame commits, once per output).
Callbacks receive the event name. A failing event hook disables that script's
hooks for the event. Failed startup scripts roll back registrations and queued
commands. Script destruction removes all owned commands.

Scripts can use base functions, tables, strings, math and UTF-8 helpers. They
cannot use `io`, `os`, `package`, `debug`, coroutines, file loaders, protected
calls, metatable manipulation, printing or explicit garbage collection. Lua VM
execution is checked every 1,000 instructions and limited to 100,000 instructions
or 10 milliseconds per invocation. Each state has a 16 MiB allocation limit;
source files are limited to 1 MiB. These limits do not preempt a running C library
function. Put substantial computations or I/O in a Notelet worker or an external
program. These restrictions support responsive extensions; they are not a
security boundary for untrusted code.

# Native plugins

Native plugins export all three lifecycle functions from
`include/DiftrayWM-Plugin.h`. Initialization returns zero on success; the version
function returns a nonempty plugin version string. Discovery loads `.so` files
in filename order from `$XDG_CONFIG_HOME/diftraywm/plugins` or
`~/.config/diftraywm/plugins`. `DIFTRAYWM_PLUGIN_PATH` overrides the directory.
Loading and symbol resolution use the vendored dynalo headers.

The host implements logging, status messages, scoped command registration,
command removal and frame/view/input subscriptions. Call the host API only
while running initialization, cleanup or a host-invoked callback on the
compositor thread. Registered command callbacks receive tokens including their
command name. A command registered for cell scope cannot run from the global
bar, and vice versa. Registrations belong to their plugin: failed initialization
rolls them back, and unloading removes them before releasing the library.

Plugins are trusted in-process code and must return promptly from callbacks.
They must not retain pointers to command tokens after a callback returns.
Cleanup runs in reverse load order. See `tests/extension_plugin.c` for a minimal
plugin covering every subscription.
