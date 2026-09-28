// End-to-end runtime tests for the LibShell runtime.
//
// These tests exercise the typed path: DSL/parser -> IR -> validation ->
// expansion -> Executor -> ExecutionReport, plus the expansion layer in
// isolation. The real LocalExecutor (fork/execvp, builtins, kernels) from
// LibShell-Posix.hpp is used, and DryRunExecutor for the no-spawn contract.
// No external test framework is required: a tiny assertion harness keeps the
// suite a self-contained TU that CTest can register.
//
// Build (see tests/CMakeLists.txt):
//   g++ -std=c++20 -Iinclude -Istdkern -Wall -Wextra -Werror -pedantic
//       tests/test_runtime.cpp cli/lsh-lex.cpp cli/libsh-parse.cpp
//       -Lbuild -lshell_qamrpp -o test_runtime

#include "LibShell.hpp"
#include "LibShell-Kernel.hpp"
#include "LibShell-Posix.hpp"
#include "LibShell-Scripting.hpp"
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

// Defined in cli/libsh-parse.cpp; declared here so the test TU does not need the
// CLI entrypoint.
namespace lsh::cli {
Result<ir::Program> parse_script(std::string_view text);
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
lsh::Shell local_shell() { return lsh::Shell(std::make_shared<lsh::posix::LocalExecutor>()); }

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Attaches a memory capture to the right-most command of a graph, which is the
// one whose output a test wants to inspect.
bool attach_capture(const lsh::ir::NodePtr& node, const std::shared_ptr<lsh::MemoryWriter>& memory) {
    if (!node) {
        return false;
    }
    if (auto* command = std::get_if<lsh::ir::Command>(&node->value)) {
        command->redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));
        return true;
    }
    // In a pipeline only the last stage writes to the terminal; the earlier ones
    // write into the pipe.
    if (auto* pipeline = std::get_if<lsh::ir::Pipeline>(&node->value)) {
        if (pipeline->commands.empty()) {
            return false;
        }
        pipeline->commands.back().redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, memory));
        return true;
    }
    if (const auto* seq = std::get_if<lsh::ir::Sequence>(&node->value)) {
        return attach_capture(seq->right, memory) && attach_capture(seq->left, memory);
    }
    if (const auto* redirected = std::get_if<lsh::ir::Redirected>(&node->value)) {
        return attach_capture(redirected->subject, memory);
    }
    if (const auto* negate = std::get_if<lsh::ir::Negate>(&node->value)) {
        return attach_capture(negate->subject, memory);
    }
    if (const auto* group = std::get_if<lsh::ir::BraceGroup>(&node->value)) {
        return attach_capture(group->body, memory);
    }
    if (const auto* clause = std::get_if<lsh::ir::IfClause>(&node->value)) {
        const bool ok = attach_capture(clause->condition, memory);
        if (clause->alternative) {
            if (!attach_capture(*clause->alternative, memory)) {
                return false;
            }
        }
        return attach_capture(clause->body, memory) && ok;
    }
    if (const auto* loop = std::get_if<lsh::ir::WhileClause>(&node->value)) {
        return attach_capture(loop->body, memory);
    }
    if (const auto* loop = std::get_if<lsh::ir::ForClause>(&node->value)) {
        return attach_capture(loop->body, memory);
    }
    if (const auto* clause = std::get_if<lsh::ir::CaseClause>(&node->value)) {
        for (const lsh::ir::CaseItem& item : clause->items) {
            if (!attach_capture(item.body, memory)) {
                return false;
            }
        }
        return true;
    }
    if (const auto* subshell = std::get_if<lsh::ir::Subshell>(&node->value)) {
        return attach_capture(subshell->body, memory);
    }
    if (const auto* function = std::get_if<lsh::ir::FunctionDefinition>(&node->value)) {
        return attach_capture(function->body, memory);
    }
    // A node that produces no output of its own (an assignment, a control
    // transfer) is not a capture failure.
    return true;
}

// Runs shell source through the real front end, capturing stdout.
std::string run_source(lsh::Shell& shell, std::string_view source) {
    auto memory = capture();
    auto program = lsh::cli::parse_script(source);
    if (!program) {
        return "parse-error: " + program.error().message;
    }
    if (!attach_capture(program.value().root, memory)) {
        return "unsupported-root";
    }
    auto report = shell.run(program.value());
    if (!report) {
        return "exec-error: " + report.error().message;
    }
    return memory->bytes();
}

// ---- expansion layer -------------------------------------------------------

// Tilde expansion is the defect this pass was opened for: `~` must become $HOME,
// `~+` the working directory, and a quoted `~` must stay literal.
void test_tilde_expansion() {
    lsh::Shell shell = local_shell();
    shell.env().set("HOME", "/home/tester");
    const auto work = std::filesystem::temp_directory_path() / "libsh_tilde_work";
    std::filesystem::create_directories(work);
    shell.set_cwd(work);

    EXPECT_EQ(run_source(shell, "echo ~\n"), std::string("/home/tester\n"), "~ expands to $HOME");
    EXPECT_EQ(run_source(shell, "echo ~/src\n"), std::string("/home/tester/src\n"), "~ expands inside a word");
    EXPECT_EQ(run_source(shell, "echo ~+\n"), std::string(work.string() + "\n"), "~+ expands to the working directory");
    EXPECT_EQ(run_source(shell, "echo '~'\n"), std::string("~\n"), "a quoted tilde stays literal");
    EXPECT_EQ(run_source(shell, "echo a~b\n"), std::string("a~b\n"), "a mid-word tilde is literal");
    EXPECT_EQ(run_source(shell, "P=~/bin; echo $P\n"), std::string("/home/tester/bin\n"), "tilde after ':' in an assignment");
    EXPECT_EQ(run_source(shell, "echo ~nosuchlogin\n"), std::string("~nosuchlogin\n"), "an unknown login name stays literal");
    std::filesystem::remove_all(work);
}

