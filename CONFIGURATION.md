# Configuration programs

Diftray accepts the PEGTL-based `.conf` DSL, TOML, and YAML. Each format can
contain one optional `program`, with the same expression language and behavior.
Existing files without a program continue to work. Complete working examples
are in `examples/program.conf`, `examples/program.toml`, and
`examples/program.yaml`. Select one with `DIFTRAYWM_CONFIG=/path/to/file`.

## Variables, functions, macros, and bindings

In the DSL:

```text
program {
  let modifier = "Meta";
  let physical = keycode(33);
  let symbolic = keysym("F8");
  let font_base = 7;
  let portrait = true;

  fn twice(n) = n * 2;
  fn workspace(n) = "workspace " + str(n);
  macro notebook(name) = commands("ncursor new", "notelet open " + name);
  macro choose(condition, yes, no) = condition ? yes : no;

  bind(modifier, physical, workspace(3));
  bind(modifier, symbolic, notebook("scratchpad"), "global");
  eval choose(true, twice(font_base), 1 / 0);
}
general {
  font_size = $(twice(font_base))
}
monitor {
  name = DP-1
  rotation = $(portrait ? 90 : 0)
}
```

- `let name = expression;` assigns an immutable variable. Values retain their
  types: numbers, booleans, strings, keysyms, keycodes, or command lists.
  Duplicate names and reassignments are rejected.
- `fn name(parameters) = expression;` defines a function. Arguments evaluate
  once, before the body. Functions return the expression's value.
- `macro name(parameters) = expression;` defines an expression macro. Each
  parameter evaluates in the caller's environment only when used. Unused
  arguments are never evaluated; repeated uses evaluate again. This supports
  conditional composition without textual substitution or quote manipulation.
- `eval expression;` evaluates an expression during config loading. It can
  call functions or macros; it does not execute the commands they return.
- `bind(modifiers, typed_key, action[, scope]);` installs a key binding.
  The action is a command string or `commands(...)` list, computed at config
  load. The optional scope is `"global"` (default) or `"cell"`.
  Pressing the key dispatches those commands through the public Command Bar.

Variables initialize in source order. Functions and macros can be called before
their definitions, but may only read variables already initialized at the time
of a call. Callable bodies use global variables and their own parameters;
macro argument expressions retain the caller's parameters. Parameter names may
shadow globals. Built-in names are reserved. Functions and macros may call each
other or recurse, within the evaluation limits below.

## Keysyms versus keycodes

`keysym("Return")`, `keysym("F8")`, and `keysym("a")` match the keysym produced
by the active keyboard layout. Names use the vendored xkbcommon names and are
case-sensitive. A plain string is not implicitly a keysym.

`keycode(28)` matches raw Linux evdev keycode 28, independently of the produced
symbol. **Do not add XKB's offset of 8.** Accepted codes are integers 1–767.
A plain number is not implicitly a keycode. Typed keys remain distinct through
variables, arguments, return values, and equality comparisons.

Modifiers are a `+`-separated string: `Meta` (alias `Super`), `Ctrl`, `Alt`,
`Shift`, `Mod3`, and `Mod5`. Use `""` or `"None"` for no modifiers.
Modifiers must match exactly, except Caps Lock and Num Lock are ignored.
A shifted symbolic binding must specify the shifted symbol and `Shift`.

