# Libsh: Header-Only POSIX Shell API

Libsh is a header-only C++20 library that provides a POSIX-oriented shell runtime, a typed C++ API, and a native DSL for building shell-like workflows inside C++ programs.

The library is designed for two complementary use cases:

- **Programmatic shell construction** through a direct C++ API.
- **Declarative shell composition** through a DSL that maps naturally to shell pipelines, redirections, and control flow.

The primary namespace is `libsh`. For convenience, examples use the alias `lsh`.


---

## Goals

Libsh aims to provide:

- A **POSIX-aligned execution model** for commands, pipelines, redirections, and shell state.
- A **header-only integration model** suitable for embedding in larger C++ applications.
- A **safe and inspectable API** for constructing shell behavior without relying on stringly-typed command composition.
- A **native C++ DSL** for writing readable pipeline-oriented code.
- A **runtime extension model** through kernels, adapters, and embedded scripting.

Libsh is not just a command launcher. It is a shell runtime that can execute built-ins, external commands, scripted kernels, interactive sessions, and structured output transformations.

---

## Core Design

At a high level, a Libsh shell consists of:

- an **environment**,
- an **executor**,
- one or more **sinks**,
- an **expander**,
- and a **shell instance** that coordinates these components.

A robust design should keep these responsibilities separate:

- **Environment** manages variables, exported state, and shell-visible context.
- **Executor** launches external processes, built-ins, and kernels.
- **Sink** receives output streams and can forward, buffer, filter, or transform them.
- **Expander** resolves variables, arithmetic expressions, command substitutions, and related shell expansions.
- **Shell** owns execution state, orchestration, and user-facing APIs.

This separation makes Libsh easier to embed, test, sandbox, and extend.

---

## Basic Example: C++ API

A minimal API-driven workflow might look like this:

```cpp
#include "Libsh.hpp"

namespace lsh = libsh;

int main() {
    auto env = lsh::derive_system_env();
    auto exec = lsh::make_virtual_executor();
    auto sink = lsh::make_buffered_sink(stdout);
    auto expander = lsh::make_global_expander();

    auto shell = lsh::make_sandboxed_shell(
        "MyShell",
        env,
        exec,
        sink,
        expander
    );

    shell.set_assign("PROJECT", lsh::expandvar<>("Libsh"));
    shell.set_exec({ "printf", "Project: %s\n", lsh::quote("$PROJECT") });
    shell.execute();
}
```

A more realistic example uses assignments, functions, redirections, and conditionals:

```cpp
#include "Libsh.hpp"

namespace lsh = libsh;

int main() {
    auto env = lsh::derive_system_env();
    auto exec = lsh::make_virtual_executor();
    auto sink = lsh::make_buffered_sink(stdout);
    auto expander = lsh::make_global_expander();

    auto shell = lsh::make_sandboxed_shell(
        "BuildShell",
        env,
        exec,
        sink,
        expander
    );

    auto queue =
        shell.queue()
            .set_function("show_sum", [](lsh::Subshell& ctx) {
                if (ctx.args.size() < 2) {
                    ctx.execute_command({ "echo", "missing argument" });
                    ctx.exit_subshell(lsh::EXIT_FAILURE);
                    return;
                }

                const auto& arg = ctx.args[1];
                if (arg.is_expandable() && arg.is_arith()) {
                    auto result = arg.expand_arith();
                    ctx.execute_command({ "echo", lsh::quote(result) });
                    ctx.exit_subshell(lsh::EXIT_SUCCESS);
                    return;
                }

                ctx.execute_command({ "echo", "argument is not arithmetic" });
                ctx.exit_subshell(lsh::EXIT_FAILURE);
            })
            .set_assign("A", lsh::arith("1 + 2 * 3"))
            .set_exec({ "show_sum", lsh::quote("A") })
            .set_redirect<lsh::Redirect::Out>(lsh::file("result.txt"))
            .set_connective<lsh::Connective::Conjunct>()
            .set_exec({ "ls", lsh::flag("-a") })
            .enqueue();

    shell.echo("Executing queue");
    queue.execute();

    return 0;
}
```

