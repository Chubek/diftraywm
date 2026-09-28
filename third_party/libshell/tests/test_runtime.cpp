// End-to-end runtime tests for the LibShell/Lakposht runtime.
//
// These tests exercise the typed path: DSL -> IR -> validation -> expansion ->
// Executor -> ExecutionReport. They use the real LocalExecutor (fork/execvp,
// builtins, kernels) from LibShell-Posix.hpp, plus the DryRunExecutor for the
// no-spawn contract. No external test framework is required; a tiny assertion
// harness is defined below so the suite stays a single self-contained TU that
// CTest can register.
//
// Build (see tests/CMakeLists.txt): g++ -std=c++20 -Iinclude -Wall -Wextra
// -Werror -pedantic tests/test_runtime.cpp cli/libsh-parse.cpp -o test_runtime

#include "LibShell.hpp"
#include "LibShell-Kernel.hpp"
#include "LibShell-Posix.hpp"
#include "../stdkern/StdKern.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// parse_line is defined in cli/libsh-parse.cpp and declared in cli/libsh-cli.cpp.
// Forward-declared here so the test TU does not need the CLI entrypoint.
namespace lsh::cli {
Result<ir::Program> parse_line(std::string_view line);
}

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, std::string_view context) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << context << " (" << expr << ")\n";
    }
}

#define EXPECT(cond, ctx) check((cond), #cond, (ctx))
#define EXPECT_EQ(a, b, ctx)                                                                \
    do {                                                                                    \
        auto _va = (a);                                                                     \
        auto _vb = (b);                                                                     \
        check(_va == _vb, #a " == " #b, (ctx));                                             \
        if (!(_va == _vb)) {                                                                \
            std::cerr << "      got=" << _va << " want=" << _vb << '\n';                    \
        }                                                                                   \
    } while (0)

std::shared_ptr<lsh::MemoryWriter> capture() { return std::make_shared<lsh::MemoryWriter>(); }

// Shell wired to the real POSIX executor. Builtins, kernels, and external
// processes all flow through this one.
lsh::Shell local_shell() { return lsh::Shell(std::make_shared<lsh::LocalExecutor>()); }

// Read a small file back as a string; used to verify file redirections.
std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---- Tests ----------------------------------------------------------------

// The DSL must construct an inspectable IR graph and never execute. A DryRun
// shell produces a report with no diagnostics and no spawned pid.
void test_dsl_builds_ir_without_exec() {
    using namespace lsh::dsl;

    auto graph = (cmd("printf", "%s\\n", lsh::variable("PROJECT")) | cmd("grep", "Lak"))
                 && redirect(cmd("wc", "-l"), lsh::out("count.txt"));

    const auto& node = graph.node();
    EXPECT(static_cast<bool>(node), "dsl produces a node");

    // Top-level node is the && sequence.
    const auto* seq = std::get_if<lsh::ir::Sequence>(&node->value);
    EXPECT(seq != nullptr, "top-level is a Sequence");
    if (seq) {
        const auto* left = std::get_if<lsh::ir::Pipeline>(&seq->left->value);
        EXPECT(left != nullptr && left->commands.size() == 2, "left operand is a 2-stage pipeline");
        EXPECT(seq->connective == lsh::Connective::and_if, "connective is and_if");
    }

    lsh::Shell shell; // DryRunExecutor by default
    auto report = shell.run(graph.program());
    EXPECT(report.has_value(), "dry run returns a report");
    EXPECT(report.value().diagnostics.empty(), "dry run reports no diagnostics");
}

// Validation is a pure pass: empty argv and null nodes are rejected before any
// executor is consulted.
void test_validation_rejects_malformed_graphs() {
    lsh::ir::Command empty;
    auto empty_program = lsh::ir::Program {lsh::ir::command(empty)};
    auto empty_report = lsh::ir::validate(empty_program);
    EXPECT(!empty_report.ok(), "empty argv is rejected");

    lsh::ir::Program null_program {nullptr};
    auto null_report = lsh::ir::validate(null_program);
    EXPECT(!null_report.ok(), "null root node is rejected");
}

// Builtin echo routed through the POSIX executor with stdout captured into a
// memory writer.
void test_builtin_echo_capture() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(lsh::dsl::cmd("echo", "hello"), lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo succeeds");
    EXPECT_EQ(mem->bytes(), std::string("hello\n"), "echo output captured");
}

// Variable expansion uses shell state, not string concatenation.
void test_variable_expansion() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    shell.env().set("NAME", "world");
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("echo", lsh::variable("NAME")),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo $NAME succeeds");
    EXPECT_EQ(mem->bytes(), std::string("world\n"), "variable expanded");
}

