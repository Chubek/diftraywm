#include "lsh-lex.hpp"

#include "LibShell.hpp"

// Redirection constructors live in the DSL layer.
#include "lsh/DSL.hpp"

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lsh::cli {

namespace {

using TokenList = std::deque<Token>;

[[nodiscard]] bool is_digits(std::string_view text) {
    return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
}

[[nodiscard]] std::string literal_of(const Token& token) {
    std::string text;
    for (const Expansion& fragment : token.fragments) {
        text += fragment.text;
    }
    return text;
}

// `NAME=value` as a single token. The lexer coalesces adjacent literal
// characters, so `FOO=bar` arrives as one raw fragment while `FOO=$x` arrives as
// a raw head followed by expansions. Both forms are accepted; anything quoted
// is an ordinary word.
[[nodiscard]] std::optional<std::size_t> assignment_split(const Token& token) {
    if (token.quoted || token.fragments.empty()) {
        return std::nullopt;
    }
    const Expansion& head = token.fragments.front();
    if (head.kind != ExpansionKind::raw) {
        return std::nullopt;
    }
    const auto eq = head.text.find('=');
    if (eq == std::string::npos || !valid_variable_name(std::string_view(head.text).substr(0, eq))) {
        return std::nullopt;
    }
    return eq;
}

[[nodiscard]] bool is_assignment_token(const Token& token) {
    const auto split = assignment_split(token);
    if (!split) {
        return false;
    }
    // A bare `NAME=` with nothing after it is still an assignment; a value made
    // only of later fragments is too. Anything else is a word.
    return true;
}

[[nodiscard]] ir::Assignment assignment_of(const Token& token) {
    ir::Assignment assignment;
    const std::size_t eq = *assignment_split(token);
    const Expansion& head = token.fragments.front();
    assignment.name = head.text.substr(0, eq);

    if (head.text.size() > eq + 1) {
        assignment.value.fragments.push_back(make_expansion(ExpansionKind::raw, head.text.substr(eq + 1)));
    }
    for (std::size_t index = 1; index < token.fragments.size(); ++index) {
        assignment.value.fragments.push_back(token.fragments[index]);
    }
    if (assignment.value.fragments.empty()) {
        assignment.value.fragments.push_back(make_expansion(ExpansionKind::raw));
    }
    return assignment;
}

// Recursive-descent parser for the POSIX shell grammar:
//
//   program   := linebreak complete_commands
//   list      := and_or { separator_op and_or }
//   and_or    := pipeline { ('&&'|'||') linebreak pipeline }
//   pipeline  := '!'? command { '|' linebreak? command }
//   command   := brace_group | subshell | for_clause | case_clause
//              | if_clause | while_clause | until_clause | function_def
//              | simple_command
//
// The parser produces IR only: it never expands, resolves, or executes.
// Expansion rules live in the Expander, execution policy in the Executor.
class Parser {
public:
    explicit Parser(TokenList tokens) : tokens_(std::move(tokens)) {}

    Result<ir::Program> parse_program() {
        skip_newlines();
        if (at_end()) {
            return failure(ErrorCode::empty_argv, "empty command");
        }
        auto expression = parse_list();
        if (!expression) {
            return expression.error();
        }
        skip_newlines();
        if (!at_end()) {
            return failure(ErrorCode::syntax_error, "unexpected token after the end of the command", peek().text);
        }
        return ir::Program {expression.value()};
    }

private:
    // ---- token helpers ----------------------------------------------------

    // The stream is terminated by an explicit end token so peek() is always
    // well defined; at_end() means "reached it".
    [[nodiscard]] bool at_end() const noexcept {
        return current_ >= tokens_.size() || peek().kind == TokenKind::end;
    }
    [[nodiscard]] const Token& peek(std::size_t ahead = 0) const {
        const std::size_t index = current_ + ahead;
        return index < tokens_.size() ? tokens_[index] : tokens_.back();
    }
    const Token& advance() { return at_end() ? tokens_.back() : tokens_[current_++]; }

    [[nodiscard]] bool at_operator(Operator op) const {
        return peek().kind == TokenKind::operator_token && peek().op == op;
    }
    [[nodiscard]] bool at_separator() const {
        if (peek().kind == TokenKind::newline) {
            return true;
        }
        if (peek().kind != TokenKind::operator_token) {
            return false;
        }
        return peek().op == Operator::semicolon || peek().op == Operator::amp
            || peek().op == Operator::double_semicolon || peek().op == Operator::semicolon_and;
    }
    // A reserved word, which is recognized only when the token is unquoted and
    // spells it exactly.
    [[nodiscard]] bool at_reserved(std::string_view text) const {
        return peek().kind == TokenKind::word && peek().reserved && peek().text == text;
    }
    bool match_operator(Operator op) {
        if (!at_operator(op)) {
            return false;
        }
        ++current_;
        return true;
    }
    bool match_reserved(std::string_view text) {
        if (!at_reserved(text)) {
            return false;
        }
        ++current_;
        return true;
    }
    void skip_newlines() {
        while (peek().kind == TokenKind::newline) {
            ++current_;
        }
    }
    bool match_newlines() {
        const bool any = peek().kind == TokenKind::newline;
        skip_newlines();
        return any;
    }
    // Reserved words that close the construct a list belongs to. A list ends
    // when one of these follows a separator, so `if cmd; then ...` does not try
    // to parse `then` as a command.
    [[nodiscard]] bool at_clause_end() const {
        return at_reserved("then") || at_reserved("do") || at_reserved("done") || at_reserved("esac")
            || at_reserved("elif") || at_reserved("else") || at_reserved("fi") || at_reserved("}")
            || at_reserved("in");
    }

    [[nodiscard]] bool at_list_end() const {
        return at_end() || at_separator() || at_clause_end() || at_operator(Operator::subshell_close);
    }

    [[nodiscard]] static ir::NodePtr empty_list() {
        // `:` is the identity command, so an empty list is a real, harmless node
        // rather than a null pointer the validator would reject.
        return ir::command(ir::make_command({":"}), "empty-list");
    }

    // ---- grammar ----------------------------------------------------------

    // A list is one or more and_or terms joined by separators. A trailing
    // separator (`echo a;`) is legal, and so is an empty list (`{ ; }`).
    Result<ir::NodePtr> parse_list() {
        skip_newlines();
        if (at_list_end()) {
            return empty_list();
        }
        auto left = parse_and_or();
        if (!left) {
            return left.error();
        }
        for (;;) {
            const std::size_t saved = current_;
            if (peek().kind == TokenKind::newline) {
                skip_newlines();
            }
            if (!at_operator(Operator::semicolon)) {
                current_ = saved;
                break;
            }
            advance();
            skip_newlines();
            if (at_list_end()) {
                break;
            }
            auto right = parse_and_or();
            if (!right) {
                return right.error();
            }
            ir::Sequence sequence;
            sequence.left = left.value();
            sequence.right = right.value();
            sequence.connective = Connective::sequence;
            left = ir::node(std::move(sequence), "sequence");
        }
        return left;
    }

    Result<ir::NodePtr> parse_and_or() {
        auto left = parse_pipeline();
        if (!left) {
            return left.error();
        }
        for (;;) {
            skip_newlines();
            if (!at_operator(Operator::and_if) && !at_operator(Operator::or_if)) {
                return left;
            }
            const Connective connective = at_operator(Operator::and_if) ? Connective::and_if : Connective::or_if;
            advance();
            skip_newlines();
            auto right = parse_pipeline();
            if (!right) {
                return right.error();
            }
            ir::Sequence sequence;
            sequence.left = left.value();
            sequence.right = right.value();
            sequence.connective = connective;
            left = ir::node(std::move(sequence), "sequence");
        }
    }

    Result<ir::NodePtr> parse_pipeline() {
        bool negate = false;
        if (at_reserved("!") && peek(1).kind == TokenKind::word) {
            advance();
            negate = true;
        }
        auto first = parse_command();
        if (!first) {
            return first.error();
        }
        if (negate) {
            ir::Negate inversion;
            inversion.subject = first.value();
            return ir::node(std::move(inversion), "negate");
        }

        const auto* head = std::get_if<ir::Command>(&first.value()->value);
        if (head == nullptr) {
            // A compound command is not a pipeline stage in POSIX; `a | { b; }`
            // is invalid, and reporting it here is clearer than silently
            // dropping the pipe.
            if (at_operator(Operator::pipe)) {
                return failure(ErrorCode::syntax_error, "a pipeline stage must be a simple command");
            }
            return first;
        }

        std::vector<ir::Command> commands {*head};
        while (match_operator(Operator::pipe)) {
            match_newlines();
            auto next = parse_command();
            if (!next) {
                return next.error();
            }
            const auto* command = std::get_if<ir::Command>(&next.value()->value);
            if (command == nullptr) {
                return failure(ErrorCode::syntax_error, "a pipeline stage must be a simple command");
            }
            commands.push_back(*command);
        }
        if (commands.size() == 1) {
            return ir::command(commands.front(), "command");
        }
        ir::Pipeline pipeline;
        pipeline.commands = std::move(commands);
        return ir::node(std::move(pipeline), "pipeline");
    }

    Result<ir::NodePtr> parse_command() {
        if (at_reserved("{")) {
            return parse_brace_group();
        }
        if (at_reserved("if")) {
            return parse_if();
        }
        if (at_reserved("while")) {
            return parse_while(/*until=*/false);
        }
        if (at_reserved("until")) {
            return parse_while(/*until=*/true);
        }
        if (at_reserved("for")) {
            return parse_for();
        }
        if (at_reserved("case")) {
            return parse_case();
        }
        if (at_reserved("function")) {
            return parse_function_keyword();
        }
        if (at_reserved("}")) {
            return failure(ErrorCode::syntax_error, "'}' without a matching '{'");
        }
        if (at_operator(Operator::subshell_open)) {
            return parse_subshell();
        }
        // `name()` with no intervening space is a function definition: the
        // parentheses follow the name directly.
        if (peek().kind == TokenKind::word && !peek().quoted && !peek().reserved) {
            const Token& open = peek(1);
            const Token& close = peek(2);
            if (open.kind == TokenKind::operator_token && open.op == Operator::subshell_open && close.kind == TokenKind::operator_token
                && close.op == Operator::subshell_close) {
                return parse_function_paren();
            }
        }
        return parse_simple_command();
    }

    Result<ir::NodePtr> parse_brace_group() {
        advance(); // '{'
        auto body = parse_list();
        if (!body) {
            return body.error();
        }
        skip_newlines();
        if (!match_reserved("}")) {
            return failure(ErrorCode::syntax_error, "expected '}' to close a brace group");
        }
        ir::BraceGroup group;
        group.body = body.value();
        auto redirections = parse_redirections();
        if (!redirections) {
            return redirections.error();
        }
        group.redirections = std::move(redirections).value();
        return ir::node(std::move(group), "brace-group");
    }

    Result<ir::NodePtr> parse_subshell() {
        advance(); // '('
        auto body = parse_list();
        if (!body) {
            return body.error();
        }
        skip_newlines();
        if (!match_operator(Operator::subshell_close)) {
            return failure(ErrorCode::syntax_error, "expected ')' to close a subshell");
        }
        ir::Subshell subshell;
        subshell.body = body.value();
        auto redirections = parse_redirections();
        if (!redirections) {
            return redirections.error();
        }
        subshell.redirections = std::move(redirections).value();
        return ir::node(std::move(subshell), "subshell");
    }

    Result<ir::NodePtr> parse_if() {
        advance(); // 'if'
        auto condition = parse_list();
        if (!condition) {
            return condition.error();
        }
        skip_newlines();
        if (!match_reserved("then")) {
            return failure(ErrorCode::syntax_error, "expected 'then'");
        }
        auto body = parse_list();
        if (!body) {
            return body.error();
        }

        // The chain nests to the right. `else` binds to the *innermost* clause,
        // so `if a; then ..; elif b; then ..; else ..; fi` puts the else under
        // the elif instead of replacing the whole chain.
        std::vector<ir::IfClause> clauses;
        ir::IfClause root;
        root.condition = condition.value();
        root.body = body.value();
        clauses.push_back(std::move(root));

        for (;;) {
            const std::size_t saved = current_;
            skip_newlines();
            if (match_reserved("elif")) {
                auto nested = parse_list();
                if (!nested) {
                    return nested.error();
                }
                skip_newlines();
                if (!match_reserved("then")) {
                    return failure(ErrorCode::syntax_error, "expected 'then' after 'elif'");
                }
                auto nested_body = parse_list();
                if (!nested_body) {
                    return nested_body.error();
                }
                ir::IfClause clause;
                clause.condition = nested.value();
                clause.body = nested_body.value();
                clauses.push_back(std::move(clause));
                continue;
            }
            if (match_reserved("else")) {
                auto else_body = parse_list();
                if (!else_body) {
                    return else_body.error();
                }
                clauses.back().alternative = else_body.value();
                continue;
            }
            if (match_reserved("fi")) {
                break;
            }
            current_ = saved;
            return failure(ErrorCode::syntax_error, "expected 'fi' to close an if clause");
        }

        // Fold the chain from the inside out so the first condition is the root.
        std::optional<ir::NodePtr> alternative;
        for (std::size_t index = clauses.size(); index > 0; --index) {
            ir::IfClause clause = std::move(clauses[index - 1]);
            clause.alternative = alternative;
            alternative = ir::node(std::move(clause), "if");
        }
        return *alternative;
    }

    Result<ir::NodePtr> parse_while(bool until) {
        advance(); // 'while' / 'until'
        auto condition = parse_list();
        if (!condition) {
            return condition.error();
        }
        match_newlines();
        if (!match_reserved("do")) {
            return failure(ErrorCode::syntax_error, "expected 'do'");
        }
        auto body = parse_list();
        if (!body) {
            return body.error();
        }
        match_newlines();
        if (!match_reserved("done")) {
            return failure(ErrorCode::syntax_error, "expected 'done'");
        }
        ir::WhileClause clause;
        clause.condition = condition.value();
        clause.body = body.value();
        clause.until = until;
        auto redirections = parse_redirections();
        if (!redirections) {
            return redirections.error();
        }
        clause.redirections = std::move(redirections).value();
        return ir::node(std::move(clause), until ? "until" : "while");
    }

    Result<ir::NodePtr> parse_for() {
        advance(); // 'for'
        if (peek().kind != TokenKind::word || peek().reserved) {
            return failure(ErrorCode::syntax_error, "expected a loop variable after 'for'");
        }
        ir::ForClause clause;
        clause.variable = advance().text;

        // `for x in a b c` and `for x do ... done` (which iterates "$@").
        const std::size_t saved = current_;
        skip_newlines();
        if (match_reserved("in")) {
            skip_newlines();
            while (peek().kind == TokenKind::word && !peek().reserved) {
                clause.words.push_back(Argument {advance().fragments});
            }
        } else {
            current_ = saved;
        }
        skip_newlines();
        match_operator(Operator::semicolon);
        skip_newlines();
        if (!match_reserved("do")) {
            return failure(ErrorCode::syntax_error, "expected 'do'");
        }
        auto body = parse_list();
        if (!body) {
            return body.error();
        }
        match_newlines();
        if (!match_reserved("done")) {
            return failure(ErrorCode::syntax_error, "expected 'done'");
        }
        clause.body = body.value();
        auto redirections = parse_redirections();
        if (!redirections) {
            return redirections.error();
        }
        clause.redirections = std::move(redirections).value();
        return ir::node(std::move(clause), "for");
    }

    Result<ir::NodePtr> parse_case() {
        advance(); // 'case'
        if (peek().kind != TokenKind::word) {
            return failure(ErrorCode::syntax_error, "expected a word after 'case'");
        }
        ir::CaseClause clause;
        clause.subject = Argument {advance().fragments};
        skip_newlines();
        if (!match_reserved("in")) {
            return failure(ErrorCode::syntax_error, "expected 'in' after the case subject");
        }
        skip_newlines();

        std::vector<ir::CaseItem> items;
        std::optional<ir::CaseItem> default_item;
        for (;;) {
            skip_newlines();
            if (match_reserved("esac")) {
                break;
            }
            if (at_end()) {
                return failure(ErrorCode::syntax_error, "unterminated case clause");
            }

            ir::CaseItem item;
            if (at_operator(Operator::subshell_open)) {
                advance();
            }
            for (;;) {
                if (peek().kind != TokenKind::word) {
                    return failure(ErrorCode::syntax_error, "expected a case pattern");
                }
                item.patterns.push_back(Argument {advance().fragments});
                if (match_operator(Operator::pipe)) {
                    continue;
                }
                break;
            }
            if (!match_operator(Operator::subshell_close)) {
                return failure(ErrorCode::syntax_error, "expected ')' after a case pattern");
            }
            skip_newlines();
            auto body = parse_list();
            if (!body) {
                return body.error();
            }
            item.body = body.value();

            const bool fallthrough = match_operator(Operator::semicolon_and);
            if (!fallthrough && !match_operator(Operator::double_semicolon)) {
                match_operator(Operator::semicolon);
            }
            item.fallthrough = fallthrough;

            if (item.patterns.empty()) {
                // `*)` — the default branch, tried only when nothing else matched.
                default_item = std::move(item);
            } else {
                items.push_back(std::move(item));
            }
        }

        if (default_item) {
            items.push_back(std::move(*default_item));
        }
        clause.items = std::move(items);
        return ir::node(std::move(clause), "case");
    }

    Result<ir::NodePtr> parse_function_keyword() {
        advance(); // 'function'
        if (peek().kind != TokenKind::word) {
            return failure(ErrorCode::syntax_error, "expected a function name");
        }
        const std::string name = advance().text;
        // Both `function f ()` and `function f` are accepted; the parentheses are
        // optional in the keyword form.
        if (at_operator(Operator::subshell_open) && peek(1).kind == TokenKind::operator_token
            && peek(1).op == Operator::subshell_close) {
            advance();
            advance();
        }
        skip_newlines();
        auto body = parse_command();
        if (!body) {
            return body.error();
        }
        ir::FunctionDefinition definition;
        definition.name = name;
        definition.body = body.value();
        return ir::node(std::move(definition), "function");
    }

    Result<ir::NodePtr> parse_function_paren() {
        const std::string name = advance().text;
        advance(); // '('
        advance(); // ')'
        skip_newlines();
        auto body = parse_command();
        if (!body) {
            return body.error();
        }
        ir::FunctionDefinition definition;
        definition.name = name;
        definition.body = body.value();
        return ir::node(std::move(definition), "function");
    }

    Result<ir::NodePtr> parse_simple_command() {
        // `break`, `continue`, and `return` are control constructs, not commands.
        if (peek().kind == TokenKind::word && !peek().quoted && !peek().reserved) {
            const std::string& name = peek().text;
            if (name == "break" || name == "continue" || name == "return") {
                return parse_control(name);
            }
        }

        ir::Command command;
        bool saw_word = false;
        for (;;) {
            if (!saw_word && peek().kind == TokenKind::word && is_assignment_token(peek())) {
                command.assignments.push_back(assignment_of(advance()));
                continue;
            }
            if (peek().kind == TokenKind::io_number || is_redirection(peek())) {
                auto redirections = parse_redirections();
                if (!redirections) {
                    return redirections.error();
                }
                for (Redirection& redirection : redirections.value()) {
                    command.redirections.push_back(std::move(redirection));
                }
                continue;
            }
            if (peek().kind != TokenKind::word) {
                break;
            }
            if (peek().reserved) {
                if (!saw_word) {
                    return failure(ErrorCode::syntax_error, "unexpected reserved word", peek().text);
                }
                // A reserved word is a keyword only where a *command* is
                // expected. After the command name it is an ordinary argument,
                // so `echo done` prints "done".
                command.argv.push_back(Argument {advance().fragments});
                continue;
            }
            command.argv.push_back(Argument {advance().fragments});
            saw_word = true;
        }

        if (!saw_word && command.assignments.empty() && command.redirections.empty()) {
            return failure(ErrorCode::empty_argv, "expected a command");
        }
        return ir::command(std::move(command), "command");
    }

    Result<ir::NodePtr> parse_control(const std::string& name) {
        advance(); // the control word
        int code = 0;
        if (peek().kind == TokenKind::word && is_digits(literal_of(peek()))) {
            const std::string digits = literal_of(advance());
            code = std::stoi(digits);
        } else if (peek().kind == TokenKind::word && !peek().quoted) {
            return failure(ErrorCode::syntax_error, "expected a numeric argument to '" + name + "'");
        }
        ir::Control control;
        if (name == "return") {
            control.signal = ControlSignal::return_;
            control.code = code;
        } else if (name == "break") {
            control.signal = ControlSignal::break_;
        } else {
            control.signal = ControlSignal::continue_;
        }
        return ir::node(std::move(control), name);
    }

    // ---- redirections -----------------------------------------------------

    [[nodiscard]] static bool is_redirection(const Token& token) {
        if (token.kind == TokenKind::io_number) {
            return true;
        }
        if (token.kind != TokenKind::operator_token) {
            return false;
        }
        switch (token.op) {
        case Operator::redirect_in:
        case Operator::redirect_out:
        case Operator::redirect_append:
        case Operator::redirect_clobber:
        case Operator::redirect_read_write:
        case Operator::redirect_dup_in:
        case Operator::redirect_dup_out:
        case Operator::redirect_heredoc:
        case Operator::redirect_heredoc_strip:
        case Operator::redirect_herestring:
        case Operator::redirect_amp_out:
        case Operator::redirect_amp_append:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] static bool is_input_operator(Operator op) {
        return op == Operator::redirect_in || op == Operator::redirect_dup_in;
    }

    Result<std::vector<Redirection>> parse_redirections() {
        std::vector<Redirection> redirections;
        for (;;) {
            std::optional<int> source_fd;
            if (peek().kind == TokenKind::io_number) {
                const std::string digits = advance().text;
                if (!is_digits(digits)) {
                    return failure(ErrorCode::syntax_error, "an IO number must be decimal digits");
                }
                source_fd = std::stoi(digits);
            }
            if (peek().kind != TokenKind::operator_token || !is_redirection(peek())) {
                if (source_fd) {
                    return failure(ErrorCode::syntax_error, "an IO number must precede a redirection operator");
                }
                return redirections;
            }
            const Operator op = advance().op;
            const RedirectStream default_stream = is_input_operator(op) ? RedirectStream::stdin_stream : RedirectStream::stdout_stream;
            const RedirectStream stream = source_fd && *source_fd == 2 ? RedirectStream::stderr_stream : default_stream;

            switch (op) {
            case Operator::redirect_dup_out:
            case Operator::redirect_dup_in: {
                // The operand may arrive as an IO_NUMBER when another
                // redirection follows it, so both spellings are accepted.
                if (peek().kind != TokenKind::word && peek().kind != TokenKind::io_number) {
                    return failure(ErrorCode::invalid_redirection, "descriptor duplication requires a target");
                }
                const std::string target = peek().kind == TokenKind::io_number ? advance().text : literal_of(advance());
                Redirection redirection;
                redirection.stream = stream;
                if (source_fd) {
                    redirection.source_fd = source_fd;
                }
                if (target == "-") {
                    redirection.mode = RedirectMode::close;
                    redirection.target.kind = StdioTargetKind::closed;
                    redirections.push_back(std::move(redirection));
                    break;
                }
                if (!is_digits(target)) {
                    return failure(
                        ErrorCode::invalid_redirection,
                        "descriptor duplication target must be a number or '-'");
                }
                redirection.mode = RedirectMode::duplicate;
                redirection.target.kind = StdioTargetKind::fd;
                redirection.target.fd = std::stoi(target);
                redirections.push_back(std::move(redirection));
                break;
            }
            case Operator::redirect_heredoc:
            case Operator::redirect_heredoc_strip: {
                if (peek().kind != TokenKind::word) {
                    return failure(ErrorCode::invalid_redirection, "a here-document requires a delimiter");
                }
                // The lexer resolved the body while scanning past the newline that
                // ended the command line.
                const Token delimiter = advance();
                Redirection redirection;
                redirection.stream = RedirectStream::stdin_stream;
                redirection.mode = RedirectMode::read;
                redirection.target.kind = StdioTargetKind::memory;
                redirection.target.input = std::make_shared<MemoryReader>(delimiter.heredoc);
                redirections.push_back(std::move(redirection));
                break;
            }
            case Operator::redirect_herestring: {
                if (peek().kind != TokenKind::word) {
                    return failure(ErrorCode::invalid_redirection, "a here-string requires a word");
                }
                Argument word {advance().fragments};
                redirections.push_back(from_word(std::move(word)));
                break;
            }
            case Operator::redirect_amp_out:
            case Operator::redirect_amp_append: {
                if (peek().kind != TokenKind::word) {
                    return failure(ErrorCode::invalid_redirection, "this redirection requires a path");
                }
                Argument path {advance().fragments};
                // `&>f` is `>f 2>&1` and `&>>f` is `>>f 2>&1`. The path stays an
                // Argument so tilde and parameter expansion still apply.
                Redirection primary;
                primary.stream = RedirectStream::stdout_stream;
                primary.mode = op == Operator::redirect_amp_out ? RedirectMode::truncate : RedirectMode::append;
                primary.target.kind = StdioTargetKind::deferred;
                primary.target.deferred = std::move(path);
                redirections.push_back(std::move(primary));
                redirections.push_back(to_fd(RedirectStream::stderr_stream, 1));
                break;
            }
            default: {
                if (peek().kind != TokenKind::word) {
                    return failure(ErrorCode::invalid_redirection, "a redirection requires a path");
                }
                Argument path {advance().fragments};
                Redirection redirection;
                redirection.stream = stream;
                if (source_fd) {
                    redirection.source_fd = source_fd;
                }
                switch (op) {
                case Operator::redirect_in:
                    redirection.mode = RedirectMode::read;
                    break;
                case Operator::redirect_out:
                    redirection.mode = RedirectMode::truncate;
                    break;
                case Operator::redirect_append:
                    redirection.mode = RedirectMode::append;
                    break;
                case Operator::redirect_clobber:
                    redirection.mode = RedirectMode::clobber;
                    break;
                case Operator::redirect_read_write:
                    redirection.mode = RedirectMode::read_write;
                    break;
                default:
                    return failure(ErrorCode::invalid_redirection, "unsupported redirection");
                }
                redirection.target.kind = StdioTargetKind::deferred;
                redirection.target.deferred = std::move(path);
                redirections.push_back(std::move(redirection));
                break;
            }
            }
        }
    }

    TokenList tokens_;
    std::size_t current_ {0};
};

} // namespace

Result<ir::Program> parse_script(std::string_view text) {
    Lexer lexer {text};
    if (lexer.error()) {
        return lexer.error().value();
    }
    Parser parser {lexer.tokens()};
    return parser.parse_program();
}

Result<ir::Program> parse_line(std::string_view line) {
    // A single line is a one-line script. The lexer still needs the whole text so
    // that here-documents and multi-line quotes behave identically to a script.
    return parse_script(line);
}

} // namespace lsh::cli