### Design Notes

For long-term robustness, the API should favor:

- typed builders over loosely ordered mutators,
- explicit lifetime and ownership of sinks and executors,
- clear distinction between shell state and transient subshell state,
- validation before execution, especially for redirections and malformed argument trees.

For example, builder objects should ideally reject invalid states such as:

- a redirection with no target,
- a connective with no preceding command,
- an expansion node that mixes incompatible modes.

---

## Pipeline Construction API

Libsh also supports explicit pipeline construction through builders. This is useful when pipelines are assembled dynamically.

```cpp
#include "Libsh.hpp"

namespace lsh = libsh;

int main() {

auto env = lsh::derive_system_env();
    auto exec = lsh::make_virtual_executor();
    auto sink = lsh::make_buffered_sink(stdout);
    auto expander = lsh::make_global_expander();

    auto shell = lsh::make_sandboxed_shell(
        "ProcShell",
        env,
        exec,
        sink,
        expander
    );

    auto jobman = lsh::load_job_manager("simple.cnf");
    auto pipeline = lsh::create_pipeline_builder(jobman, false);

    auto ps_cmd = lsh::create_command_builder();
    auto grep_cmd = lsh::create_command_builder();

    ps_cmd.add_exec({ "ps", lsh::multiflag("-aux") });
    grep_cmd.add_exec({ "grep", "firefox" });

    pipeline.add_command(lsh::command(ps_cmd));
    pipeline.add_command(lsh::command(grep_cmd));
    pipeline.add_sink(lsh::sink(stdout));

    pipeline.execute();

    if (pipeline.exit_status == lsh::EXIT_SUCCESS)
        shell.echo("Firefox found");
    else
        shell.echo("Firefox not found");

    return 0;
}
```

A second example shows a three-stage pipeline:

```cpp
auto pipeline = lsh::create_pipeline_builder(jobman, false);

auto ls_cmd = lsh::create_command_builder();
auto grep_cmd = lsh::create_command_builder();
auto wc_cmd = lsh::create_command_builder();

ls_cmd.add_exec({ "ls", "-1" });
grep_cmd.add_exec({ "grep", "\\.cpp$" });
wc_cmd.add_exec({ "wc", "-l" });

pipeline.add_command(lsh::command(ls_cmd));
pipeline.add_command(lsh::command(grep_cmd));
pipeline.add_command(lsh::command(wc_cmd));
pipeline.add_sink(lsh::sink(stdout));

pipeline.execute();
```

### Recommended Improvements

To make this part of Libsh more robust, the design should include:

- a **validated pipeline graph** before launch,
- explicit **stderr routing** control,
- stronger **exit-status aggregation rules**,
- first-class support for **pipefail-style semantics**,
- support for **timeouts**, **resource limits**, and **cancellation**.

---

## Basic Example: Native DSL

Libsh offers a native C++ DSL for expressing shell-like flows more naturally.

```cpp
#include "Libsh.hpp"

namespace lsh = libsh;
using namespace lsh::dsl;

int main() {
    auto env = lsh::derive_system_env();
    auto exec = lsh::make_virtual_executor();
    auto sink = lsh::make_buffered_sink(stdout);
    auto expander = lsh::make_global_expander();

    auto shell = lsh::make_sandboxed_shell(
        "MyShell",
        env,
        exec,
        sink,
        expander
    );

    auto pipeline =
        cmd("ps", "-aux")
        | cmd("grep", "firefox")
        > file("processes.txt");

    auto status = shell.run(pipeline);

    if (status == lsh::EXIT_SUCCESS)
        shell.echo("Firefox found");
    else
        shell.echo("Firefox not found");

    return 0;
}
```

Another example combines pipelines and boolean composition:

```cpp
using namespace lsh::dsl;

auto task =
    (cmd("test", "-f", "CMakeLists.txt")
     && cmd("echo", "Project root detected"))
    || cmd("echo", "Not a CMake project");

shell.run(task);
```