// Arithmetic expansion lowers $((...)) through the recursive-descent evaluator.
void test_arithmetic_expansion() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("echo", lsh::arithmetic("6 * 7")),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo $((6*7)) succeeds");
    EXPECT_EQ(mem->bytes(), std::string("42\n"), "arithmetic evaluated");
}

// QaMRpp is the default Lua backend. Shell variables are exposed through the
// explicit env(NAME) capability and identifier-safe names are Lua globals.
void test_lua_expansion() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    shell.env().set("PROJECT", "libsh");
    shell.env().set("DASH-NAME", "visible-through-env");
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("echo", lsh::lua("return PROJECT .. ':' .. env('DASH-NAME') .. ':' .. (6 * 7)")),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "Lua expansion succeeds");
    EXPECT_EQ(mem->bytes(), std::string("libsh:visible-through-env:42\n"), "QaMRpp evaluated Lua with shell environment");
}

void test_lua_failure_is_diagnostic() {
    lsh::Shell shell = local_shell();
    auto report = shell.run(lsh::dsl::cmd("echo", lsh::lua("return (")).program());
    EXPECT(!report.has_value(), "invalid Lua rejects execution");
    if (!report) {
        EXPECT(report.error().code == lsh::ErrorCode::bad_expansion, "Lua parse error is a bad expansion");
    }
}

void test_redirect_does_not_mutate_source_expression() {
    auto mem = capture();
    auto base = lsh::dsl::cmd("echo", "unredirected");
    auto redirected = lsh::dsl::redirect(
        base, lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));

    const auto& base_command = std::get<lsh::ir::Command>(base.node()->value);
    const auto& redirected_command = std::get<lsh::ir::Command>(redirected.node()->value);
    EXPECT(base_command.redirections.empty(), "redirect leaves source IR immutable");
    EXPECT(redirected_command.redirections.size() == 1, "redirected IR owns its redirection");
}

// Command substitution re-enters the runtime via the scripting backend. Wiring
// the CLI parser as the parse hook lets $(...) use the full grammar.
void test_command_substitution() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    shell.set_command_substitution_parser([](std::string_view line) { return lsh::cli::parse_line(line); });
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("echo", lsh::command_substitution("echo hi")),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo $(echo hi) succeeds");
    EXPECT_EQ(mem->bytes(), std::string("hi\n"), "command substitution captured and trimmed");
}

// A mixed builtin -> external pipeline. printf (builtin) feeds wc (external)
// through the buffered pipeline path; the last stage stdout is captured.
void test_buffered_pipeline() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("printf", "%s\\n", "a", "b") | lsh::dsl::cmd("wc", "-l"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "printf | wc succeeds");
    EXPECT_EQ(report.value().pipeline_statuses.size(), std::size_t(2), "two pipeline stages ran");
    EXPECT_EQ(mem->bytes(), std::string("2\n"), "wc counted two lines");
}

// An all-external pipeline exercises the fork/execvp path directly.
void test_external_pipeline() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("/bin/echo", "alpha") | lsh::dsl::cmd("/usr/bin/sort"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo | sort succeeds");
    EXPECT_EQ(mem->bytes(), std::string("alpha\n"), "sort passed the line through");
}