All key remapping and binding lives in INI files, not here: see [Key remapping
and keymap INI files](#key-remapping-and-keymap-ini-files). Caps Lock is an
ordinary modifier and is not treated as Meta. The `bind()` above is the one
exception, kept because a binding written down explicitly should be able to
override a keymap profile; see that section for the precedence order.

When both kinds match, the physical keycode binding wins. Duplicate bindings of
the same type and modifiers are errors. Configured bindings override normal
built-in shortcuts, but confirmation prompts and text entry in the Command Bar
keep their modal handling; unmodified help-pager input also remains modal.
`Meta+Escape` remains reserved for quitting. Actions execute on presses, not
releases; consumed key releases are withheld from graphical clients.

## Expressions

Numbers are finite double-precision values. Strings use double quotes, with
`\n`, `\r`, `\t`, `\"`, and `\\` escapes. Comments start with `#` outside
strings. Statements end with semicolons.

Arithmetic: `+ - * / %`, unary `+ -`; comparisons: `== != < <= > >=`;
booleans: `! && ||`; selection: `condition ? yes : no`.
Use parentheses to group expressions. Boolean operators and conditionals
short-circuit. No implicit string/number/boolean conversion occurs; `+`
concatenates two strings or adds two numbers. Equality respects value types.

Built-ins:

| Expression | Result |
| --- | --- |
| `keysym("Return")` | Typed keysym; rejects unknown names |
| `keycode(28)` | Typed physical keycode |
| `str(value)` | String from a string, number, or boolean |
| `commands("spawn below", "notelet open hello")` | Ordered command list; nested command lists flatten |
| `Composite("Meta", "Shift")` | Composite `+`-joined key/modifier set string |

`Composite(...)` takes 1–64 nonempty name strings (no `+` inside) and joins
them with `+`. It builds composite modifier sets for `bind()`, such as
`bind(Composite("Meta", "Shift"), keysym("F9"), ...)`, and composite chords
such as `Composite("Ctrl", "Q")`. An explicit binding for the Meta-prefix
chord itself always wins over the prefix behavior below.

## Meta prefix

`Meta` bindings are reachable through the Logo key and through a
tmux-style prefix chord, `Ctrl+Q` by default: press and release the chord,
then press the next key, which behaves as if Meta were held. `Escape`
cancels an armed prefix. `Meta+Escape` remains reserved for quitting.

The chord itself is set by the keymap INI, not here — see
[Key remapping and keymap INI files](#key-remapping-and-keymap-ini-files).
Two fallbacks apply when no INI is loaded, so removing the file cannot lock
you out of your own session: a `prefix` variable in the configuration
program, set to a `Composite()` of modifiers plus one key name such as
`let prefix = Composite("Ctrl", "X");`, and otherwise `Ctrl+Q`. A `prefix`
variable that is not a string, or not shaped as modifiers plus one known key
name, is ignored.

An explicit `bind()` for the prefix chord always wins: the binding runs
instead of arming the prefix.

Use `$(expression)` as an entire setting value to substitute a string, number,
or boolean. This works in general, terminal, and monitor settings before their
normal validation, including monitor rotation and placement. Typed keys and
command lists cannot be used as scalar settings. There is no interpolation
inside arbitrary strings; concatenate in the expression instead.

## Key remapping and keymap INI files

Everything about keys lives in an INI file, not in this one. The configuration
names the file and nothing more:

```ini
general {
  keymap = keymap.ini
}
```

The path resolves relative to the configuration file, so a configuration
directory stays self-contained. Leave it out, or point it at a file that does
not exist, and the compositor keeps its built-in keys rather than refusing to
start — a typo in a keymap must not lock you out of your own session.

The format is the one `keyd` uses:

```ini
[section]
chord = Action(argument)
```

A chord is a key plus exactly the modifiers written: `<C-q>`, `<C-S-q>`,
`<M-q>` (Meta, the Logo/Super key), `<G-q>` (Logo), `<f8>`, `<space>`,
`<leftbrace>`, or a bare `q` / `;` / `5`. An unknown chord, action or section
is a hard error with a line number, not a silently ignored line, so a typo
shows up as a message rather than a key that quietly does nothing.

| Action | Effect |
| --- | --- |
| `Diftray(command)` | Dispatch a Command Bar command; everything typed after `:` works |
| `Exec(command)` | Run a program, detached, with `WAYLAND_DISPLAY` set |
| `Typeout(text)` | Type text into the focused cell's terminal |
| `Trigger(profile)` | Switch profile; the key is swallowed |
| `Remap(<chord>)` | Send a different chord downstream instead of this one |
| `Ignore()` | Swallow the key entirely |

### Sections

| Section | Meaning |
| --- | --- |
| `[devices]` | USB `VENDOR:PRODUCT` pairs for the system-wide remapper. Omit for every keyboard |
| `[init]` | The remapping layer: `prefix`, `action = Trigger(...)`, `default_profile`, `system_wide` |
| `[meta]` | `prefix`, the compositor's Meta prefix chord |
| `[diftray]` | The help pager keys, moved here from this file |
| anything else | A profile: a set of chord to action bindings |

Two prefixes, and they are different things. `[init] prefix` is the keyd-style
remapping layer: pressing it runs `[init] action`, normally `Trigger` into
another profile, and the keypress is swallowed. It is a device-level idea, so
`diftrayremap` honours it too. `[meta] prefix` is the compositor's Meta prefix:
press it, then the next key behaves as if Meta were held. They are separate
settings because giving them one chord would be ambiguous — whichever ran first
would swallow the key and the other would never see it.

### Precedence

A key press is offered to three things, in this order:

1. **`bind()` in a configuration program.** A binding the user wrote down wins.
2. **The `[meta]` prefix chord.** Arms Meta, so a profile cannot consume the one
   chord that makes Meta reachable at all.
3. **The active keymap profile.** A profile can therefore redefine any built-in
   binding, but not override a deliberate `bind()` or the prefix.

The help pager's keys (`help_key_close`, `help_key_next`, and the rest) used to
be settings here. They are now in the INI's `[diftray]` section, each taking a
single character or one of `space`, `pagedown`, `pageup`, `up`, `down`. The
values in the shipped `keymap.ini` are the defaults, so that whole section can
be deleted without changing behaviour.

### Commands

| Command | Effect |
| --- | --- |
| `keymap show` | The loaded keymap, its profiles, the active one, every binding |
| `keymap reload` | Re-read the file; a parse failure keeps the previous keymap |
| `keymap profile <name>` | Switch profile; an unknown name is refused |
| `keymap reset` | Return to the default profile |
| `keymap path` | The loaded keymap's path |
| `keymap check <path>` | Parse a candidate file and report what it contains, without adopting it |
| `keymap chord <spec>` | Validate a chord spelling and report its key code |

`diftrayctl keymap-show`, `keymap-reload`, `keymap-profile`, `keymap-reset`,
`keymap-path`, `keymap-check` and `keymap-chord` are the remote forms; every one
of them is the same Command Bar command, so `diftrayctl keymap-show` and typing
`keymap show` after `:` do the same thing.

### System-wide remapping

The compositor applies the INI to the keys it already reads, which needs no
privileges and affects only the DiftrayWM session. `diftrayremap` applies the
same file system-wide: it takes the physical keyboard with `EVIOCGRAB` and
republishes it through `uinput`, so the remapping reaches every consumer on the
machine — the compositor, a TTY, another session, anything reading evdev.

It is a separate program rather than a compositor mode because that reach is
expensive to give away. A compositor that grabbed the keyboard would take it
from the session it is nested inside, and it would require every user to run
their window manager as root. Run `diftrayremap check` first: it prints the
exact reason when it cannot start rather than failing halfway. See
`diftrayremap(1)` and `diftraywm-keymap(5)`.

## Settings reference

`general` accepts:

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `border_size` | int >= 1 | 3 | Cell border thickness in pixels |
| `command_bar_height` | int >= 1 | 40 | Bottom command bar height |
| `status_bar_height` | int >= 1 | 24 | Top status bar height |
| `launcher_bar_height` | int >= 1 | 32 | Launcher taskbar height |
| `border_color` | `#RRGGBB[AA]` | `#59a6ff` | Cell border |
| `background_color` | `#RRGGBB[AA]` | `#090c11` | Desktop background |
| `command_bar_color` | `#RRGGBB[AA]` | `#141f2efa` | Bottom command bar |
| `launcher_bar_color` | `#RRGGBB[AA]` | `#0f172af5` | Launcher taskbar |
| `launcher_locked` | bool | false | Start with the taskbar locked |
| `keymap` | path | keymap.ini | Key remapping and bindings INI file |
| `launcher_locked` | bool | false | Start with the taskbar locked |
| `ncursor_mode` | `stack` or `tab` | stack | Layout for extra NCursors |
| `gcursor_mode` | `stack` or `tab` | tab | Layout for extra GCursors |
| `font` | non-empty string | monospace | Font family for NTerm |
| `font_size` | int >= 8 | 14 | Font pixel size |
| `font_ligatures` | bool | true | Enable OpenType ligatures and contextual alternates (`liga`, `clig`, `calt`) |
| `shell` | non-empty string | libshell | New-cell shell |
| `word_pool` | path | /usr/share/dict/words | GCursor name pool |
| `theme` | path | (none) | CSS theme file |
| `help_path` | colon-separated paths | (auto) | Manual page search path |
| `help_key_*` | key name or char | see `diftray.conf` | Help pager keys |

Booleans accept `true`/`false` in the DSL and YAML, and are written unquoted in
TOML. The launcher taskbar is an overlay, so locking it never resizes a cell's
terminal.

### Programming ligatures and Nerd Fonts

NTerm enables font-provided ligatures by default, using HarfBuzz to shape
adjacent text with compatible styling. Select an installed ligature-capable
font with `font`, for example `JetBrainsMono Nerd Font`. Nerd Font icon patches
and programming ligatures are distinct: the underlying font must provide the
ligature substitutions; adding icons alone does not add ligatures.

Set `font_ligatures = false` in `general { ... }` to disable optional ligatures
and contextual alternates, then run `config reload` in the Command Bar.
Unicode shaping, combining marks, and Nerd Font icon fallback remain available
with ligatures disabled.

## TOML and YAML

TOML puts the program at the root, before any table headers:

```toml
program = '''
let size = 7;
fn twice(n) = n * 2;
bind("Meta", keysym("F8"), "notelet open scratchpad");
'''
[general]
font_size = '$(twice(size))'
```

YAML uses a root block scalar:

```yaml
program: |
  let size = 7;
  fn twice(n) = n * 2;
  bind("Meta", keycode(33), "notelet open scratchpad");
general:
  font_size: '$(twice(size))'
```

The surrounding file's quoting rules still apply. In DSL settings write
`font_size = $(twice(size))` without outer quotes.

## Evaluate and run from the Command Bar

- `config vars` lists variable values, preserving key type labels.
- `config eval twice(font_base)` displays the result.
- `config eval notebook("hello")` displays the command list without executing it.
- `config run notebook("hello")` executes the returned commands in the current
  Command Bar's scope. Explicit Cell IDs retain their normal scoping rules.

Configuration expressions are pure: no file, network, process, or compositor
access. Side effects occur only when a binding or `config run` dispatches a
command. Commands retain their existing validation and confirmation behavior.
A sequence is not a transaction: earlier commands remain applied if a later
one fails. Dispatch stops on unknown or scope-rejected commands; built-in
handlers may report an error status without signalling dispatch failure.

Invalid programs, bindings, and setting expressions reject the whole config
load without changing the previous configuration. Programs are limited to
256 KiB, 32,768 tokens, 1,024 statements, 32 parameters per callable, 256 bindings,
and 64 levels of expression syntax nesting. Evaluation is limited to 10,000
expression steps and 64 nested evaluations per program compilation or explicit
evaluation. Strings are at most 64 KiB. Actions contain 1–64 commands, each at
most 4,096 bytes and at most 64 KiB combined. Nested Command Bar dispatch is
limited to 16 levels, including command strings that call `config run` again.