A third example demonstrates redirection and quoting:

```cpp
using namespace lsh::dsl;

auto report =
    cmd("printf", "alpha\nbeta\ngamma\n")
    | cmd("grep", "a")
    > file("matches.txt");

shell.run(report);
```

### DSL Design Recommendations

The DSL should remain readable, but it must also be precise. A stronger design would provide:

- explicit grouping rules,
- predictable precedence between `|`, `&&`, `||`, and redirections,
- compile-time diagnostics where possible,
- a typed internal representation rather than immediate execution side effects.

The DSL should compile into an intermediate command graph that can be validated, optimized, inspected, and serialized before execution.

Libsh uses MetaTk’s DSL support to implement this layer. If the project already depends on `DSLtk.hpp`, that document should describe the boundary clearly: MetaTk provides the DSL machinery, while Libsh defines the shell semantics.

---

## Sinklets

Sinklets are programmable sinks. They receive stream output, inspect it, transform it, filter it, and optionally forward it downstream.

This makes them useful for:

- line filtering,
- log analysis,
- output rewriting,
- metrics extraction,
- structured decoding of JSON, YAML, or similar formats.

Libsh exposes two sinklet APIs:

- a **class-based API** for reusable components,
- a **lambda-based API** for compact one-off logic.

---

## Sinklets: Class-Based API

A simplified version of the core interface looks like this:

```cpp
namespace libsh {
    struct SinkletContext {
        Shell& shell;
        std::string_view source;
    };

    class Sinklet {
    public:
        virtual ~Sinklet() = default;

        virtual void begin(SinkletContext&) {}
        virtual void write(std::string_view chunk, SinkletContext&) = 0;
        virtual void end(SinkletContext&) {}
    };
}
```

A sinklet should behave like a stream processor: it may maintain state between `begin`, `write`, and `end`, but it should avoid hidden global state and should clearly define buffering semantics.

### Example: Line-Oriented TODO Filter

```cpp
class TodoFilter : public lsh::batteries::LineSinklet {
public:
    explicit TodoFilter(std::shared_ptr<lsh::Writer> out)
        : out_(std::move(out)) {}

    void on_line(std::string_view line, lsh::SinkletContext&) override {
        if (line.find("TODO") != std::string_view::npos) {
            ++matches_;
            out_->write(std::string(line) + "\n");
        }
    }

    std::size_t matches() const noexcept {
        return matches_;
    }

private:
    std::shared_ptr<lsh::Writer> out_;
    std::size_t matches_ = 0;
};
```

Usage:

```cpp
auto writer = lsh::make_writer(stdout);
auto filter = std::make_shared<TodoFilter>(writer);

shell.run(
    lsh::dsl::cmd("grep", "-R", "TODO", "src")
    > lsh::sink(filter)
);

shell.echo("TODO matches: " + std::to_string(filter->matches()));
```

### Example: JSON Metrics Sinklet

```cpp
class CountingSinklet : public lsh::batteries::LineSinklet {
public:
    void on_line(std::string_view line, lsh::SinkletContext&) override {
        ++line_count_;
        byte_count_ += line.size();
    }

    std::size_t line_count() const noexcept { return line_count_; }
    std::size_t byte_count() const noexcept { return byte_count_; }

private:
    std::size_t line_count_ = 0;
    std::size_t byte_count_ = 0;
};
```

Usage:

```cpp
auto counter = std::make_shared<CountingSinklet>();

shell.run(
    lsh::dsl::cmd("cat", "README.md")
    > lsh::sink(counter)
);

shell.echo("lines=" + std::to_string(counter->line_count()));
shell.echo("bytes=" + std::to_string(counter->byte_count()));
```

### Batteries Included

Libsh ships with several sinklet building blocks.

**Primitive sinklets**
- `lsh::batteries::LineSinklet`
- `lsh::batteries::BlockSinklet`
- `lsh::batteries::UnbufSinklet`