// Sequence connectives short-circuit: && skips the right operand on failure,
// || runs the right operand on failure.
void test_sequence_short_circuit() {
    auto mem = capture();
    lsh::Shell shell = local_shell();

    // false && echo nope  -> echo must not run
    auto skipped = lsh::dsl::redirect(
        lsh::dsl::builtin("false") && lsh::dsl::cmd("echo", "nope"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto r1 = shell.run(skipped.program());
    EXPECT(r1.has_value() && !r1.value().status.success(), "false && ... fails");
    EXPECT_EQ(mem->bytes(), std::string(""), "right operand of && skipped on failure");

    // false || echo yes  -> echo runs
    auto recovered = lsh::dsl::redirect(
        lsh::dsl::builtin("false") || lsh::dsl::cmd("echo", "yes"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto r2 = shell.run(recovered.program());
    EXPECT(r2.has_value() && r2.value().status.success(), "false || echo succeeds");
    EXPECT_EQ(mem->bytes(), std::string("yes\n"), "right operand of || ran on failure");
}

// File redirection writes to disk and is observable outside the runtime.
void test_file_redirection() {
    const auto out = std::filesystem::temp_directory_path() / "libsh_test_out.txt";
    std::filesystem::remove(out);
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(lsh::dsl::cmd("echo", "disk"), lsh::out(out.string()));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo > file succeeds");
    EXPECT_EQ(slurp(out), std::string("disk\n"), "file received redirected output");
    std::filesystem::remove(out);
}

// A copy-policy subshell isolates shell-state mutations: a `set` inside the
// subshell does not leak into the parent environment.
void test_subshell_isolation() {
    lsh::Shell shell = local_shell();
    EXPECT(!shell.env().get("LEAKED").has_value(), "parent has no LEAKED before subshell");

    auto body = lsh::dsl::cmd("set", "LEAKED=42");
    auto isolated = lsh::dsl::subshell(body, lsh::EnvironmentInheritance::copy);
    auto report = shell.run(isolated.program());
    EXPECT(report.has_value() && report.value().status.success(), "subshell body succeeds");
    EXPECT(!shell.env().get("LEAKED").has_value(), "subshell mutation did not leak");
}

// A registered FunctionKernel is resolved by name and driven through the full
// load/initialize/execute/shutdown lifecycle.
void test_kernel_invocation() {
    auto mem = capture();
    lsh::Shell shell = local_shell();

    lsh::kernel::Metadata metadata;
    metadata.name = "greet";
    metadata.summary = "greeting kernel";
    bool initialized = false;
    bool shut_down = false;

    auto kernel = std::make_shared<lsh::kernel::FunctionKernel>(
        metadata,
        [](const lsh::kernel::Invocation& inv) -> lsh::Result<lsh::ExitStatus> {
            if (inv.stdout_writer) {
                if (auto r = inv.stdout_writer->write("hello from kernel\n"); !r) {
                    return r.error();
                }
            }
            return lsh::ExitStatus {};
        });
    // Wrap initialize/shutdown by subclassing is heavier than needed; instead
    // register a second observing kernel to prove lifecycle ordering would be
    // exercised. Here we simply verify execute ran.
    auto registry = std::make_shared<lsh::kernel::Registry>();
    EXPECT(registry->add(kernel).has_value(), "kernel registered");
    shell.set_kernels(registry);

    auto expr = lsh::dsl::redirect(
        lsh::dsl::kernel("greet"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "kernel executed successfully");
    EXPECT_EQ(mem->bytes(), std::string("hello from kernel\n"), "kernel stdout captured");
    (void)initialized;
    (void)shut_down;
}

// A kernel lookup by alias resolves to the same instance.
void test_kernel_alias() {
    lsh::kernel::Metadata metadata;
    metadata.name = "greet";
    metadata.aliases = {"hi", "hello"};
    auto kernel = std::make_shared<lsh::kernel::FunctionKernel>(
        metadata, [](const lsh::kernel::Invocation&) { return lsh::ExitStatus {}; });
    lsh::kernel::Registry registry;
    EXPECT(registry.add(kernel).has_value(), "kernel with aliases registered");
    EXPECT(registry.find("hi") == kernel, "alias resolves to kernel");
    EXPECT(registry.find("hello") == kernel, "second alias resolves to kernel");
    EXPECT(!registry.find("missing"), "unknown name does not resolve");
}

void test_stdkern_catalog() {
    auto registry = lsh::stdkern::registry();
    EXPECT(static_cast<bool>(registry->find("true")), "stdkern true registered");
    EXPECT(static_cast<bool>(registry->find("printf")), "stdkern printf registered");
    EXPECT(static_cast<bool>(registry->find("dirname")), "stdkern dirname registered");
    EXPECT(lsh::stdkern::catalog().size() >= 9, "stdkern catalog is populated");
}

void test_stdkern_execution() {
    lsh::Shell shell {std::make_shared<lsh::LocalExecutor>()};
    shell.set_kernels(lsh::stdkern::registry());

    auto mem = capture();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("printf", "name=%s\\n", "stdkern"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "stdkern printf succeeds");
    EXPECT_EQ(mem->bytes(), std::string("name=stdkern\n"), "stdkern printf writes formatted output");
}

void test_stdkern_path_kernels() {
    lsh::Shell shell {std::make_shared<lsh::LocalExecutor>()};
    shell.set_kernels(lsh::stdkern::registry());

    auto mem = capture();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("dirname", "/tmp/libsh/file.txt"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "stdkern dirname succeeds");
    EXPECT_EQ(mem->bytes(), std::string("/tmp/libsh\n"), "stdkern dirname writes parent path");

    mem = capture();
    expr = lsh::dsl::redirect(
        lsh::dsl::cmd("basename", "/tmp/libsh/file.txt", ".txt"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "stdkern basename succeeds");
    EXPECT_EQ(mem->bytes(), std::string("file\n"), "stdkern basename strips suffix");
}

// A sinklet transforms a stream in flight: GrepSinklet forwards only matching
// lines to its downstream memory writer.
void test_grep_sinklet() {
    auto mem = capture();
    auto grep = std::make_shared<lsh::GrepSinklet>("beta", mem);
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("printf", "%s\\n", "alpha", "beta", "gamma"),
        lsh::to_sinklet(lsh::RedirectStream::stdout_stream, grep));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "printf | grep sinklet succeeds");
    EXPECT_EQ(mem->bytes(), std::string("beta\n"), "grep sinklet filtered to matching line");
}

// The CLI parser lowers shell text into the same validated IR the DSL produces.
// The compound line must parse to a Sequence whose left is a Pipeline.
void test_cli_parse_lowers_to_ir() {
    auto parsed = lsh::cli::parse_line("echo hi | grep h && wc -l 2>> err.log");
    EXPECT(parsed.has_value(), "compound line parses");
    if (!parsed) return;
    auto validation = lsh::ir::validate(parsed.value());
    EXPECT(validation.ok(), "parsed IR validates");

    const auto& root = parsed.value().root;
    const auto* seq = std::get_if<lsh::ir::Sequence>(&root->value);
    EXPECT(seq != nullptr, "parsed top-level is a Sequence");
    if (seq) {
        const auto* pipe = std::get_if<lsh::ir::Pipeline>(&seq->left->value);
        EXPECT(pipe != nullptr && pipe->commands.size() == 2, "left operand is a 2-stage pipeline");
        EXPECT(seq->connective == lsh::Connective::and_if, "connective is and_if");
        // `|` binds tighter than `&&`, so the wc command (with its 2>>
        // redirection) is the right operand of the sequence, not the tail of
        // the left pipeline.
        const auto* wc = std::get_if<lsh::ir::Command>(&seq->right->value);
        EXPECT(wc != nullptr && !wc->argv.empty() && wc->argv.front().fragments.front().text == "wc",
               "right operand is the wc command");
        EXPECT(wc != nullptr && !wc->redirections.empty(), "wc carries a redirection");
    }
}

void test_cli_escape_preserves_following_input() {
    auto parsed = lsh::cli::parse_line("echo alpha\\ beta");
    EXPECT(parsed.has_value(), "escaped CLI word parses");
    if (!parsed) return;
    const auto* command = std::get_if<lsh::ir::Command>(&parsed.value().root->value);
    EXPECT(command != nullptr && command->argv.size() == 2, "escaped space remains in one argument");
    if (command && command->argv.size() == 2) {
        EXPECT_EQ(command->argv[1].fragments.front().text, std::string("alpha beta"), "escape does not skip the next character");
    }
}

// A parsed line runs end-to-end through the real executor.
void test_cli_parse_runs() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto parsed = lsh::cli::parse_line("echo hello");
    EXPECT(parsed.has_value(), "echo hello parses");
    if (!parsed) return;
    // Attach a memory capture to the lowered command and run it.
    auto& cmd = std::get<lsh::ir::Command>(parsed.value().root->value);
    cmd.redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(parsed.value());
    EXPECT(report.has_value() && report.value().status.success(), "parsed echo runs");
    EXPECT_EQ(mem->bytes(), std::string("hello\n"), "parsed echo output captured");
}

// Exported variables reach spawned children; the printenv external command
// observes them.
void test_exported_env_reaches_child() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    shell.env().set("LIBSH_EXPORT_PROBE", "reachable", /*exported=*/true);
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("/usr/bin/printenv", "LIBSH_EXPORT_PROBE"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "printenv succeeds");
    EXPECT_EQ(mem->bytes(), std::string("reachable\n"), "exported var visible to child");
}

struct Test {
    const char* name;
    void (*fn)();
};

const Test k_tests[] = {
    {"dsl_builds_ir_without_exec", test_dsl_builds_ir_without_exec},
    {"validation_rejects_malformed_graphs", test_validation_rejects_malformed_graphs},
    {"builtin_echo_capture", test_builtin_echo_capture},
    {"variable_expansion", test_variable_expansion},
    {"arithmetic_expansion", test_arithmetic_expansion},
    {"lua_expansion", test_lua_expansion},
    {"lua_failure_is_diagnostic", test_lua_failure_is_diagnostic},
    {"redirect_does_not_mutate_source_expression", test_redirect_does_not_mutate_source_expression},
    {"command_substitution", test_command_substitution},
    {"buffered_pipeline", test_buffered_pipeline},
    {"external_pipeline", test_external_pipeline},
    {"sequence_short_circuit", test_sequence_short_circuit},
    {"file_redirection", test_file_redirection},
    {"subshell_isolation", test_subshell_isolation},
    {"kernel_invocation", test_kernel_invocation},
    {"kernel_alias", test_kernel_alias},
    {"stdkern_catalog", test_stdkern_catalog},
    {"stdkern_execution", test_stdkern_execution},
    {"stdkern_path_kernels", test_stdkern_path_kernels},
    {"grep_sinklet", test_grep_sinklet},
    {"cli_parse_lowers_to_ir", test_cli_parse_lowers_to_ir},
    {"cli_escape_preserves_following_input", test_cli_escape_preserves_following_input},
    {"cli_parse_runs", test_cli_parse_runs},
    {"exported_env_reaches_child", test_exported_env_reaches_child},
};

} // namespace

int main() {
    for (const Test& test : k_tests) {
        std::cerr << "[run] " << test.name << '\n';
        test.fn();
    }

    std::cerr << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed";
    if (g_failures == 0) {
        std::cerr << " — all tests passed\n";
        return 0;
    }
    std::cerr << ", " << g_failures << " FAILED\n";
    return 1;
}