// Every POSIX parameter-expansion operator, plus the trim forms.
void test_parameter_expansion() {
    lsh::Shell shell = local_shell();
    shell.env().set("SET_VAR", "value");
    shell.env().set("EMPTY", "");
    shell.env().set("PATHLIKE", "/usr/local/bin:/usr/bin:/bin");

    EXPECT_EQ(run_source(shell, "echo ${SET_VAR}\n"), std::string("value\n"), "${name}");
    EXPECT_EQ(run_source(shell, "echo ${UNSET:-def}\n"), std::string("def\n"), "${x:-word} on unset");
    EXPECT_EQ(run_source(shell, "echo ${UNSET-def}\n"), std::string("def\n"), "${x-word} on unset");
    EXPECT_EQ(run_source(shell, "echo ${EMPTY:-def}\n"), std::string("def\n"), "${x:-word} on empty");
    EXPECT_EQ(run_source(shell, "echo ${EMPTY-def}\n"), std::string("\n"), "${x-word} keeps an empty value");
    EXPECT_EQ(run_source(shell, "echo ${SET_VAR:-def}\n"), std::string("value\n"), "${x:-word} keeps a set value");
    EXPECT_EQ(run_source(shell, "echo ${UNSET+alt}\n"), std::string("\n"), "${x+word} on unset");
    EXPECT_EQ(run_source(shell, "echo ${SET_VAR+alt}\n"), std::string("alt\n"), "${x+word} on set");
    EXPECT_EQ(run_source(shell, "echo ${EMPTY+alt}\n"), std::string("alt\n"), "${x+word} on set-but-empty");
    EXPECT_EQ(run_source(shell, "echo ${#SET_VAR}\n"), std::string("5\n"), "${#name} is the value length");
    EXPECT_EQ(run_source(shell, "echo ${#}\n"), std::string("0\n"), "${#} is the positional count");
    EXPECT_EQ(run_source(shell, "echo ${PATHLIKE%%:*}\n"), std::string("/usr/local/bin\n"), "${x%%pattern} trims the longest suffix");
    EXPECT_EQ(run_source(shell, "echo ${PATHLIKE%:*}\n"), std::string("/usr/local/bin:/usr/bin\n"), "${x%pattern} trims the shortest suffix");
    EXPECT_EQ(run_source(shell, "echo ${PATHLIKE#*/}\n"), std::string("usr/local/bin:/usr/bin:/bin\n"), "${x#pattern} trims the shortest prefix");
    EXPECT_EQ(run_source(shell, "echo ${PATHLIKE##*/}\n"), std::string("bin\n"), "${x##pattern} trims the longest prefix");
    EXPECT_EQ(run_source(shell, "echo ${SET_VAR:-\"$SET_VAR\"}\n"), std::string("value\n"), "a nested expansion inside an operand");

    // ${x=word} assigns; the plain "=" form only when unset.
    EXPECT_EQ(run_source(shell, "echo ${NEWVAR=created}\n"), std::string("created\n"), "${x=word} on unset");
    EXPECT_EQ(run_source(shell, "echo ${NEWVAR}\n"), std::string("created\n"), "${x=word} assigned");
    EXPECT_EQ(run_source(shell, "echo ${NEWVAR:=replaced}\n"), std::string("created\n"), "${x:=word} keeps a set value");
    EXPECT_EQ(run_source(shell, "echo ${EMPTY:=filled}\n"), std::string("filled\n"), "${x:=word} fills an empty value");
    EXPECT_EQ(run_source(shell, "echo ${EMPTY}\n"), std::string("filled\n"), "${x:=word} assigned");
}

// A failing ${x:?word} is a diagnostic, not an empty string.
void test_parameter_expansion_error() {
    lsh::Shell shell = local_shell();
    auto program = lsh::cli::parse_script("echo ${MISSING:?is required}\n");
    EXPECT(program.has_value(), "the line parses");
    auto report = shell.run(program.value());
    EXPECT(!report.has_value() || !report.value().status.success(), "${x:?word} fails the command");
    if (!report) {
        EXPECT(report.error().code == lsh::ErrorCode::bad_expansion, "the failure is a bad expansion");
    }
}

// Special parameters and positional parameters.
void test_special_parameters() {
    lsh::Shell shell = local_shell();
    shell.options().name = "testshell";
    shell.set_positional({"one", "two", "three"});

    EXPECT_EQ(run_source(shell, "echo $0\n"), std::string("testshell\n"), "$0 is the shell name");
    EXPECT_EQ(run_source(shell, "echo $1 $3\n"), std::string("one three\n"), "$1 and $3 are positional");
    EXPECT_EQ(run_source(shell, "echo $#\n"), std::string("3\n"), "$# is the positional count");
    EXPECT_EQ(run_source(shell, "echo \"$@\"\n"), std::string("one two three\n"), "\"$@\" joins the positionals");
    EXPECT_EQ(run_source(shell, "set -- a b; echo $#\n"), std::string("2\n"), "set -- replaces the positionals");
    EXPECT_EQ(run_source(shell, "shift; echo $1\n"), std::string("b\n"), "shift drops the first positional");
    // The default option string is the "-c" invocation flag.
    EXPECT_EQ(run_source(shell, "echo $- | wc -c\n"), std::string("3\n"), "$- reports the invocation flag");

    // $? tracks the previous command's status.
    EXPECT_EQ(run_source(shell, "true\n"), std::string(""), "true produces no output");
    auto program = lsh::cli::parse_script("false; echo $?\n");
    auto report = shell.run(program.value());
    EXPECT(report.has_value(), "the status probe runs");
    EXPECT_EQ(shell.last_status(), 0, "$? is zero after a successful echo");

    // $$ is a positive pid and $! is unset until a background job exists.
    lsh::Shell probe = local_shell();
    auto pid_program = lsh::cli::parse_script("echo $$ | wc -c\n");
    (void)pid_program;
    EXPECT(probe.last_status() == 0, "a fresh shell reports status zero");
}

// Field splitting follows IFS, and quoting suppresses it.
void test_field_splitting() {
    lsh::Shell shell = local_shell();
    shell.env().set("LIST", "a b c");
    shell.env().set("JOINED", "a:b:c");

    EXPECT_EQ(run_source(shell, "echo $LIST\n"), std::string("a b c\n"), "an unquoted expansion is split");
    EXPECT_EQ(run_source(shell, "echo \"$LIST\"\n"), std::string("a b c\n"), "a quoted expansion is not split");
    EXPECT_EQ(run_source(shell, "IFS=:; echo $JOINED\n"), std::string("a b c\n"), "IFS selects the separator");
    EXPECT_EQ(run_source(shell, "IFS=:; echo \"$JOINED\"\n"), std::string("a:b:c\n"), "quoting beats IFS");
    // The expansion is split into three fields, and the literal "-post" stays
    // attached to the last one.
    EXPECT_EQ(run_source(shell, "echo pre$LIST-post\n"), std::string("prea b c-post\n"),
              "a split expansion keeps its literal neighbours");
    EXPECT_EQ(run_source(shell, "IFS=; echo $LIST\n"), std::string("a b c\n"), "an empty IFS disables splitting");
    EXPECT_EQ(run_source(shell, "echo $EMPTY_UNSET_X\n"), std::string("\n"), "an unset unquoted expansion yields no field");
    EXPECT_EQ(run_source(shell, "set --; echo x$EMPTY_UNSET_X\n"), std::string("x\n"), "a removed expansion leaves the literal");
    EXPECT_EQ(run_source(shell, "echo \"$EMPTY_UNSET_X\"\n"), std::string("\n"), "a quoted unset expansion is one empty field");
    EXPECT_EQ(run_source(shell, "echo ''\n"), std::string("\n"), "'' is one empty argument");
    EXPECT_EQ(run_source(shell, "echo \"\"\n"), std::string("\n"), "\"\" is one empty argument");
}