**Higher-level sinklets**
- `lsh::batteries::GrepSinklet`
- `lsh::batteries::ForwardSinklet`
- `lsh::batteries::TeeSinklet`
- `lsh::batteries::JSONSinklet`
- `lsh::batteries::YAMLSinklet`

If the project provides standard buffering adapters, document them under the Libsh namespace rather than `std::batteries`, which would be confusing and likely incorrect in C++ terminology.

### Example: Forwarding Sinklet

A simplified forwarding sinklet might look like this:

```cpp
class ForwardingSinklet : public lsh::Sinklet {
public:
    explicit ForwardingSinklet(std::shared_ptr<lsh::Writer> next)
        : next_(std::move(next)) {}

    void write(std::string_view data, lsh::SinkletContext&) override {
        if (next_)
            next_->write(data);
    }

private:
    std::shared_ptr<lsh::Writer> next_;
};
```

### Recommended Improvements

To make sinklets more robust, Libsh should define:

- ownership and lifetime rules for downstream writers,
- error propagation behavior from sinklets back into the shell,
- whether sinklets are reusable across commands,
- thread-safety expectations,
- a standard contract for partial writes, flushes, and finalization.

---

## Sinklets: Lambda-Based API

For smaller transformations, Libsh also supports lambda-based sinklets.

### Example: Line Filter

```cpp
auto sinklet = lsh::make_line_sinklet(
    shell,
    [](std::string_view line, lsh::SinkletContext&, lsh::Writer& out) {
        if (line.contains("TODO"))
            out.write("Incomplete code detected\n");
    }
);
```

### Example: Prefix Logger

```cpp
auto prefixed = lsh::make_line_sinklet(
    shell,
    [](std::string_view line, lsh::SinkletContext&, lsh::Writer& out) {
        out.write("[build] ");
        out.write(line);
        out.write("\n");
    }
);
```

### Example: Block Transformer

```cpp
auto sinklet = lsh::make_block_sinklet(
    shell,
    [](std::string_view block, lsh::SinkletContext&, lsh::Writer& out) {
        if (!block.empty()) {
            out.write("BEGIN BLOCK\n");
            out.write(block);
            out.write("\nEND BLOCK\n");
        }
    }
);
```

Available constructors:

- `lsh::make_line_sinklet`
- `lsh::make_block_sinklet`
- `lsh::make_unbuf_sinklet`

### Recommended Improvements

The lambda-based API becomes much more useful if it guarantees:

- consistent buffering behavior with class-based sinklets,
- access to command metadata,
- optional state objects for accumulators,
- a standard error-return mechanism.

---

## Kernels

Kernels are command modules implemented in Lua-compatible scripting. In Libsh, they act like built-in commands that participate in the shell runtime.

The kernel system should be documented separately in `include/Libsh-Kernel.hpp`, since it is a distinct subsystem with its own lifecycle, packaging model, and authoring API.

Libsh uses QaMRpp rather than the canonical Lua runtime. Kernels are packaged through Libsh’s podlet-based tooling and compiled into `.lakk` archives.

### Kernel Design

A kernel should expose:

- metadata,
- argument parsing,
- execution hooks,
- access to shell streams,
- optional state storage.

A clear lifecycle makes kernels easier to reason about:

1. **Load**
2. **Initialize**
3. **Parse arguments**
4. **Read input**
5. **Process**
6. **Write output**
7. **Shutdown**

### Example: Columnizing Kernel

```lua
-- MakeColumn.lua
local lak = require("Libsh")
local col = require("Columizer")

local K = lak.kernel.create("Columnize", {
    command = "columnize",
    aliases = { "mkcolumn", "columnz" },
    authors = { "Chubak Bidpaa <chubakbidpaa@gmail.com>" },
    version = "0.1.0",
    manual = lak.document.markdown:LoadFromFile("GUIDE.md"),
})

local shell = lak.shell:Get()
local stdin = shell.stdin:Get()
local stdout = shell.stdout:Get()

local function columnize_data(lines, width)
    return col.columnize(lines, width)
end

K.Startup = function(args)
    K.data = K.data or {}
    K.data.width = 16

    local optparse = lak.utils.optparse:New()
    optparse:add_option("--width/-w", "Column width", function(value)
        K.data.width = value.integral
    end)

    optparse:Parse(args)
end

K.Filter = function()
    local input = stdin:Read()
    K.data.columnized_data = columnize_data(input, K.data.width)
end

K.Shutdown = function()
    stdout:Write(K.data.columnized_data)
end

return K
```

