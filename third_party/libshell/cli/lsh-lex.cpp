#include "lsh-lex.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace lsh::cli {

namespace {

// POSIX reserved words. A reserved word is recognized only when the token that
// spells it is unquoted and appears where a command is expected; the parser
// enforces the second condition.
const std::array<std::string_view, 17> k_reserved_words {
    "if", "then", "else", "elif", "fi", "do", "done", "case", "esac",
    "while", "until", "for", "{", "}", "!", "in", "function",
};

[[nodiscard]] bool is_name_start(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

[[nodiscard]] bool is_name_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

[[nodiscard]] bool is_io_number(std::string_view text) {
    if (text.empty() || text.size() > 2) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
}

} // namespace

Lexer::Lexer(std::string_view text) : text_(text) { run(); }

void Lexer::run() {
    lex_line();
    Token end;
    end.kind = TokenKind::end;
    end.offset = text_.size();
    tokens_.push_back(std::move(end));
}

void Lexer::push_operator(Operator op, std::size_t offset) {
    flush_literal();
    if (op == Operator::redirect_heredoc || op == Operator::redirect_heredoc_strip) {
        expect_heredoc_ = true;
        heredoc_strip_pending_ = op == Operator::redirect_heredoc_strip;
    }
    Token token;
    token.kind = TokenKind::operator_token;
    token.op = op;
    token.offset = offset;
    tokens_.push_back(std::move(token));
}

void Lexer::push_newline(std::size_t offset) {
    flush_literal();
    // A here-document body starts on the line after the command line that
    // declared it, so the newline is the sync point.
    read_heredoc_bodies();
    Token token;
    token.kind = TokenKind::newline;
    token.offset = offset;
    tokens_.push_back(std::move(token));
}

void Lexer::flush_literal() {
    // A pending tilde prefix belongs *before* the literal that follows it, so it
    // is emitted first; otherwise `~/x` would expand as `/x` + `~`.
    flush_tilde();
    if (literal_.empty()) {
        return;
    }
    Expansion fragment;
    fragment.kind = literal_kind_;
    fragment.text = std::move(literal_);
    // An unquoted literal is subject to pathname expansion but never to field
    // splitting: splitting applies to the result of an expansion, not to text.
    fragment.globbable = literal_kind_ == ExpansionKind::raw;
    literal_.clear();
    literal_active_ = false;
    fragments_.push_back(std::move(fragment));
}

void Lexer::flush_tilde() {
    if (!tilde_pending_) {
        return;
    }
    Expansion fragment = make_expansion(ExpansionKind::tilde, std::move(tilde_spec_));
    fragment.tilde_prefix = true;
    fragment.globbable = true;
    tilde_spec_.clear();
    tilde_pending_ = false;
    fragments_.push_back(std::move(fragment));
}

void Lexer::append_literal(char ch, bool /*word_start*/) {
    literal_.push_back(ch);
    literal_active_ = true;
}

// Reads the here-document bodies declared before the current newline, in
// declaration order, and attaches them to the corresponding delimiter tokens.
void Lexer::read_heredoc_bodies() {
    if (pending_heredocs_.empty()) {
        return;
    }
    std::vector<PendingHeredoc> pending;
    pending.swap(pending_heredocs_);

    for (const PendingHeredoc& heredoc : pending) {
        std::string body;
        for (;;) {
            if (position_ >= text_.size()) {
                break;
            }
            const std::size_t line_end = text_.find('\n', position_);
            std::string_view line = line_end == std::string_view::npos
                ? text_.substr(position_)
                : text_.substr(position_, line_end - position_);
            position_ = line_end == std::string_view::npos ? text_.size() : line_end + 1;

            std::string_view candidate = line;
            if (heredoc.strip) {
                // `<<-` strips leading tabs from both the body and the delimiter.
                std::size_t start = 0;
                while (start < candidate.size() && candidate[start] == '\t') {
                    ++start;
                }
                candidate = candidate.substr(start);
            }
            if (candidate == heredoc.delimiter) {
                break;
            }
            body.append(candidate);
            body.push_back('\n');
        }

        if (heredoc.token_index < tokens_.size()) {
            tokens_[heredoc.token_index].heredoc = std::move(body);
            tokens_[heredoc.token_index].heredoc_strip = heredoc.strip;
        }
    }
}

// Copies a `$(...)`, `$((...))`, or backquote construct starting at `start`,
// returning the index just past it. Quotes and nested substitutions inside it
// are skipped wholesale.
std::size_t Lexer::copy_substitution(std::size_t start) const {
    // `start` is the '$' of a "$(", "$((" or a backquote; step past the
    // introducer and return the index just past the closing bracket at this
    // level. Nested parentheses, quotes, and substitutions are skipped.
    std::size_t scan = start + 1;
    while (scan < text_.size() && (text_[scan] == '$' || text_[scan] == '`')) {
        ++scan;
    }
    if (scan < text_.size() && (text_[scan] == '(' || text_[scan] == '{')) {
        ++scan;
    }
    int depth = 0;
    while (scan < text_.size()) {
        const char ch = text_[scan];
        if (ch == '\\' && scan + 1 < text_.size()) {
            scan += 2;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            const auto close = text_.find(ch, scan + 1);
            scan = close == std::string_view::npos ? text_.size() : close + 1;
            continue;
        }
        if (ch == '$' && scan + 1 < text_.size() && text_[scan + 1] == '(') {
            scan = copy_substitution(scan);
            continue;
        }
        if (ch == '(' || ch == '{') {
            ++depth;
            ++scan;
            continue;
        }
        if (ch == ')' || ch == '}') {
            if (depth == 0) {
                return scan + 1;
            }
            --depth;
            ++scan;
            continue;
        }
        ++scan;
    }
    return text_.size();
}

Result<std::string> Lexer::scan_balanced(std::size_t& index, char open, char close, const char* what) {
    std::string body;
    // The opening bracket is already consumed by the caller, so one level is
    // already open when the scan begins.
    int depth = 1;
    std::size_t scan = index;
    bool single = false;
    bool doubleq = false;
    bool escaped = false;
    for (; scan < text_.size(); ++scan) {
        const char ch = text_[scan];
        if (escaped) {
            body.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\' && !single) {
            body.push_back(ch);
            escaped = true;
            continue;
        }
        if (ch == '\'' && !doubleq) {
            single = !single;
            body.push_back(ch);
            continue;
        }
        if (ch == '"' && !single) {
            doubleq = !doubleq;
            body.push_back(ch);
            continue;
        }
        if (ch == '`' && !single) {
            body.push_back(ch);
            // A backquote substitution may itself contain balanced parentheses,
            // so copy it wholesale.
            std::size_t inner = scan + 1;
            while (inner < text_.size()) {
                if (text_[inner] == '\\' && inner + 1 < text_.size()) {
                    body.push_back(text_[inner]);
                    body.push_back(text_[inner + 1]);
                    inner += 2;
                    continue;
                }
                body.push_back(text_[inner]);
                if (text_[inner] == '`') {
                    break;
                }
                ++inner;
            }
            scan = inner;
            continue;
        }
        if (single || doubleq) {
            body.push_back(ch);
            continue;
        }
        if (ch == '$' && scan + 1 < text_.size() && text_[scan + 1] == '(') {
            // A nested command substitution is copied wholesale. Counting its
            // parentheses against the enclosing construct would let an inner
            // ')' close a '${' that is still open.
            const std::size_t nested = copy_substitution(scan);
            body.append(text_.substr(scan, nested - scan));
            scan = nested - 1;
            continue;
        }
        if (ch == open) {
            ++depth;
            body.push_back(ch);
            continue;
        }
        if (ch == close) {
            --depth;
            if (depth == 0) {
                index = scan + 1;
                return body;
            }
            body.push_back(ch);
            continue;
        }
        body.push_back(ch);
    }
    (void)open;
    return failure(ErrorCode::bad_expansion, std::string("unterminated ") + what);
}

// Splits a `${...}` body into a name plus an operator or trim form. The operand
// and pattern are kept as raw text: the Expander expands them, which is the
// only layer with an ExpandContext.
bool Lexer::parse_parameter_body(const std::string& body, Expansion& fragment) {
    std::size_t cursor = 0;
    const bool length = !body.empty() && body.front() == '#';

    if (length) {
        ++cursor;
    }
    // A positional parameter: ${1}, ${#1}.
    if (cursor < body.size() && std::isdigit(static_cast<unsigned char>(body[cursor])) != 0) {
        while (cursor < body.size() && std::isdigit(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }
        const std::string digits = body.substr(length ? 1 : 0, cursor - (length ? 1 : 0));
        fragment.kind = ExpansionKind::positional;
        fragment.text = digits;
        if (cursor != body.size()) {
            error_ = failure(ErrorCode::bad_expansion, "unexpected text after a positional parameter", body);
            return false;
        }
        if (length) {
            // ${#1} is the length of $1; the Expander reads it through the
            // variable path, so it is re-tagged as a variable named "1".
            fragment.kind = ExpansionKind::variable;
            fragment.op = ParameterOp::length;
        }
        return true;
    }

    while (cursor < body.size()
           && (std::isalnum(static_cast<unsigned char>(body[cursor])) != 0 || body[cursor] == '_')) {
        ++cursor;
    }
    if (cursor == 0) {
        error_ = failure(ErrorCode::bad_expansion, "parameter expansion without a name", body);
        return false;
    }
    fragment.kind = ExpansionKind::variable;
    fragment.text = body.substr(length ? 1 : 0, cursor - (length ? 1 : 0));

    if (cursor == body.size()) {
        if (length) {
            fragment.op = ParameterOp::length;
        }
        return true;
    }

    const std::string rest = body.substr(cursor);
    // A trim operator is '#', '##', '%', or '%%' followed by a pattern. The
    // length form was already consumed above, so anything starting with '#' here
    // is a trim.
    if (!rest.empty() && (rest.front() == '#' || rest.front() == '%')) {
        const bool longest = rest.size() >= 2 && rest[1] == rest.front();
        const bool suffix = rest.front() == '%';
        fragment.trim = suffix ? (longest ? TrimOp::longest_suffix : TrimOp::suffix)
                               : (longest ? TrimOp::longest_prefix : TrimOp::prefix);
        fragment.has_pattern = true;
        fragment.pattern = rest.substr(longest ? 2 : 1);
        return true;
    }

    std::size_t op_cursor = 0;
    bool colon = false;
    if (rest[op_cursor] == ':') {
        colon = true;
        ++op_cursor;
    }
    if (op_cursor >= rest.size()) {
        error_ = failure(ErrorCode::bad_expansion, "parameter expansion operator without an operand", body);
        return false;
    }
    switch (rest[op_cursor]) {
    case '-':
        fragment.op = ParameterOp::alternate;
        break;
    case '=':
        fragment.op = ParameterOp::assign;
        break;
    case '?':
        fragment.op = ParameterOp::error;
        break;
    case '+':
        fragment.op = ParameterOp::plus;
        break;
    default:
        error_ = failure(ErrorCode::bad_expansion, "unsupported parameter expansion operator", body);
        return false;
    }
    fragment.colon = colon;
    fragment.has_operand = true;
    fragment.operand = rest.substr(op_cursor + 1);
    (void)length;
    return true;
}

void Lexer::lex_dollar() {
    const std::size_t start = position_;
    const std::size_t after = start + 1;
    if (after >= text_.size()) {
        append_literal('$', false);
        ++position_;
        return;
    }
    const char next = text_[after];

    // $(( ... )) -- arithmetic expansion.
    if (next == '(' && after + 1 < text_.size() && text_[after + 1] == '(') {
        std::size_t index = after + 2;
        auto body = scan_balanced(index, '(', ')', "arithmetic expansion");
        if (!body) {
            // Consume the introducer so a malformed construct cannot spin the
            // lexer; the diagnostic is reported once the token stream is built.
            error_ = body.error();
            append_literal('$', false);
            position_ = after;
            return;
        }
        {
            // scan_balanced stops at the first ')' at depth 0; for $(( )) that is
            // the first of the two closers, so a second ')' must follow.
            std::string text = std::move(body).value();
            if (index < text_.size() && text_[index] == ')') {
                ++index;
            } else {
                text = {};
            }
            flush_literal();
            flush_tilde();
            Expansion fragment = make_expansion(ExpansionKind::arithmetic, std::move(text));
            fragment.field_splitting = false; // POSIX: never split
            fragment.globbable = false;        // POSIX: never globbed
            fragments_.push_back(std::move(fragment));
            position_ = index;
            return;
        }
        append_literal('$', false);
        ++position_;
        return;
    }

    if (next == '(') {
        std::size_t index = after + 1;
        auto body = scan_balanced(index, '(', ')', "command substitution");
        if (!body) {
            error_ = body.error();
            append_literal('$', false);
            position_ = after;
            return;
        }
        flush_literal();
        flush_tilde();
        // A body of the form `lua:<code>` selects the embedded-Lua evaluator. The
        // prefix cannot name a real program, so the form is unambiguous.
        const std::string text = std::move(body).value();
        Expansion fragment;
        if (text.rfind("lua:", 0) == 0) {
            fragment = make_expansion(ExpansionKind::lua, text.substr(4));
        } else {
            fragment = make_expansion(ExpansionKind::command, text);
        }
        fragment.field_splitting = literal_kind_ != ExpansionKind::double_quoted;
        fragment.globbable = literal_kind_ != ExpansionKind::double_quoted;
        fragments_.push_back(std::move(fragment));
        position_ = index;
        return;
    }

    if (next == '{') {
        std::size_t index = after + 1;
        auto body = scan_balanced(index, '{', '}', "parameter expansion");
        if (!body) {
            error_ = body.error();
            append_literal('$', false);
            position_ = after;
            return;
        }
        flush_literal();
        flush_tilde();
        Expansion fragment = make_expansion(ExpansionKind::variable);
        if (!parse_parameter_body(body.value(), fragment)) {
            // The balanced construct has already been consumed. Advance past
            // it before returning so malformed `${...}` input cannot re-enter
            // this branch at the same '$' indefinitely.
            position_ = index;
            return;
        }
        fragment.field_splitting = literal_kind_ != ExpansionKind::double_quoted;
        fragment.globbable = literal_kind_ != ExpansionKind::double_quoted;
        fragments_.push_back(std::move(fragment));
        position_ = index;
        return;
    }

    if (next == '@' || next == '*' || next == '?' || next == '$' || next == '!' || next == '#' || next == '-') {
        flush_literal();
        flush_tilde();
        Expansion fragment = make_expansion(ExpansionKind::special, std::string(1, next));
        fragment.field_splitting = literal_kind_ != ExpansionKind::double_quoted;
        fragment.globbable = literal_kind_ != ExpansionKind::double_quoted;
        fragments_.push_back(std::move(fragment));
        position_ = after + 1;
        return;
    }

    if (std::isdigit(static_cast<unsigned char>(next)) != 0) {
        std::size_t end = after;
        while (end < text_.size() && std::isdigit(static_cast<unsigned char>(text_[end])) != 0) {
            ++end;
        }
        flush_literal();
        flush_tilde();
        Expansion fragment = make_expansion(ExpansionKind::positional, std::string(text_.substr(after, end - after)));
        fragment.field_splitting = literal_kind_ != ExpansionKind::double_quoted;
        fragment.globbable = literal_kind_ != ExpansionKind::double_quoted;
        fragments_.push_back(std::move(fragment));
        position_ = end;
        return;
    }

    if (is_name_start(next)) {
        std::size_t end = after + 1;
        while (end < text_.size() && is_name_char(text_[end])) {
            ++end;
        }
        flush_literal();
        flush_tilde();
        Expansion fragment = make_expansion(ExpansionKind::variable, std::string(text_.substr(after, end - after)));
        fragment.field_splitting = literal_kind_ != ExpansionKind::double_quoted;
        fragment.globbable = literal_kind_ != ExpansionKind::double_quoted;
        fragments_.push_back(std::move(fragment));
        position_ = end;
        return;
    }

    // A '$' with nothing expandable after it is literal.
    append_literal('$', false);
    ++position_;
}

void Lexer::lex_backquote(bool /*quoted*/) {
    std::string body;
    ++position_; // consume `
    bool escaped = false;
    for (; position_ < text_.size(); ++position_) {
        const char ch = text_[position_];
        if (escaped) {
            // Inside backquotes a backslash escapes only \, $, and `.
            if (ch != '\\' && ch != '$' && ch != '`') {
                body.push_back('\\');
            }
            body.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '`') {
            ++position_;
            flush_literal();
            flush_tilde();
            // A body of the form `lua:<code>` selects the embedded evaluator; the
            // prefix cannot name a real program, so the form is unambiguous.
            Expansion fragment;
            if (body.rfind("lua:", 0) == 0) {
                fragment = make_expansion(ExpansionKind::lua, body.substr(4));
            } else {
                fragment = make_expansion(ExpansionKind::command, std::move(body));
            }
            fragment.field_splitting = literal_kind_ != ExpansionKind::double_quoted;
            fragment.globbable = literal_kind_ != ExpansionKind::double_quoted;
            fragments_.push_back(std::move(fragment));
            return;
        }
        body.push_back(ch);
    }
    flush_literal();
}

void Lexer::lex_single_quote() {
    ++position_; // consume '
    const std::size_t close = text_.find('\'', position_);
    if (close == std::string_view::npos) {
        // Unterminated quote: keep the text so the parse still produces a word,
        // and record the diagnostic so the caller can reject the input.
        error_ = failure(ErrorCode::syntax_error, "unterminated single quote");
        flush_literal();
        flush_tilde();
        literal_kind_ = ExpansionKind::single_quoted;
        for (; position_ < text_.size(); ++position_) {
            append_literal(text_[position_], false);
        }
        flush_literal();
        return;
    }
    flush_literal();
    flush_tilde();
    Expansion fragment = make_expansion(ExpansionKind::single_quoted, std::string(text_.substr(position_, close - position_)));
    fragment.globbable = false;
    fragments_.push_back(std::move(fragment));
    position_ = close + 1;
}

void Lexer::lex_double_quote(bool word_start) {
    ++position_; // consume "
    flush_literal();
    flush_tilde();
    literal_kind_ = ExpansionKind::double_quoted;
    literal_active_ = true;
    std::size_t start = position_;
    // Emits the literal span accumulated since the last inner expansion. A
    // double-quoted word is a sequence of spans interleaved with expansions, and
    // every span must survive.
    auto emit_span = [&] {
        if (position_ <= start) {
            return;
        }
        fragments_.push_back(
            make_expansion(ExpansionKind::double_quoted, std::string(text_.substr(start, position_ - start))));
    };

    while (position_ < text_.size() && text_[position_] != '"') {
        if (text_[position_] == '\\' && position_ + 1 < text_.size()) {
            const char escaped = text_[position_ + 1];
            // Inside double quotes a backslash is special only before $, `, ", \.
            if (escaped == '$' || escaped == '`' || escaped == '"' || escaped == '\\') {
                position_ += 2;
                continue;
            }
            ++position_;
            continue;
        }
        if (text_[position_] == '$' || text_[position_] == '`') {
            emit_span();
            if (text_[position_] == '$') {
                lex_dollar();
            } else {
                lex_backquote(true);
            }
            literal_kind_ = ExpansionKind::double_quoted;
            start = position_;
            continue;
        }
        ++position_;
    }
    // The trailing literal span is emitted even when empty: `""` and `"$x"` are
    // words with a quote-protected component, which is what makes them produce
    // exactly one (possibly empty) field instead of none.
    {
        Expansion fragment = make_expansion(ExpansionKind::double_quoted, std::string(text_.substr(start, position_ - start)));
        fragment.globbable = false;
        fragments_.push_back(std::move(fragment));
    }
    literal_.clear();
    literal_active_ = false;
    literal_kind_ = ExpansionKind::raw;
    if (position_ < text_.size()) {
        ++position_; // consume the closing quote
    } else {
        error_ = failure(ErrorCode::syntax_error, "unterminated double quote");
    }
    (void)word_start;
}

void Lexer::lex_word(bool word_start) {
    const std::size_t start = position_;
    bool quoted = false;
    bool io_candidate = true;
    // Tilde expansion is armed at the start of a word and re-armed after each
    // ':' (so PATH=~/bin:$PATH expands both). Any other literal or expansion
    // disarms it.
    bool tilde_armed = true;

    auto finish = [&] {
        flush_literal();
        flush_tilde();
        Token token;
        token.kind = TokenKind::word;
        token.fragments = std::move(fragments_);
        token.quoted = quoted;
        token.offset = start;
        // The literal spelling of the word, used for reserved-word recognition
        // and case patterns. Every fragment contributes exactly once.
        for (const Expansion& fragment : token.fragments) {
            token.text += fragment.text;
        }
        fragments_.clear();
        literal_.clear();
        literal_active_ = false;
        literal_kind_ = ExpansionKind::raw;
        // A `<<`/`<<-` immediately before this word makes it a delimiter; the
        // body is read at the next newline.
        if (expect_heredoc_) {
            expect_heredoc_ = false;
            pending_heredocs_.push_back(
                PendingHeredoc {token.text, heredoc_strip_pending_, tokens_.size()});
        }
        tokens_.push_back(std::move(token));
    };

    while (position_ < text_.size()) {
        const char ch = text_[position_];

        if (ch == '\\') {
            quoted = true;
            tilde_armed = false;
            flush_tilde();
            if (position_ + 1 < text_.size()) {
                if (text_[position_ + 1] == '\n') {
                    // Line continuation: the backslash-newline pair disappears.
                    position_ += 2;
                    literal_kind_ = ExpansionKind::raw;
                    continue;
                }
                if (text_[position_ + 1] == '\r' && position_ + 2 < text_.size() && text_[position_ + 2] == '\n') {
                    position_ += 3;
                    literal_kind_ = ExpansionKind::raw;
                    continue;
                }
                append_literal(text_[position_ + 1], false);
            }
            position_ = std::min(position_ + 2, text_.size());
            literal_kind_ = ExpansionKind::raw;
            continue;
        }

        if (ch == '\'') {
            quoted = true;
            tilde_armed = false;
            lex_single_quote();
            continue;
        }

        if (ch == '"') {
            quoted = true;
            tilde_armed = false;
            lex_double_quote(word_start);
            continue;
        }

        if (ch == '$') {
            io_candidate = false;
            tilde_armed = false;
            lex_dollar();
            continue;
        }

        if (ch == '`') {
            io_candidate = false;
            tilde_armed = false;
            lex_backquote(false);
            continue;
        }

        if (ch == '~' && tilde_armed && !tilde_pending_) {
            // Tilde expansion applies at the start of a word, and after the first
            // ':' of an assignment value. The specification is `~` or `~name`;
            // everything from the first '/' on is a literal path.
            std::size_t name_end = position_ + 1;
            // `~+` and `~-` are the working and previous directories, not login
            // names, so they are single-character specifications.
            if (name_end < text_.size() && (text_[name_end] == '+' || text_[name_end] == '-')) {
                ++name_end;
            } else {
                while (name_end < text_.size() && is_name_char(text_[name_end])) {
                    ++name_end;
                }
            }
            const bool bare = name_end == position_ + 1;
            // A named tilde is still a prefix when the name ends the word, so the
            // terminator may be a metacharacter, a blank, or end of input.
            const bool before_slash = !bare && name_end < text_.size() && text_[name_end] == '/';
            const bool terminated = name_end >= text_.size()
                || std::isspace(static_cast<unsigned char>(text_[name_end])) != 0
                || std::string_view(";|&<>()").find(text_[name_end]) != std::string_view::npos;
            if (bare || before_slash || terminated) {
                flush_literal();
                tilde_pending_ = true;
                tilde_spec_ = std::string(text_.substr(position_, name_end - position_));
                position_ = name_end;
                tilde_armed = false;
                continue;
            }
        }

        // A metacharacter ends the word.
        if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '|' || ch == '&' || ch == ';'
            || ch == '<' || ch == '>' || ch == '(' || ch == ')') {
            break;
        }

        if (ch == '\r') {
            ++position_;
            continue;
        }

        // ':' re-arms tilde expansion, which is what makes PATH=~/bin:$PATH
        // expand both tildes. The ':' is flushed into its own fragment first so
        // it can act as a boundary.
        if (ch == ':' && !quoted) {
            flush_literal();
            flush_tilde();
            append_literal(ch, false);
            flush_literal();
            tilde_armed = true;
            ++position_;
            continue;
        }

        // '=' also re-arms tilde expansion, for the value of an assignment, but
        // it must stay in the same fragment as the name: the parser recognises
        // an assignment by finding '=' inside the head fragment.
        if (ch == '=' && !quoted) {
            append_literal(ch, false);
            flush_literal();
            tilde_armed = true;
            ++position_;
            continue;
        }

        append_literal(ch, word_start);
        tilde_armed = false;
        ++position_;
    }

    const bool after_duplicate = !tokens_.empty() && tokens_.back().kind == TokenKind::operator_token
        && (tokens_.back().op == Operator::redirect_dup_in || tokens_.back().op == Operator::redirect_dup_out);
    // POSIX: an IO_NUMBER is a digit run immediately followed by a redirection
    // operator, which includes the descriptor-duplication forms.
    const bool starts_redirection = position_ < text_.size()
        && (text_[position_] == '<' || text_[position_] == '>' || text_[position_] == '&');
    if (io_candidate && !after_duplicate && starts_redirection
        && is_io_number(std::string(text_.substr(start, position_ - start)))) {
        // A number immediately followed by a redirection operator is an IO number,
        // not a word -- unless it is the operand of a `>&`, where the following
        // redirection belongs to a separate operator.
        flush_literal();
        flush_tilde();
        Token token;
        token.kind = TokenKind::io_number;
        token.text = std::string(text_.substr(start, position_ - start));
        token.offset = start;
        tokens_.push_back(std::move(token));
        // The digits are consumed by the IO number, so the word builder must not
        // carry them into the next word.
        fragments_.clear();
        literal_.clear();
        literal_active_ = false;
        literal_kind_ = ExpansionKind::raw;
        return;
    }

    finish();
}

void Lexer::lex_line() {
    while (position_ < text_.size()) {
        const char ch = text_[position_];

        if (ch == '\\' && position_ + 1 < text_.size()
            && (text_[position_ + 1] == '\n' || (text_[position_ + 1] == '\r' && text_[position_ + 2] == '\n'))) {
            position_ += (text_[position_ + 1] == '\n') ? 2 : 3;
            continue;
        }

        if (ch == '\r') {
            ++position_;
            continue;
        }

        if (ch == '\n') {
            // The here-document body begins on the next line, so the newline is
            // consumed before the bodies are read.
            ++position_;
            push_newline(position_ - 1);
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            ++position_;
            continue;
        }

        if (ch == '#') {
            // A '#' at the start of a token introduces a comment.
            const std::size_t line_end = text_.find('\n', position_);
            position_ = line_end == std::string_view::npos ? text_.size() : line_end;
            continue;
        }

        if (ch == '&') {
            const std::size_t offset = position_;
            if (position_ + 1 < text_.size()) {
                const char next = text_[position_ + 1];
                if (next == '&') {
                    push_operator(Operator::and_if, offset);
                    position_ += 2;
                    continue;
                }
                if (next == '>') {
                    if (position_ + 2 < text_.size() && text_[position_ + 2] == '>') {
                        push_operator(Operator::redirect_amp_append, offset);
                        position_ += 3;
                        continue;
                    }
                    push_operator(Operator::redirect_amp_out, offset);
                    position_ += 2;
                    continue;
                }
            }
            push_operator(Operator::amp, offset);
            ++position_;
            continue;
        }

        if (ch == '|') {
            const std::size_t offset = position_;
            if (position_ + 1 < text_.size() && text_[position_ + 1] == '|') {
                push_operator(Operator::or_if, offset);
                position_ += 2;
                continue;
            }
            push_operator(Operator::pipe, offset);
            ++position_;
            continue;
        }

        if (ch == ';') {
            const std::size_t offset = position_;
            const char next = position_ + 1 < text_.size() ? text_[position_ + 1] : '\0';
            if (next == ';') {
                if (position_ + 2 < text_.size() && text_[position_ + 2] == '&') {
                    push_operator(Operator::semicolon_and, offset);
                    position_ += 3;
                    continue;
                }
                push_operator(Operator::double_semicolon, offset);
                position_ += 2;
                continue;
            }
            if (next == '&') {
                // `;&` ends a case item and continues into the next one.
                push_operator(Operator::semicolon_and, offset);
                position_ += 2;
                continue;
            }
            push_operator(Operator::semicolon, offset);
            ++position_;
            continue;
        }

        if (ch == '(' || ch == ')') {
            push_operator(ch == '(' ? Operator::subshell_open : Operator::subshell_close, position_);
            ++position_;
            continue;
        }

        if (ch == '<') {
            const std::size_t offset = position_;
            if (position_ + 1 < text_.size()) {
                const char next = text_[position_ + 1];
                if (next == '<' && position_ + 2 < text_.size() && text_[position_ + 2] == '<') {
                    push_operator(Operator::redirect_herestring, offset);
                    position_ += 3;
                    continue;
                }
                if (next == '<' && position_ + 2 < text_.size() && text_[position_ + 2] == '-') {
                    push_operator(Operator::redirect_heredoc_strip, offset);
                    position_ += 3;
                    continue;
                }
                if (next == '<') {
                    push_operator(Operator::redirect_heredoc, offset);
                    position_ += 2;
                    continue;
                }
                if (next == '&') {
                    push_operator(Operator::redirect_dup_in, offset);
                    position_ += 2;
                    continue;
                }
                if (next == '>') {
                    push_operator(Operator::redirect_read_write, offset);
                    position_ += 2;
                    continue;
                }
            }
            push_operator(Operator::redirect_in, offset);
            ++position_;
            continue;
        }

        if (ch == '>') {
            const std::size_t offset = position_;
            if (position_ + 1 < text_.size()) {
                const char next = text_[position_ + 1];
                if (next == '>') {
                    push_operator(Operator::redirect_append, offset);
                    position_ += 2;
                    continue;
                }
                if (next == '&') {
                    push_operator(Operator::redirect_dup_out, offset);
                    position_ += 2;
                    continue;
                }
                if (next == '|') {
                    push_operator(Operator::redirect_clobber, offset);
                    position_ += 2;
                    continue;
                }
            }
            push_operator(Operator::redirect_out, offset);
            ++position_;
            continue;
        }

        // Anything else begins a word.
        lex_word(/*word_start=*/true);

        // Mark a bare unquoted word as a reserved word when it spells one.
        if (!tokens_.empty()) {
            Token& token = tokens_.back();
            if (token.kind == TokenKind::word && !token.quoted && token.fragments.size() == 1
                && token.fragments.front().kind == ExpansionKind::raw) {
                for (const std::string_view reserved : k_reserved_words) {
                    if (token.text == reserved) {
                        token.reserved = true;
                        break;
                    }
                }
            }
        }
    }
}

} // namespace lsh::cli