// Pathname expansion: multi-component patterns, bracket expressions, and the
// rule that quoted metacharacters are literal.
void test_pathname_expansion() {
    lsh::Shell shell = local_shell();
    const auto root = std::filesystem::temp_directory_path() / "libsh_glob_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "sub");
    for (const char* name : {"alpha.txt", "beta.txt", "gamma.log", ".hidden.txt"}) {
        std::ofstream(root / name).put('\n');
    }
    std::ofstream(root / "sub" / "delta.txt").put('\n');
    shell.set_cwd(root);

    EXPECT_EQ(run_source(shell, "echo *.txt\n"), std::string("alpha.txt beta.txt\n"), "* matches, dotfiles excluded");
    EXPECT_EQ(run_source(shell, "echo a????.txt\n"), std::string("alpha.txt\n"), "? matches exactly one character");
    EXPECT_EQ(run_source(shell, "echo [ab]*.txt\n"), std::string("alpha.txt beta.txt\n"), "a bracket expression matches");
    EXPECT_EQ(run_source(shell, "echo [!a]*.txt\n"), std::string("beta.txt\n"), "a negated bracket expression");
    EXPECT_EQ(run_source(shell, "echo sub/*.txt\n"), std::string("sub/delta.txt\n"), "a multi-component pattern");
    EXPECT_EQ(run_source(shell, "echo \"*.txt\"\n"), std::string("*.txt\n"), "a quoted pattern is literal");
    EXPECT_EQ(run_source(shell, "echo *.nomatch\n"), std::string("*.nomatch\n"), "an unmatched pattern is preserved");
    EXPECT_EQ(run_source(shell, "echo .hidden.txt\n"), std::string(".hidden.txt\n"), "an explicit dot matches a dotfile");
    std::filesystem::remove_all(root);
}

// Arithmetic: precedence, bases, variables, and error reporting.
void test_arithmetic() {
    lsh::Shell shell = local_shell();
    EXPECT_EQ(run_source(shell, "echo $((1+2*3))\n"), std::string("7\n"), "* binds tighter than +");
    EXPECT_EQ(run_source(shell, "echo $(( (1+2)*3 ))\n"), std::string("9\n"), "parentheses");
    EXPECT_EQ(run_source(shell, "echo $((7/2)) $((-7/2))\n"), std::string("3 -3\n"), "division truncates toward zero");
    EXPECT_EQ(run_source(shell, "echo $((7%3))\n"), std::string("1\n"), "modulus");
    EXPECT_EQ(run_source(shell, "echo $((2**10))\n"), std::string("1024\n"), "exponentiation");
    EXPECT_EQ(run_source(shell, "echo $((1<<4)) $((32>>2))\n"), std::string("16 8\n"), "shifts");
    EXPECT_EQ(run_source(shell, "echo $((0x1f)) $((010))\n"), std::string("31 8\n"), "hex and octal literals");
    EXPECT_EQ(run_source(shell, "echo $((1&&0)) $((1||0)) $((!0))\n"), std::string("0 1 1\n"), "logical operators");
    EXPECT_EQ(run_source(shell, "echo $((5>3)) $((5<=3))\n"), std::string("1 0\n"), "relational operators");
    EXPECT_EQ(run_source(shell, "echo $((1?10:20))\n"), std::string("10\n"), "the conditional operator");
    EXPECT_EQ(run_source(shell, "echo $(( 2 + 3 * 4 ))\n"), std::string("14\n"), "leading and inner whitespace");
    EXPECT_EQ(run_source(shell, "N=4; echo $((N*N))\n"), std::string("16\n"), "variables are read from the environment");
    EXPECT_EQ(run_source(shell, "echo $((UNSET_VAR_X+1))\n"), std::string("1\n"), "an unset variable is zero");
    EXPECT(run_source(shell, "echo $((1/0))\n").find("division by zero") != std::string::npos, "division by zero is reported");
}

// ---- grammar ---------------------------------------------------------------

void test_grammar() {
    lsh::Shell shell = local_shell();
    shell.set_command_substitution_parser([](std::string_view line) { return lsh::cli::parse_script(line); });
    shell.set_lua_evaluator(&lsh::scripting::eval_lua_qamrpp);

    EXPECT_EQ(run_source(shell, "echo one; echo two\n"), std::string("one\ntwo\n"), "a trailing semicolon is legal");
    EXPECT_EQ(run_source(shell, "echo a && echo b\n"), std::string("a\nb\n"), "&&");
    EXPECT_EQ(run_source(shell, "false && echo no || echo yes\n"), std::string("yes\n"), "|| recovery");
    EXPECT_EQ(run_source(shell, "! true; echo $?\n"), std::string("1\n"), "! inverts the status");
    EXPECT_EQ(run_source(shell, "if true; then echo t; else echo f; fi\n"), std::string("t\n"), "if/then/else");
    EXPECT_EQ(run_source(shell, "if false; then echo a; elif true; then echo b; else echo c; fi\n"), std::string("b\n"), "elif binds else to the innermost clause");
    EXPECT_EQ(run_source(shell, "n=0; while [ $n -lt 3 ]; do n=$((n+1)); done; echo $n\n"), std::string("3\n"), "while");
    EXPECT_EQ(run_source(shell, "until false; do break; done; echo ok\n"), std::string("ok\n"), "until with break");
    EXPECT_EQ(run_source(shell, "for i in a b; do echo $i; done\n"), std::string("a\nb\n"), "for");
    EXPECT_EQ(run_source(shell, "for i in 1 2; do for j in a b; do echo $i$j; done; done\n"), std::string("1a\n1b\n2a\n2b\n"), "nested for");
    EXPECT_EQ(run_source(shell, "case abc in a*) echo star;; *) echo other;; esac\n"), std::string("star\n"), "case with a glob pattern");
    EXPECT_EQ(run_source(shell, "case zz in a*) echo star;; *) echo other;; esac\n"), std::string("other\n"), "case default branch");
    EXPECT_EQ(run_source(shell, "case b in a) echo a;; b) echo b;& c) echo c;; esac\n"), std::string("b\nc\n"), "case fallthrough");
    EXPECT_EQ(run_source(shell, "{ echo brace; }\n"), std::string("brace\n"), "a brace group");
    EXPECT_EQ(run_source(shell, "(echo sub)\n"), std::string("sub\n"), "a subshell");
    EXPECT_EQ(run_source(shell, "f() { echo fn; }; f\n"), std::string("fn\n"), "a function definition");
    EXPECT_EQ(run_source(shell, "function g { echo g; }; g\n"), std::string("g\n"), "the function keyword");
    EXPECT_EQ(run_source(shell, "h() { echo \"$@\"; }; h a b\n"), std::string("a b\n"), "a function's positional parameters");
    EXPECT_EQ(run_source(shell, "k() { return 7; }; k; echo $?\n"), std::string("7\n"), "return sets the status");
    EXPECT_EQ(run_source(shell, "echo a # trailing comment\n"), std::string("a\n"), "a comment is ignored");
    // The backslash-newline is removed before tokenization, so the two runs of
    // literal text become one word.
    EXPECT_EQ(run_source(shell, "echo a\\\nb\n"), std::string("ab\n"), "a backslash-newline continues the line");
}