### Packaging Example

```sh
libsh-cli lakkpak \
    --podlet Columnizer \
    --output make-column.lakk \
    MakeColumn.lua
```

### Loading a Kernel from C++

```cpp
auto env = lsh::derive_system_env();
auto exec = lsh::make_virtual_executor();
auto sink = lsh::make_buffered_sink(stdout);
auto expander = lsh::make_global_expander();

auto shell = lsh::make_sandboxed_shell("MyShell", env, exec, sink, expander);

shell.load_kernel("./make-column.lakk");

shell.run(
    lsh::dsl::cmd("printf", "alpha\nbeta\ngamma\ndelta\n")
    | lsh::dsl::cmd("columnize", "--width", "12")
);
```

### Additional Example: Kernel in a Pipeline

```cpp
shell.run(
    lsh::dsl::cmd("cat", "names.txt")
    | lsh::dsl::cmd("columnize", "--width", "20")
    > lsh::dsl::file("columns.txt")
);
```

### Standard Kernels

If Libsh implements POSIX commands such as `ls`, `cd`, and similar utilities as kernels, that is a significant architectural decision and should be stated clearly:

- which commands are always built in,
- which commands are shipped as standard kernels,
- how precedence works between kernels and system binaries,
- how versioning and compatibility are managed.

If the standard kernels live in `stdkern`, the document should explain how they are compiled, installed, and discovered.

### Installation and Discovery

For example:

```sh
libsh-cli lakk --install foo-bar.lakk
```

Installed kernels should be placed under `$LAKPOSHT_HOME/kernels`.

If `$LAKPOSHT_HOME` is unset, a documented default such as `~/.libsh` or `~/.local/lib/libsh` should be used consistently across the project.

A kernel loaded by name should resolve through the configured kernel search path; a kernel loaded by path should bypass lookup and load directly.

### Recommended Improvements

To make kernels more robust, Libsh should define:

- ABI or API compatibility guarantees for kernel packages,
- search path precedence rules,
- signature or trust policy for installed archives,
- version constraints between Libsh and packaged kernels,
- failure modes for malformed, incompatible, or untrusted kernels.

---

## Inline Lua Scripts

Libsh evaluates inline scripts through its QaMRpp adapter; QaMRpp types do not leak through the public API.

### Example: Arithmetic Preprocessing

```cpp
shell.env().set("BASE", "40");
auto expression = lsh::dsl::cmd("echo", lsh::lua("return BASE + 2"));
shell.run(expression.program());
```

### Example: String Normalization

```cpp
shell.env().set("TITLE", "Libsh Shell");
auto expression = lsh::dsl::cmd("echo", lsh::lua("return TITLE .. ' runtime'"));
shell.run(expression.program());
```

### Runtime Podlets

Inline expansions expose only:

- identifier-safe shell environment names as globals;
- `env("NAME")` for any environment key.

QaMRpp standard libraries are not loaded for inline expansion; filesystem and process capabilities are not implicitly granted.

### Recommended Improvements

Inline scripting is valuable, but it increases complexity and attack surface. The design should include:

- sandbox controls,
- script execution timeouts,
- memory limits,
- import restrictions,
- deterministic behavior rules where needed.

---

## XI Adapters

XI adapters describe how Libsh interoperates with external utilities. They provide structured metadata, discovery, argument normalization, capability probing, and execution policy.

This is a strong design direction because it turns command interoperability into data rather than ad hoc code.

### Example: Full Adapter

