# Extensions

Eight Lua extensions, each registering one Command Bar command. They are
installed by copying this directory to `$XDG_CONFIG_HOME/diftraywm/extensions`
(default `~/.config/diftraywm/extensions`) or pointing
`DIFTRAYWM_EXTENSION_PATH` at it. The compositor loads `*.lua` in filename
order and gives each file its own Lua state.

```sh
cp extensions/*.lua ~/.config/diftraywm/extensions/
```

| Command | File | What it is for |
|---|---|---|
| `:ac` | `autocomplete.lua` | Completion database and command palette |
| `:hist` | `history.lua` | Command journal with inverse-aware undo |
| `:remap` | `keyremap.lua` | Chord table and keymap INI workbench |
| `:macro` | `macro.lua` | Named command macros |
| `:note` | `notepad.lua` | Cell and GCursor notebook |
| `:pulse` | `pulse.lua` | Session event sampler |
| `:skin` | `skin.lua` | Theme and display presets |
| `:ws` | `workspace.lua` | Labelled workspace board |

## What these extensions can and cannot do

The sandbox decides the shape of all eight, so it is worth stating once.

**No file system.** There is no `io` and no `os`. An extension cannot read or
write `keymap.ini`, cannot open a theme file, and cannot keep state across a
restart. Anything that sounds like persistence is really a table in memory that
lives as long as the compositor does.

**No clock.** No `os.time`, so nothing here measures seconds. Where recency or
timing matters, it is measured in *events* — `view`, `input` and `frame` hooks
advance a tick. A tick is not a millisecond, and the extensions that use one say
so rather than presenting a tick count as a duration.

**No view of compositor state.** An extension is told *that* a layout changed,
never what changed. So none of these can answer "which workspace am I on" or
"where is that GCursor docked". The ones that need that information either ask
the compositor and let its answer land on the status line (`:note ls`,
`:skin show`) or say plainly that they do not know (`:ws` calls its marker
"last touched", not "current").

**Nothing is dispatched recursively.** `diftray.command` queues, and the queue
drains 16 commands per tick on the compositor's loop. Every extension here
queues rather than dispatching, which is why a macro cannot re-enter the command
bar while running and why a 32-step limit is a real ceiling rather than an
arbitrary one.

**Neither `pcall` nor `setmetatable`.** A failure in a callback is caught by the
host and reported; a hook that throws loses that event's hooks for that script.
`tests/extension_scripts.cpp` fires all three events after loading every script,
so that failure mode is caught in CI rather than discovered as a dead extension.

## The eight

### `:ac` — autocomplete and palette

`:ac key` finds lines beginning with `key`, `:ac find theme` finds lines
containing it, and `:ac pick 2` runs the second candidate. Ranking is
frequency divided by time-since-use, both in ticks. Roughly 79 built-in Command
Bar commands ship preloaded with one-line descriptions, so the palette is useful
before you have taught it anything; `ac add` teaches it your own lines.

### `:hist` — journal and undo

`:hist record spawn below` adds an entry; `:hist undo` reverses it. The
interesting part is the refusal: `spawn` and `kill` are recorded but never
reversed, because undoing a spawn would mean killing a cell the extension cannot
identify. `:hist undo --dry` prints the plan without dispatching, and a request
that cannot be satisfied in full changes nothing rather than undoing half of
itself.

### `:remap` — key remapping workbench

`:remap add <M-f3> spawn below` declares a chord; `:remap write` prints the
whole INI, sections and all, ready to paste. The chord table is validated
locally against the same names as `kNamedKeys` in `src/keymap/Keymap.cpp`, and
the test suite cross-checks that table against `keymap chord` in both
directions — an extension that accepted a chord the compositor rejects, or
rejected one it accepts, would be worse than no extension.

`:remap prefix` and `:remap metaprefix` refuse to be set to the same chord. The
two prefixes answer different questions, and sharing one means whichever ran
first swallows the key.

### `:macro` — named sequences

`:macro save build`, `:macro add build spawn below`, `:macro run build`. This is
the session-time sibling of the config DSL's `macro` keyword: same shape, no
compile-time binding. Bind it with `bind("Meta", keysym("F5"), "macro run build")`.

### `:ws` — workspace board

Ten labelled workspaces: `:ws mark 3 editor`, `:ws find ed`, `:ws next`. The
board is what you told it, not what the compositor did, and it does not pretend
otherwise.

### `:skin` — theme presets

`:skin set big border-size 1px`, `:skin apply big`. Presets are CSS, handed to
`set theme`, so the compositor still owns every value. Property names are
hyphenated to match `themes/default.css`; `:skin keys` lists the ones actually
read, and an unknown property is called out rather than stored as a token
nothing consumes.

### `:note` — cell and cursor notebook

`:note ls` prints the live identifiers; `:note watch 3af1 vim` names one;
`:note pin browser F1` binds a named GCursor to a Quick Restore key. It performs
the one operation available without knowing where anything is, because a
Quick Restore slot is absolute.

### `:pulse` — session sampler

Counts frames, layout updates and keypresses, and reports the gaps. A `frame`
event fires once per output per drawn frame, so a gap is not a duration and the
report says so. This is the one extension that uses the event hooks as its
substance rather than as a clock substitute.

## Tests

```sh
cmake -DBUILD_TESTING=ON ..
make diftraywm_extension_scripts_test
./tests/diftraywm_extension_scripts_test
```

`tests/extension_scripts.cpp` loads every file in this directory into the real
engine, asserts each registers its command, then drives each subcommand through
the Command Bar. A syntax error, a call to a function the sandbox removed, or a
pluralisation bug in a status line fails there rather than on a desktop.