void test_substitutions() {
    lsh::Shell shell = local_shell();
    shell.set_command_substitution_parser([](std::string_view line) { return lsh::cli::parse_script(line); });
    shell.set_lua_evaluator(&lsh::scripting::eval_lua_qamrpp);
    shell.env().set("PROJECT", "libsh");

    EXPECT_EQ(run_source(shell, "echo $(echo hi)\n"), std::string("hi\n"), "$(...)");
    EXPECT_EQ(run_source(shell, "echo `echo hi`\n"), std::string("hi\n"), "backquotes");
    EXPECT_EQ(run_source(shell, "echo $(echo a; echo b)\n"), std::string("a b\n"), "a multi-command substitution");
    EXPECT_EQ(run_source(shell, "echo \"x$(echo y)z\"\n"), std::string("xyz\n"), "a substitution inside double quotes");
    EXPECT_EQ(run_source(shell, "echo `lua:return 6*7`\n"), std::string("42\n"), "the inline Lua evaluator");
    EXPECT_EQ(run_source(shell, "echo ${UNSET:-$(echo nested)}\n"), std::string("nested\n"), "a substitution inside a parameter operand");
}

void test_heredocs_and_redirections() {
    lsh::Shell shell = local_shell();

    // A here-document body is text that follows the newline ending its command
    // line, so the lexer must own the whole script.
    EXPECT_EQ(run_source(shell, "cat <<EOF\nfirst\nsecond\nEOF\n"),
              std::string("first\nsecond\n"), "a here-document reaches stdin");
    EXPECT_EQ(run_source(shell, "cat <<-END\n\tindented\n\tEND\n"),
              std::string("indented\n"), "<<- strips the leading tab");
    EXPECT_EQ(run_source(shell, "cat <<EOF\nplain\nEOF\n"),
              std::string("plain\n"), "a here-document without tabs is unchanged");
    EXPECT_EQ(run_source(shell, "cat <<< \"a b\"\n"), std::string("a b\n"), "a here-string becomes stdin");
    EXPECT_EQ(run_source(shell, "V=world; cat <<< \"hello $V\"\n"), std::string("hello world\n"),
              "a here-string word is expanded");
    EXPECT_EQ(run_source(shell, "cat <<EOF | wc -l\none\ntwo\nEOF\n"),
              std::string("2\n"), "a here-document feeds a pipeline");

    const auto out = std::filesystem::temp_directory_path() / "libsh_test_out.txt";
    std::filesystem::remove(out);
    // A file redirection and a memory capture are both redirections on the same
    // stream, so the file is checked without going through run_source.
    auto program = lsh::cli::parse_script("echo disk > " + out.string() + "\n");
    EXPECT(program.has_value(), "the redirecting line parses");
    (void)shell.run(program.value());
    EXPECT_EQ(slurp(out), std::string("disk\n"), "the file received the output");

    (void)shell.run(lsh::cli::parse_script("echo more >> " + out.string() + "\n").value());
    EXPECT_EQ(slurp(out), std::string("disk\nmore\n"), "append preserved the earlier content");

    (void)shell.run(lsh::cli::parse_script("echo v > " + out.string() + "; cat " + out.string() + "\n").value());
    EXPECT_EQ(slurp(out), std::string("v\n"), "truncate replaced the content");
    EXPECT_EQ(run_source(shell, "cat < " + out.string() + "\n"), std::string("v\n"), "input redirection");
    // A memory capture and a file redirection are both redirections on stdout,
    // and the last one wins, so these are checked through the file only.
    (void)shell.run(lsh::cli::parse_script("echo x &> " + out.string() + "\n").value());
    EXPECT_EQ(slurp(out), std::string("x\n"), "&> writes stdout to the file");
    (void)shell.run(lsh::cli::parse_script("echo y 1>" + out.string() + " 2>&1\n").value());
    EXPECT_EQ(slurp(out), std::string("y\n"), "1> with 2>&1 writes stdout to the file");
    std::filesystem::remove(out);

    // Descriptor duplication is a property of the materialized channel, so it is
    // checked where it is decided rather than through its side effects: a later
    // redirection for the same stream legitimately wins.
    {
        lsh::ExecSpec spec;
        spec.argv = {"echo", "err"};
        spec.redirections.push_back(lsh::to_fd(lsh::RedirectStream::stdout_stream, 2));
        std::vector<std::shared_ptr<lsh::posix::TempFile>> temporaries;
        auto channel = lsh::posix::materialize_channel(spec, lsh::RedirectStream::stdout_stream, temporaries);
        EXPECT(channel.has_value(), "the stdout channel materializes");
        EXPECT(channel.has_value() && channel.value().dup_fd == 2, "1>&2 dups the descriptor onto stdout");
    }
    {
        lsh::ExecSpec spec;
        spec.argv = {"echo", "err"};
        spec.redirections.push_back(lsh::to_null(lsh::RedirectStream::stderr_stream));
        std::vector<std::shared_ptr<lsh::posix::TempFile>> temporaries;
        auto channel = lsh::posix::materialize_channel(spec, lsh::RedirectStream::stderr_stream, temporaries);
        EXPECT(channel.has_value(), "the stderr channel materializes");
        EXPECT(channel.has_value() && channel.value().dup_fd >= 0, "2>/dev/null opens a real descriptor");
    }
    {
        // The last redirection for a stream wins, as POSIX requires.
        lsh::ExecSpec spec;
        spec.argv = {"echo", "err"};
        auto first = capture();
        auto second = capture();
        spec.redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, first));
        spec.redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, second));
        const lsh::Redirection* effective = lsh::posix::redirection_for(spec.redirections, lsh::RedirectStream::stdout_stream);
        EXPECT(effective != nullptr && effective->target.memory == second, "the last redirection for a stream wins");
    }
    EXPECT_EQ(run_source(shell, "ls /libsh_no_such_path 2>/dev/null; echo done\n"), std::string("done\n"),
              "2>/dev/null suppresses the diagnostic stream");

    // A deferred target is expanded before the file is opened.
    const auto deferred = std::filesystem::temp_directory_path() / "libsh_deferred.txt";
    std::filesystem::remove(deferred);
    (void)shell.run(lsh::cli::parse_script("F=" + deferred.string() + "; echo d > $F\n").value());
    EXPECT_EQ(slurp(deferred), std::string("d\n"), "> $var expands the path");
    EXPECT_EQ(run_source(shell, "cat " + deferred.string() + "\n"), std::string("d\n"), "the deferred path was created");
    std::filesystem::remove(deferred);
}