```xi
adapter "man" {
    metadata {
        summary = "System manual page reader"
        category = "docs"
        pipeline = false
        interactive = true
    }

    discover {
        binary "man"

        search {
            path_env "PATH"
            common_prefixes [
                "/usr/bin",
                "/bin",
                "/usr/local/bin",
                "/opt/homebrew/bin",
                "/snap/bin"
            ]
            pkg_config "man"
            which "man"
        }

        validate {
            exec "--version"
            exec "-w man"
        }
    }

    arguments {
        positional "topic" {
            index = 1
            required = false
        }

        positional "section" {
            index = 0
            required = false
            transform = string
        }

        flag "--all" alias "-a" => var "all"
        flag "--where" alias "-w" => var "where"
        flag "--apropos" alias "-k" => var "apropos"
        option "--locale" alias "-L" takes string => var "locale"
    }

    normalize {
        if set(section) { push arg(section) }
        if set(all)     { push flag("-a") }
        if set(where)   { push flag("-w") }
        if set(apropos) { push flag("-k") }
        if set(locale)  { push flag("-L"); push arg(locale) }
        if set(topic)   { push arg(topic) }
    }

    execution {
        stdout = inherit
        stderr = inherit
        stdin = null
        timeout = none
        retry = 0
    }
}
```

### Compact Adapter Example

```xi
use resolver "posix_path_search"
use resolver "version_from_stdout"
use resolver "gnu_vs_bsd_preference"

adapter "man" {
    command = "man"

    discover = posix_path_search("man")
    resolve  = gnu_vs_bsd_preference()
    version  = version_from_stdout("--version")

    arguments {
        flag "-k" => var "apropos"
        flag "-w" => var "where"
        positional "topic"
    }

    normalize {
        if set(apropos) { push flag("-k") }
        if set(where)   { push flag("-w") }
        if set(topic)   { push arg(topic) }
    }
}
```

### C++ Usage Example

```cpp
auto env = lsh::derive_system_env();
auto exec = lsh::make_virtual_executor();
auto sink = lsh::make_buffered_sink(stdout);
auto expander = lsh::make_global_expander();

auto shell = lsh::make_sandboxed_shell("MyShell", env, exec, sink, expander);

auto manxi = shell.load_xi_adapter("man");
manxi.add_arg(lsh::arg::option("s", "3"));
manxi.add_arg(lsh::arg::pos("printf"));
manxi.run();
```

### Additional Example: `grep` Adapter

```cpp
auto grepxi = shell.load_xi_adapter("grep");
grepxi.add_arg(lsh::arg::flag("n"));
grepxi.add_arg(lsh::arg::pos("TODO"));
grepxi.add_arg(lsh::arg::pos("README.md"));
grepxi.run();
```

### Shipped Adapters

A built-in adapter catalog might include:

- `git`
- `ssh`
- `scp`
- `rsync`
- `curl`
- `wget`
- `tar`
- `gzip` and `gunzip`
- `zip` and `unzip`
- `grep`
- `sed`
- `awk`
- `find`
- `xargs`
- `jq`
- `make`
- `cmake`
- `pkg-config`
- `systemctl`
- `docker`
- `whatis`
- `groff`

### Recommended Improvements

XI is one of the strongest ideas in this design. To make it production-ready, the document should clarify:

- schema versioning for `.xi` files,
- validation and diagnostics,
- adapter discovery precedence,
- platform-specific overrides,
- capability cache invalidation,
- security policy for adapter execution.

---

## Puppeteer API

Libsh provides a Puppeteer API for automating interactive terminal programs, similar in spirit to `expect` or `pexpect`.

This API is useful for:

- logging into remote shells,
- handling password or OTP prompts,
- driving REPLs,
- testing interactive CLI applications,
- building higher-level interactive kernels.

### Basic Example

