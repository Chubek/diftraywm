#include "LibShell.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lsh::cli {

namespace {

enum class TokenKind {
    word,
    pipe,
    and_if,
    or_if,
    sequence,
    background,
    redirect_in,
    redirect_out,
    redirect_append,
    redirect_err,
    redirect_err_append,
};

struct Token {
    TokenKind kind {TokenKind::word};
    std::vector<Expansion> fragments; // populated for word tokens
    std::string text;                 // operator label or fallback text
};

Token token(TokenKind kind) {
    Token result;
    result.kind = kind;
    return result;
}

Token word_token(std::vector<Expansion> fragments) {
    Token result;
    result.kind = TokenKind::word;
    result.fragments = std::move(fragments);
    return result;
}

// Concatenate the literal text of a word's fragments. Used where a structured
// target (e.g. a redirection path) must collapse to a single path string;
// expansion is the runtime's job, not the tokenizer's.
std::string word_text(const std::vector<Expansion>& fragments) {
    std::string out;
    for (const Expansion& fragment : fragments) {
        out += fragment.text;
    }
    return out;
}

bool is_name_start(char ch) { return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_'; }
bool is_name_char(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

// A word is built as an ordered list of typed Expansion fragments so the
// runtime expander receives the same structure the DSL produces, instead of a
// collapsed raw string. Literal runs are tagged by the quoting context that
// produced them (raw / single_quoted / double_quoted).
struct WordBuilder {
    std::vector<Expansion> fragments;
    std::string pending;
    ExpansionKind lit_kind {ExpansionKind::raw};
    bool has_pending {false};

    void set_kind(ExpansionKind kind) {
        if (has_pending && lit_kind != kind) {
            flush();
        }
        lit_kind = kind;
    }

    void push_char(char ch) {
        pending.push_back(ch);
        has_pending = true;
    }

    void flush() {
        if (has_pending) {
            fragments.push_back(Expansion {.kind = lit_kind, .text = std::move(pending), .field_splitting = false});
            pending.clear();
            has_pending = false;
        }
    }

    void push_expansion(ExpansionKind kind, std::string text, bool field_splitting) {
        flush();
        fragments.push_back(Expansion {.kind = kind, .text = std::move(text), .field_splitting = field_splitting});
    }

    std::vector<Expansion> finish() {
        flush();
        return std::move(fragments);
    }
};

// Scan a command/arithmetic substitution body starting at `start` (the char
// after the opening parenthesis(s)). Respects quotes and nested parentheses.
// `closer` is ")" for $(...) and "))" for $((...)). Returns the body text and
// advances `index` past the closer.
Result<std::string> scan_substitution(std::string_view line, std::size_t start, std::string_view closer, std::size_t& end) {
    std::string body;
    bool single_quote = false;
    bool double_quote = false;
    bool escaped = false;
    int depth = 1; // we are inside one unclosed '('
    std::size_t index = start;
    for (; index < line.size(); ++index) {
        char ch = line[index];
        if (escaped) {
            body.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\' && !single_quote) {
            escaped = true;
            body.push_back(ch);
            continue;
        }
        if (ch == '\'' && !double_quote) {
            single_quote = !single_quote;
            body.push_back(ch);
            continue;
        }
        if (ch == '"' && !single_quote) {
            double_quote = !double_quote;
            body.push_back(ch);
            continue;
        }
        if (single_quote || double_quote) {
            body.push_back(ch);
            continue;
        }
        if (ch == '(') {
            ++depth;
            body.push_back(ch);
            continue;
        }
        if (ch == ')') {
            --depth;
            if (depth == 0) {
                // Found the closing ')'. For arithmetic, expect a second ')'.
                std::size_t consumed = 1;
                if (closer.size() == 2) {
                    if (index + 1 >= line.size() || line[index + 1] != ')') {
                        return Diagnostic {ErrorCode::bad_expansion, "unterminated arithmetic expansion", {}};
                    }
                    consumed = 2;
                }
                end = index + consumed;
                return body;
            }
            body.push_back(ch);
            continue;
        }
        body.push_back(ch);
    }
    return Diagnostic {ErrorCode::bad_expansion, "unterminated command substitution", {}};
}

// Parse a `$...` expansion beginning at line[index] == '$', advancing `index`
// past the expansion and appending a typed fragment to `word`. `field_splitting`
// is true for unquoted expansions (POSIX field splitting applies) and false
// inside double quotes.
Result<void> parse_expansion(std::string_view line, std::size_t& index, WordBuilder& word, bool field_splitting) {
    std::size_t pos = index + 1;
    if (pos >= line.size()) {
        word.set_kind(field_splitting ? ExpansionKind::raw : ExpansionKind::double_quoted);
        word.push_char('$');
        index = pos;
        return {};
    }
    const char next = line[pos];
    if (next == '(') {
        if (pos + 1 < line.size() && line[pos + 1] == '(') {
            std::size_t end = 0;
            auto body = scan_substitution(line, pos + 2, "))", end);
            if (!body) {
                return body.error();
            }
            word.push_expansion(ExpansionKind::arithmetic, std::move(body).value(), false);
            index = end;
            return {};
        }
        std::size_t end = 0;
        auto body = scan_substitution(line, pos + 1, ")", end);
        if (!body) {
            return body.error();
        }
        word.push_expansion(ExpansionKind::command, std::move(body).value(), field_splitting);
        index = end;
        return {};
    }
    if (next == '{') {
        std::size_t name_start = pos + 1;
        std::size_t name_end = name_start;
        while (name_end < line.size() && line[name_end] != '}') {
            ++name_end;
        }
        if (name_end >= line.size()) {
            return Diagnostic {ErrorCode::bad_expansion, "unterminated ${...} expansion", {}};
        }
        std::string name(line.substr(name_start, name_end - name_start));
        word.push_expansion(ExpansionKind::variable, std::move(name), field_splitting);
        index = name_end + 1;
        return {};
    }
    if (is_name_start(next)) {
        // pos indexes the first name character (the char after '$'); scan the
        // rest of the identifier and capture the full name [pos, name_end).
        std::size_t name_end = pos + 1;
        while (name_end < line.size() && is_name_char(line[name_end])) {
            ++name_end;
        }
        std::string name(line.substr(pos, name_end - pos));
        word.push_expansion(ExpansionKind::variable, std::move(name), field_splitting);
        index = name_end;
        return {};
    }
    // Bare '$' — treat as a literal.
    word.set_kind(field_splitting ? ExpansionKind::raw : ExpansionKind::double_quoted);
    word.push_char('$');
    index = pos;
    return {};
}

Result<std::vector<Token>> tokenize(std::string_view line) {
    std::vector<Token> tokens;
    WordBuilder word;
    bool in_word = false;
    bool single_quote = false;
    bool double_quote = false;
    bool escaped = false;

    auto flush_word = [&] {
        if (in_word) {
            tokens.push_back(word_token(word.finish()));
            in_word = false;
        }
    };

    auto start_word = [&] {
        if (!in_word) {
            in_word = true;
        }
    };

    std::size_t index = 0;
    while (index < line.size()) {
        const char ch = line[index];
        if (escaped) {
            word.set_kind(double_quote ? ExpansionKind::double_quoted : ExpansionKind::raw);
            word.push_char(ch);
            start_word();
            escaped = false;
            ++index;
            continue;
        }
        if (ch == '\\' && !single_quote) {
            if (double_quote) {
                // POSIX: inside "...", '\' escapes only $ ` " and \; before any
                // other character the backslash is preserved literally (so
                // printf "%s\n" retains \n for the program to interpret).
                if (index + 1 < line.size()) {
                    const char escaped_ch = line[index + 1];
                    if (escaped_ch == '$' || escaped_ch == '`'
                        || escaped_ch == '"' || escaped_ch == '\\') {
                        word.set_kind(ExpansionKind::double_quoted);
                        word.push_char(escaped_ch);
                        start_word();
                        index += 2;
                        continue;
                    }
                }
                word.set_kind(ExpansionKind::double_quoted);
                word.push_char('\\');
                start_word();
                ++index;
                continue;
            }
            escaped = true;
            ++index;
            continue;
        }
        if (ch == '\'' && !double_quote) {
            single_quote = !single_quote;
            word.set_kind(ExpansionKind::single_quoted);
            start_word();
            ++index;
            continue;
        }
        if (ch == '"' && !single_quote) {
            double_quote = !double_quote;
            word.set_kind(ExpansionKind::double_quoted);
            start_word();
            ++index;
            continue;
        }
        if (single_quote) {
            word.push_char(ch);
            ++index;
            continue;
        }
        if (double_quote) {
            if (ch == '$') {
                // parse_expansion_into handles $... inside double quotes (no split).
                auto parsed = parse_expansion(line, index, word, /*field_splitting=*/false);
                if (!parsed) {
                    return parsed.error();
                }
                start_word();
                continue;
            }
            word.push_char(ch);
            ++index;
            continue;
        }

        // Unquoted context.
        if (std::isspace(static_cast<unsigned char>(ch))) {
            flush_word();
            ++index;
            continue;
        }
        if (ch == '#' && !in_word) {
            break;
        }
        if (ch == '$') {
            auto parsed = parse_expansion(line, index, word, /*field_splitting=*/true);
            if (!parsed) {
                return parsed.error();
            }
            start_word();
            continue;
        }
        if (ch == '&' && index + 1 < line.size() && line[index + 1] == '&') {
            flush_word();
            tokens.push_back(token(TokenKind::and_if));
            index += 2;
            continue;
        }
        if (ch == '|' && index + 1 < line.size() && line[index + 1] == '|') {
            flush_word();
            tokens.push_back(token(TokenKind::or_if));
            index += 2;
            continue;
        }
        if (ch == '|') {
            flush_word();
            tokens.push_back(token(TokenKind::pipe));
            ++index;
            continue;
        }
        if (ch == '&') {
            flush_word();
            tokens.push_back(token(TokenKind::background));
            ++index;
            continue;
        }
        if (ch == ';') {
            flush_word();
            tokens.push_back(token(TokenKind::sequence));
            ++index;
            continue;
        }
        if (ch == '<') {
            flush_word();
            tokens.push_back(token(TokenKind::redirect_in));
            ++index;
            continue;
        }
        // '2>' is a stderr redirection only at a word boundary, so the literal
        // word "a2>file" is not mis-split.
        if (ch == '2' && !in_word && index + 1 < line.size() && line[index + 1] == '>') {
            flush_word();
            if (index + 2 < line.size() && line[index + 2] == '>') {
                tokens.push_back(token(TokenKind::redirect_err_append));
                index += 3;
            } else {
                tokens.push_back(token(TokenKind::redirect_err));
                index += 2;
            }
            continue;
        }
        if (ch == '>') {
            flush_word();
            if (index + 1 < line.size() && line[index + 1] == '>') {
                tokens.push_back(token(TokenKind::redirect_append));
                index += 2;
            } else {
                tokens.push_back(token(TokenKind::redirect_out));
                ++index;
            }
            continue;
        }

        word.set_kind(ExpansionKind::raw);
        word.push_char(ch);
        start_word();
        ++index;
    }
    if (escaped || single_quote || double_quote) {
        return Diagnostic {ErrorCode::bad_expansion, "unterminated quote or escape", {}};
    }
    flush_word();
    return tokens;
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Result<ir::Program> parse_program() {
        auto expr = parse_sequence();
        if (!expr) {
            return expr.error();
        }
        if (!at_end()) {
            return Diagnostic {ErrorCode::invalid_graph, "unexpected token after command", peek().text};
        }
        return ir::Program {expr.value()};
    }

private:
    Result<ir::NodePtr> parse_sequence() {
        auto left = parse_pipeline();
        if (!left) {
            return left.error();
        }
        while (match(TokenKind::sequence) || match(TokenKind::and_if) || match(TokenKind::or_if) || match(TokenKind::background)) {
            const TokenKind connective_token = previous().kind;
            if (connective_token == TokenKind::background) {
                // Background is acknowledged but treated as a sequence here;
                // true job control is a runtime/executor concern.
                continue;
            }
            auto right = parse_pipeline();
            if (!right) {
                return right.error();
            }
            Connective connective = Connective::sequence;
            if (connective_token == TokenKind::and_if) {
                connective = Connective::and_if;
            } else if (connective_token == TokenKind::or_if) {
                connective = Connective::or_if;
            }
            left = ir::node(ir::Sequence {.left = left.value(), .right = right.value(), .connective = connective}, "cli-sequence");
        }
        return left;
    }

    Result<ir::NodePtr> parse_pipeline() {
        std::vector<ir::Command> commands;
        auto command = parse_command();
        if (!command) {
            return command.error();
        }
        commands.push_back(std::move(command).value());
        while (match(TokenKind::pipe)) {
            auto next = parse_command();
            if (!next) {
                return next.error();
            }
            commands.push_back(std::move(next).value());
        }
        if (commands.size() == 1) {
            return ir::command(std::move(commands.front()), "cli-command");
        }
        return ir::node(ir::Pipeline {.commands = std::move(commands)}, "cli-pipeline");
    }

    Result<ir::Command> parse_command() {
        ir::Command command;
        while (!at_end()) {
            if (peek().kind == TokenKind::word) {
                if (peek().fragments.empty()) {
                    return Diagnostic {ErrorCode::empty_argv, "empty word", {}};
                }
                Argument argument;
                argument.fragments = advance().fragments;
                command.argv.push_back(std::move(argument));
                continue;
            }
            if (is_redirect(peek().kind)) {
                const TokenKind kind = advance().kind;
                if (at_end() || peek().kind != TokenKind::word) {
                    return Diagnostic {ErrorCode::invalid_redirection, "redirection requires a path", {}};
                }
                const std::string path = word_text(advance().fragments);
                command.redirections.push_back(make_redirection(kind, path));
                continue;
            }
            break;
        }
        if (command.argv.empty()) {
            return Diagnostic {ErrorCode::empty_argv, "expected command", {}};
        }
        return command;
    }

    static bool is_redirect(TokenKind kind) {
        return kind == TokenKind::redirect_in || kind == TokenKind::redirect_out || kind == TokenKind::redirect_append
            || kind == TokenKind::redirect_err || kind == TokenKind::redirect_err_append;
    }

    static Redirection make_redirection(TokenKind kind, std::string path) {
        switch (kind) {
        case TokenKind::redirect_in:
            return in(std::move(path));
        case TokenKind::redirect_append:
            return append(std::move(path));
        case TokenKind::redirect_err:
            return err(std::move(path));
        case TokenKind::redirect_err_append:
            return err_append(std::move(path));
        case TokenKind::redirect_out:
        default:
            return out(std::move(path));
        }
    }

    bool match(TokenKind kind) {
        if (at_end() || peek().kind != kind) {
            return false;
        }
        ++current_;
        return true;
    }

    const Token& advance() { return tokens_[current_++]; }
    [[nodiscard]] const Token& peek() const { return tokens_[current_]; }
    [[nodiscard]] const Token& previous() const { return tokens_[current_ - 1]; }
    [[nodiscard]] bool at_end() const noexcept { return current_ >= tokens_.size(); }

    std::vector<Token> tokens_;
    std::size_t current_ {0};
};

} // namespace

Result<ir::Program> parse_line(std::string_view line) {
    auto tokens = tokenize(line);
    if (!tokens) {
        return tokens.error();
    }
    if (tokens.value().empty()) {
        return Diagnostic {ErrorCode::empty_argv, "empty command line", {}};
    }
    return Parser {std::move(tokens).value()}.parse_program();
}

} // namespace lsh::cli