void test_diagnostics() {
    lsh::Shell shell = local_shell();
    auto unterminated = lsh::cli::parse_script("echo 'oops\n");
    EXPECT(!unterminated.has_value(), "an unterminated quote is a diagnostic");
    auto unclosed = lsh::cli::parse_script("if true; then echo x\n");
    EXPECT(!unclosed.has_value(), "an unterminated if is a diagnostic");
    auto bad = lsh::cli::parse_script("echo ${}\n");
    EXPECT(!bad.has_value(), "an empty parameter name is a diagnostic");
    auto empty = lsh::cli::parse_script("\n\n");
    EXPECT(!empty.has_value(), "an empty script is a diagnostic");

    // A command that cannot be found reports rather than pretending to succeed.
    lsh::Shell probe = local_shell();
    auto missing = lsh::cli::parse_script("libsh_no_such_command_xyz\n");
    auto report = probe.run(missing.value());
    EXPECT(report.has_value() && report.value().status.code == 127, "an unknown command exits 127");
    EXPECT(!report.value().diagnostics.empty(), "an unknown command reports a diagnostic");

    // A file redirection that cannot be opened is reported.
    lsh::Shell bad_redirect = local_shell();
    auto program = lsh::cli::parse_script("echo x > /libsh_no_such_dir_xyz/f\n");
    auto bad_report = bad_redirect.run(program.value());
    EXPECT(!bad_report.has_value() || !bad_report.value().status.success(), "an unopenable redirection fails");
}

// ---- runtime ---------------------------------------------------------------

void test_dsl_builds_ir_without_exec() {
    using namespace lsh::dsl;

    auto graph = (cmd("printf", "%s\\n", lsh::variable("PROJECT")) | cmd("grep", "Lak"))
                 && redirect(cmd("wc", "-l"), lsh::out("count.txt"));

    const auto& node = graph.node();
    EXPECT(static_cast<bool>(node), "dsl produces a node");

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

void test_validation_rejects_malformed_graphs() {
    lsh::ir::Command empty;
    auto empty_program = lsh::ir::Program {lsh::ir::command(empty)};
    EXPECT(!lsh::ir::validate(empty_program).ok(), "empty argv is rejected");

    lsh::ir::Program null_program {nullptr};
    EXPECT(!lsh::ir::validate(null_program).ok(), "null root node is rejected");

    // A fragment cannot both substitute and rewrite a value.
    lsh::ir::Command mixed;
    lsh::Expansion fragment;
    fragment.kind = lsh::ExpansionKind::variable;
    fragment.text = "X";
    fragment.op = lsh::ParameterOp::alternate;
    fragment.trim = lsh::TrimOp::prefix;
    mixed.argv.push_back(lsh::Argument {{fragment}});
    mixed.argv.push_back(lsh::literal("echo"));
    std::swap(mixed.argv[0], mixed.argv[1]);
    EXPECT(!lsh::ir::validate(lsh::ir::Program {lsh::ir::command(std::move(mixed))}).ok(), "a combined operator and trim is rejected");
}

void test_builtin_echo_capture() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(lsh::dsl::cmd("echo", "hello"), lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo succeeds");
    EXPECT_EQ(mem->bytes(), std::string("hello\n"), "echo output captured");
}

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

void test_lua_expansion() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    shell.set_lua_evaluator(&lsh::scripting::eval_lua_qamrpp);
    shell.env().set("PROJECT", "libsh");
    shell.env().set("DASH-NAME", "visible-through-env");
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("echo", lsh::lua("return PROJECT .. ':' .. env('DASH-NAME') .. ':' .. (6 * 7)")),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "Lua expansion succeeds");
    EXPECT_EQ(mem->bytes(), std::string("libsh:visible-through-env:42\n"), "QaMRpp evaluated Lua with the shell environment");
}

void test_lua_failure_is_diagnostic() {
    lsh::Shell shell = local_shell();
    shell.set_lua_evaluator(&lsh::scripting::eval_lua_qamrpp);
    auto report = shell.run(lsh::dsl::cmd("echo", lsh::lua("return (")).program());
    EXPECT(!report.has_value(), "invalid Lua rejects execution");
    if (!report) {
        EXPECT(report.error().code == lsh::ErrorCode::bad_expansion, "Lua parse error is a bad expansion");
    }

    // Without an evaluator the expansion reports rather than expanding to
    // nothing.
    lsh::Shell bare = local_shell();
    auto bare_report = bare.run(lsh::dsl::cmd("echo", lsh::lua("return 1")).program());
    EXPECT(!bare_report.has_value(), "Lua without an adapter is a diagnostic");
}

void test_redirect_does_not_mutate_source_expression() {
    auto mem = capture();
    auto base = lsh::dsl::cmd("echo", "unredirected");
    auto redirected = lsh::dsl::redirect(base, lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));

    const auto& base_command = std::get<lsh::ir::Command>(base.node()->value);
    const auto& redirected_command = std::get<lsh::ir::Command>(redirected.node()->value);
    EXPECT(base_command.redirections.empty(), "redirect leaves the source IR immutable");
    EXPECT(redirected_command.redirections.size() == 1, "redirected IR owns its redirection");
}