```cpp
auto env = lsh::derive_system_env();
auto exec = lsh::make_virtual_executor();
auto sink = lsh::make_buffered_sink(stdout);
auto expander = lsh::make_global_expander();
auto shell = lsh::make_sandboxed_shell("MyShell", env, exec, sink, expander);

auto puppet = shell.make_puppeteer();

puppet.spawn({ "ssh", "user@example.com" });
puppet.expect("password:");
puppet.send_line("hunter2");

puppet.expect("$");
puppet.send_line("uname -a");

auto output = puppet.capture_until("$");
shell.echo(output);

puppet.send_line("exit");
puppet.wait();
```

### REPL Example

```cpp
auto puppet = shell.make_puppeteer();

puppet.spawn({ "python3", "-q" });
puppet.expect(">>> ");
puppet.send_line("print(6 * 7)");
puppet.expect("42");
puppet.expect(">>> ");
puppet.send_line("exit()");
puppet.wait();
```

### Example with Filtered Logging

```cpp
auto logger = lsh::make_line_sinklet(
    shell,
    [](std::string_view line, lsh::SinkletContext&, lsh::Writer& out) {
        if (!line.contains("password"))
            out.write(std::string(line) + "\n");
    }
);

auto puppet = shell.make_puppeteer(logger);
puppet.spawn({ "my-interactive-tool" });
```

### API Surface

The Puppeteer API should include composable primitives such as:

- `spawn`
- `expect`
- `send`
- `send_line`
- `capture_until`
- `wait`
- `interrupt`
- `terminate`
- `kill`

### Recommended Improvements

For robustness, this subsystem should define:

- timeout handling,
- UTF-8 and raw byte semantics,
- regex or matcher ownership rules,
- transcript capture and replay,
- PTY sizing and resize events,
- secure input masking for secrets.

---

## CLI

Libsh also ships a CLI frontend built on top of the main library. This makes the library both embeddable and directly usable as a shell.

The CLI design should be documented as a separate consumer of the library rather than as part of the core shell runtime.

Relevant components include:

- `cli/libsh-cli.cpp` for the command-line frontend,
- `cli/Shell.syntax` for PikoRL syntax highlighting,
- `cli/libsh-parse.cpp` for shell parsing,
- `.agents/docs/posix-shell-std.txt` for POSIX shell references.

If PikoRL provides the REPL and PEGTL provides parsing, the document should state clearly how responsibilities are divided:

- Libsh runtime: execution and state
- CLI parser: textual shell syntax
- PikoRL: editing, history, completion, highlighting
- third-party dependencies: parser/runtime support

### Recommended Improvements

The CLI becomes much easier to maintain if it is described as a thin layer over the C++ runtime. That implies:

- shared execution code between library and CLI,
- a unified AST or IR for parsed commands,
- the same expansion rules in both API and CLI modes,
- pluggable completion and highlighting based on kernel and adapter metadata.

---

## Documentation Layout Recommendation

This document currently tries to explain too many subsystems at once. A more robust documentation structure would split it into focused documents:

- `README.md` — overview and quick start
- `docs/design/architecture.md` — core shell architecture
- `docs/design/dsl.md` — DSL semantics and precedence
- `docs/design/sinklets.md` — sink processing model
- `docs/design/kernels.md` — kernel packaging and lifecycle
- `docs/design/xi.md` — adapter language and resolution model
- `docs/design/puppeteer.md` — interactive automation
- `include/Libsh-Kernel.hpp` — kernel-facing API reference

This keeps the top-level design readable while preserving the depth needed for implementers.

---

## Summary of Design Improvements

The strongest path forward for Libsh is to make each subsystem more explicit and more strongly typed:

- define a validated intermediate representation for commands and pipelines,
- separate shell runtime, DSL, CLI parsing, and extension mechanisms cleanly,
- formalize lifecycle and error contracts for sinklets, kernels, and interactive sessions,
- document trust, sandboxing, and discovery behavior for all loaded artifacts,
- treat `include/Libsh-Kernel.hpp` as the canonical kernel API surface.

Libsh already has several promising ideas: a typed shell API, a native C++ DSL, programmable sinks, scriptable kernels, structured adapters, and interactive automation. The next step is not to add more concepts, but to tighten the semantics, boundaries, and contracts so the system remains understandable as it grows.
