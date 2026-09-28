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

Use `$(expression)` as an entire setting value to substitute a string, number,
or boolean. This works in general, terminal, and monitor settings before their
normal validation, including monitor rotation and placement. Typed keys and
command lists cannot be used as scalar settings. There is no interpolation
inside arbitrary strings; concatenate in the expression instead.

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