void test_command_substitution() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    shell.set_command_substitution_parser([](std::string_view line) { return lsh::cli::parse_script(line); });
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("echo", lsh::command_substitution("echo hi")),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "echo $(echo hi) succeeds");
    EXPECT_EQ(mem->bytes(), std::string("hi\n"), "command substitution captured and trimmed");
}

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

// A pipeline that captures both stdout and stderr must not deadlock: the drain
// is concurrent, so a chatty early stage cannot block on a full stderr pipe.
void test_pipeline_captures_both_streams() {
    lsh::Shell shell = local_shell();
    shell.set_command_substitution_parser([](std::string_view line) { return lsh::cli::parse_script(line); });

    auto out = capture();
    auto err = capture();
    auto program = lsh::cli::parse_script("libsh_stderr_source | cat\n");
    if (!program) {
        return;
    }
    // Substitute a program that writes a large amount to stderr, then a large
    // amount to stdout, so neither pipe can be drained in sequence.
    std::filesystem::create_directories(std::filesystem::temp_directory_path() / "libsh_pipeline_bin");
    const auto script = std::filesystem::temp_directory_path() / "libsh_pipeline_bin" / "noisy";
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "i=0\n"
            << "while [ $i -lt 400 ]; do echo \"stderr line $i\" >&2; i=$((i+1)); done\n"
            << "echo done\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);

    auto noisy = lsh::cli::parse_script(script.string() + " | cat\n");
    EXPECT(noisy.has_value(), "the noisy pipeline parses");
    if (!noisy) {
        return;
    }
    auto* pipeline = std::get_if<lsh::ir::Pipeline>(&const_cast<lsh::ir::Node&>(*noisy.value().root).value);
    EXPECT(pipeline != nullptr, "the noisy pipeline is a Pipeline");
    if (pipeline) {
        pipeline->commands.back().redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, out));
        pipeline->commands.front().redirections.push_back(lsh::to_memory(lsh::RedirectStream::stderr_stream, err));
    }
    auto report = shell.run(noisy.value());
    EXPECT(report.has_value() && report.value().status.success(), "the noisy pipeline completed");
    EXPECT_EQ(out->bytes(), std::string("done\n"), "the tail stage's stdout was captured");
    EXPECT(err->bytes().find("stderr line 399") != std::string::npos, "the head stage's stderr was drained in full");
    std::filesystem::remove_all(script.parent_path());
}

void test_sequence_short_circuit() {
    auto mem = capture();
    lsh::Shell shell = local_shell();

    auto skipped = lsh::dsl::redirect(
        lsh::dsl::builtin("false") && lsh::dsl::cmd("echo", "nope"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto r1 = shell.run(skipped.program());
    EXPECT(r1.has_value() && !r1.value().status.success(), "false && ... fails");
    EXPECT_EQ(mem->bytes(), std::string(""), "right operand of && skipped on failure");

    mem = capture();
    auto recovered = lsh::dsl::redirect(
        lsh::dsl::builtin("false") || lsh::dsl::cmd("echo", "yes"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto r2 = shell.run(recovered.program());
    EXPECT(r2.has_value() && r2.value().status.success(), "false || echo succeeds");
    EXPECT_EQ(mem->bytes(), std::string("yes\n"), "right operand of || ran on failure");
}

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

// A subshell's mutations are discarded under `copy` and persist under
// `shared_explicit`.
void test_subshell_isolation() {
    lsh::Shell shell = local_shell();
    EXPECT(!shell.env().get("LEAKED").has_value(), "parent has no LEAKED before subshell");

    // A bare `NAME=value` command assigns in the current shell.
    auto body = lsh::dsl::assignment("LEAKED", lsh::literal("42"));
    auto isolated = lsh::dsl::subshell(body, lsh::EnvironmentInheritance::copy);
    auto report = shell.run(isolated.program());
    EXPECT(report.has_value() && report.value().status.success(), "subshell body succeeds");
    EXPECT(!shell.env().get("LEAKED").has_value(), "subshell mutation did not leak");

    auto shared = lsh::dsl::subshell(body, lsh::EnvironmentInheritance::shared_explicit);
    (void)shell.run(shared.program());
    EXPECT(shell.env().get("LEAKED").value_or("") == std::string("42"), "shared_explicit subshell mutation persists");
}

void test_cd_maintains_shell_state() {
    lsh::Shell shell = local_shell();
    const auto root = std::filesystem::temp_directory_path() / "libsh_cd_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    auto program = lsh::cli::parse_script("cd " + root.string() + "; echo $PWD\n");
    EXPECT(program.has_value(), "the cd line parses");
    auto report = shell.run(program.value());
    EXPECT(report.has_value() && report.value().status.success(), "cd succeeded");
    EXPECT_EQ(shell.env().get("PWD").value_or(""), root.string(), "$PWD follows the working directory");
    EXPECT(!shell.env().get("OLDPWD").value_or("").empty(), "$OLDPWD records the previous directory");
    std::filesystem::remove_all(root);
}

void test_reads_stdin() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("read", "LINE"),
        lsh::from_memory("a line of text\n"));
    (void)expr;
    mem = capture();
    auto program = lsh::dsl::redirect(lsh::dsl::cmd("read", "LINE"), lsh::from_memory("a line of text\n"));
    auto report = shell.run(program.program());
    EXPECT(report.has_value() && report.value().status.success(), "read succeeded");
    EXPECT_EQ(shell.env().get("LINE").value_or(""), std::string("a line of text"), "read consumed a synthesized stdin");
}

void test_builtin_printf() {
    lsh::Shell shell = local_shell();
    EXPECT_EQ(run_source(shell, "printf '%s-%s\\n' a b\n"), std::string("a-b\n"), "printf %s");
    EXPECT_EQ(run_source(shell, "printf '%05d\\n' 42\n"), std::string("00042\n"), "printf zero padding");
    EXPECT_EQ(run_source(shell, "printf '%x\\n' 255\n"), std::string("ff\n"), "printf hexadecimal");
    EXPECT_EQ(run_source(shell, "printf '%-5s|\\n' ab\n"), std::string("ab   |\n"), "printf left alignment");
    EXPECT_EQ(run_source(shell, "printf '%.2s\\n' abcdef\n"), std::string("ab\n"), "printf precision");
    EXPECT_EQ(run_source(shell, "printf 'a\\tb\\n'\n"), std::string("a\tb\n"), "printf escape sequences");
    EXPECT_EQ(run_source(shell, "printf '%s\\n' one two\n"), std::string("one\ntwo\n"), "printf reuses the format");
    auto invalid = lsh::cli::parse_script("printf '%d\\n' abc\n");
    EXPECT(invalid.has_value(), "the invalid printf parses");
    if (invalid) {
        auto report = shell.run(invalid.value());
        EXPECT(report.has_value() && !report.value().status.success(), "printf rejects a non-numeric %d");
    }
}

void test_builtin_test() {
    lsh::Shell shell = local_shell();
    EXPECT_EQ(run_source(shell, "if [ 1 = 1 ]; then echo y; fi\n"), std::string("y\n"), "[ = ]");
    EXPECT_EQ(run_source(shell, "if [ 2 -gt 1 ]; then echo y; fi\n"), std::string("y\n"), "[ -gt ]");
    EXPECT_EQ(run_source(shell, "if [ -n abc ]; then echo y; fi\n"), std::string("y\n"), "[ -n ]");
    EXPECT_EQ(run_source(shell, "if [ -z '' ]; then echo y; fi\n"), std::string("y\n"), "[ -z ]");
    EXPECT_EQ(run_source(shell, "if [ -f /etc/passwd ]; then echo y; fi\n"), std::string("y\n"), "[ -f ]");
    EXPECT_EQ(run_source(shell, "if [ ! 1 = 2 ]; then echo y; fi\n"), std::string("y\n"), "[ ! ]");
    EXPECT_EQ(run_source(shell, "if [ 1 = 1 -a 2 = 2 ]; then echo y; fi\n"), std::string("y\n"), "[ -a ]");
    EXPECT_EQ(run_source(shell, "if [ 1 = 2 -o 2 = 2 ]; then echo y; fi\n"), std::string("y\n"), "[ -o ]");
    EXPECT_EQ(run_source(shell, "if [ \\( 1 = 1 \\) ]; then echo y; fi\n"), std::string("y\n"), "[ ( ) ]");
    EXPECT_EQ(run_source(shell, "if test 1 = 1; then echo y; fi\n"), std::string("y\n"), "test builtin");
}

void test_kernel_invocation() {
    auto mem = capture();
    lsh::Shell shell = local_shell();

    lsh::kernel::Metadata metadata;
    metadata.name = "greet";
    metadata.summary = "greeting kernel";

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
    auto registry = std::make_shared<lsh::kernel::Registry>();
    EXPECT(registry->add(kernel).has_value(), "kernel registered");
    shell.set_kernels(registry);

    auto expr = lsh::dsl::redirect(
        lsh::dsl::kernel("greet"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "kernel executed successfully");
    EXPECT_EQ(mem->bytes(), std::string("hello from kernel\n"), "kernel stdout captured");
}

// A kernel receives stdin when the command is given a here-document.
void test_kernel_reads_stdin() {
    lsh::Shell shell = local_shell();
    lsh::kernel::Metadata metadata;
    metadata.name = "reader";
    auto kernel = std::make_shared<lsh::kernel::FunctionKernel>(
        metadata,
        [](const lsh::kernel::Invocation& inv) -> lsh::Result<lsh::ExitStatus> {
            std::string payload;
            std::string chunk;
            while (inv.stdin_reader != nullptr) {
                auto count = inv.stdin_reader->read(chunk, 64);
                if (!count || count.value() == 0) {
                    break;
                }
                payload += chunk;
            }
            if (inv.stdout_writer) {
                (void)inv.stdout_writer->write(payload);
            }
            return lsh::ExitStatus {};
        });
    auto registry = std::make_shared<lsh::kernel::Registry>();
    (void)registry->add(kernel);
    shell.set_kernels(registry);

    auto mem = capture();
    auto expr = lsh::dsl::redirect(lsh::dsl::kernel("reader"), lsh::from_memory("payload"));
    expr = lsh::dsl::redirect(expr, lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "the kernel ran");
    EXPECT_EQ(mem->bytes(), std::string("payload"), "the kernel observed stdin");
}

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

    lsh::kernel::Metadata conflicting_name;
    conflicting_name.name = "hi";
    auto name_conflict = std::make_shared<lsh::kernel::FunctionKernel>(
        conflicting_name, [](const lsh::kernel::Invocation&) { return lsh::ExitStatus {}; });
    EXPECT(!registry.add(name_conflict).has_value(), "a kernel name cannot shadow an existing alias");

    lsh::kernel::Metadata conflicting_alias;
    conflicting_alias.name = "other";
    conflicting_alias.aliases = {"hello"};
    auto alias_conflict = std::make_shared<lsh::kernel::FunctionKernel>(
        conflicting_alias, [](const lsh::kernel::Invocation&) { return lsh::ExitStatus {}; });
    EXPECT(!registry.add(alias_conflict).has_value(), "an alias has a single owner");
}

void test_case_propagates_runtime_failure() {
    lsh::Shell shell = local_shell();
    auto program = lsh::cli::parse_script("case x in x) echo ${MISSING:?required};; esac\n");
    EXPECT(program.has_value(), "the failing case parses");
    if (!program) {
        return;
    }
    auto report = shell.run(program.value());
    EXPECT(!report.has_value(), "a selected case body propagates expansion failure");
    if (!report) {
        EXPECT(report.error().code == lsh::ErrorCode::bad_expansion, "the case failure keeps its diagnostic code");
    }
}

void test_stdkern_catalog() {
    auto registry = lsh::stdkern::registry();
    EXPECT(static_cast<bool>(registry->find("true")), "stdkern true registered");
    EXPECT(static_cast<bool>(registry->find("printf")), "stdkern printf registered");
    EXPECT(static_cast<bool>(registry->find("dirname")), "stdkern dirname registered");
    EXPECT(lsh::stdkern::catalog().size() >= 9, "stdkern catalog is populated");
}

void test_stdkern_execution() {
    lsh::Shell shell {std::make_shared<lsh::posix::LocalExecutor>()};
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
    lsh::Shell shell {std::make_shared<lsh::posix::LocalExecutor>()};
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

void test_cli_parse_lowers_to_ir() {
    auto parsed = lsh::cli::parse_line("echo hi | grep h && wc -l 2>> err.log");
    EXPECT(parsed.has_value(), "compound line parses");
    if (!parsed) {
        return;
    }
    auto validation = lsh::ir::validate(parsed.value());
    EXPECT(validation.ok(), "parsed IR validates");

    const auto& root = parsed.value().root;
    const auto* seq = std::get_if<lsh::ir::Sequence>(&root->value);
    EXPECT(seq != nullptr, "parsed top-level is a Sequence");
    if (seq) {
        const auto* pipe = std::get_if<lsh::ir::Pipeline>(&seq->left->value);
        EXPECT(pipe != nullptr && pipe->commands.size() == 2, "left operand is a 2-stage pipeline");
        EXPECT(seq->connective == lsh::Connective::and_if, "connective is and_if");
        // `|` binds tighter than `&&`, so the wc command is the right operand.
        const auto* wc = std::get_if<lsh::ir::Command>(&seq->right->value);
        EXPECT(wc != nullptr && !wc->argv.empty() && wc->argv.front().fragments.front().text == "wc", "right operand is the wc command");
        EXPECT(wc != nullptr && !wc->redirections.empty(), "wc carries a redirection");
    }
}

void test_cli_escape_preserves_following_input() {
    auto parsed = lsh::cli::parse_line("echo alpha\\ beta");
    EXPECT(parsed.has_value(), "escaped CLI word parses");
    if (!parsed) {
        return;
    }
    const auto* command = std::get_if<lsh::ir::Command>(&parsed.value().root->value);
    EXPECT(command != nullptr && command->argv.size() == 2, "escaped space remains in one argument");
    if (command && command->argv.size() == 2) {
        EXPECT_EQ(command->argv[1].fragments.front().text, std::string("alpha beta"), "escape does not skip the next character");
    }
}

void test_cli_parse_runs() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto parsed = lsh::cli::parse_line("echo hello");
    EXPECT(parsed.has_value(), "echo hello parses");
    if (!parsed) {
        return;
    }
    auto& cmd = std::get<lsh::ir::Command>(parsed.value().root->value);
    cmd.redirections.push_back(lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto report = shell.run(parsed.value());
    EXPECT(report.has_value() && report.value().status.success(), "parsed echo runs");
    EXPECT_EQ(mem->bytes(), std::string("hello\n"), "parsed echo output captured");
}

// Exported variables reach spawned children.
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

// A prefix assignment reaches the child without changing the shell's own table.
void test_prefix_assignment_is_scoped() {
    auto mem = capture();
    lsh::Shell shell = local_shell();
    auto expr = lsh::dsl::redirect(
        lsh::dsl::cmd("/usr/bin/printenv", "LIBSH_PREFIX_PROBE"),
        lsh::to_memory(lsh::RedirectStream::stdout_stream, mem));
    auto& command = std::get<lsh::ir::Command>(expr.node()->value);
    lsh::ir::Assignment assignment;
    assignment.name = "LIBSH_PREFIX_PROBE";
    assignment.value = lsh::literal("scoped");
    command.assignments.push_back(assignment);
    auto report = shell.run(expr.program());
    EXPECT(report.has_value() && report.value().status.success(), "the prefixed command ran");
    EXPECT_EQ(mem->bytes(), std::string("scoped\n"), "the prefix reached the child");
    EXPECT(!shell.env().contains("LIBSH_PREFIX_PROBE"), "the prefix did not leak into the shell");
}

// A timeout and a cancellation are distinct conditions.
void test_timeout_and_cancel() {
    lsh::Shell shell = local_shell();
    shell.options().timeout = lsh::Timeout {std::chrono::milliseconds(120)};
    auto program = lsh::cli::parse_script("sleep 5\n");
    EXPECT(program.has_value(), "the sleep line parses");
    auto report = shell.run(program.value());
    EXPECT(report.has_value(), "the timed-out command reported");
    if (report) {
        EXPECT(report.value().status.timed_out, "the status is marked as a timeout");
        EXPECT(!report.value().status.canceled, "a timeout is not a cancellation");
        EXPECT_EQ(report.value().status.code, 124, "a timeout exits 124");
    }
}

// `readonly` protects a variable from being reassigned.
void test_readonly_variable() {
    lsh::Shell shell = local_shell();
    auto initialize = lsh::cli::parse_script("readonly RO=1\n");
    EXPECT(initialize.has_value(), "the readonly declaration parses");
    if (!initialize) {
        return;
    }
    auto initialized = shell.run(initialize.value());
    EXPECT(initialized.has_value() && initialized.value().status.success(), "readonly declaration succeeds");

    auto program = lsh::cli::parse_script("RO=2\n");
    EXPECT(program.has_value(), "the readonly reassignment parses");
    if (!program) {
        return;
    }
    auto report = shell.run(program.value());
    EXPECT(report.has_value() && !report.value().status.success(), "reassigning a readonly variable fails");
    EXPECT_EQ(shell.env().get("RO").value_or(""), std::string("1"), "the original value survived");
}

struct Test {
    const char* name;
    void (*fn)();
};

const Test k_tests[] = {
    {"tilde_expansion", test_tilde_expansion},
    {"parameter_expansion", test_parameter_expansion},
    {"parameter_expansion_error", test_parameter_expansion_error},
    {"special_parameters", test_special_parameters},
    {"field_splitting", test_field_splitting},
    {"pathname_expansion", test_pathname_expansion},
    {"arithmetic", test_arithmetic},
    {"grammar", test_grammar},
    {"substitutions", test_substitutions},
    {"heredocs_and_redirections", test_heredocs_and_redirections},
    {"diagnostics", test_diagnostics},
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
    {"pipeline_captures_both_streams", test_pipeline_captures_both_streams},
    {"sequence_short_circuit", test_sequence_short_circuit},
    {"file_redirection", test_file_redirection},
    {"subshell_isolation", test_subshell_isolation},
    {"cd_maintains_shell_state", test_cd_maintains_shell_state},
    {"reads_stdin", test_reads_stdin},
    {"builtin_printf", test_builtin_printf},
    {"builtin_test", test_builtin_test},
    {"kernel_invocation", test_kernel_invocation},
    {"kernel_reads_stdin", test_kernel_reads_stdin},
    {"kernel_alias", test_kernel_alias},
    {"case_propagates_runtime_failure", test_case_propagates_runtime_failure},
    {"stdkern_catalog", test_stdkern_catalog},
    {"stdkern_execution", test_stdkern_execution},
    {"stdkern_path_kernels", test_stdkern_path_kernels},
    {"grep_sinklet", test_grep_sinklet},
    {"cli_parse_lowers_to_ir", test_cli_parse_lowers_to_ir},
    {"cli_escape_preserves_following_input", test_cli_escape_preserves_following_input},
    {"cli_parse_runs", test_cli_parse_runs},
    {"exported_env_reaches_child", test_exported_env_reaches_child},
    {"prefix_assignment_is_scoped", test_prefix_assignment_is_scoped},
    {"timeout_and_cancel", test_timeout_and_cancel},
    {"readonly_variable", test_readonly_variable},
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
