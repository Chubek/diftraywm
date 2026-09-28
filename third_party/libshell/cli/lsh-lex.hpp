#pragma once

// Layer 8a -- shell tokenizer.
//
// Converts shell text into a token stream. The lexer is the only component that
// knows about quoting, so the parser above it deals exclusively in typed
// fragments, operators, and reserved words.
//
// Two properties are load-bearing and are the reason the lexer exists as a
// separate layer:
//
//   * quote provenance. Every fragment records whether it is unquoted, so
//     field splitting and pathname expansion can be applied later to exactly the
//     regions that POSIX says they apply to.
//   * here-documents. A here-document body is text that appears *after* the
//     newline that ends its command line, so the lexer must own the whole script
//     rather than one line at a time.

#include "LibShell.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lsh::cli {

enum class TokenKind : std::uint8_t {
    word,
    operator_token,
    newline,
    io_number,
    end,
};

enum class Operator : std::uint8_t {
    and_if,           // &&
    or_if,            // ||
    pipe,             // |
    amp,              // &
    semicolon,        // ;
    double_semicolon, // ;;
    semicolon_and,    // ;&
    subshell_open,    // (
    subshell_close,   // )
    redirect_in,      // <
    redirect_out,     // >
    redirect_append,  // >>
    redirect_clobber, // >|
    redirect_read_write, // <>
    redirect_dup_in,  // <&
    redirect_dup_out, // >&
    redirect_heredoc,      // <<
    redirect_heredoc_strip,// <<-
    redirect_herestring,   // <<<
    redirect_amp_out,      // &>
    redirect_amp_append,   // &>>
};

struct Token {
    TokenKind kind {TokenKind::word};
    std::vector<Expansion> fragments;
    // Operator spelling, io-number digits, or the literal text of a word.
    std::string text;
    // The word contained any quote or escape, so it can never be a reserved
    // word and its literal characters are not metacharacters.
    bool quoted {false};
    bool reserved {false};
    Operator op {Operator::and_if};
    // Here-document body, attached to the delimiter word token.
    std::string heredoc;
    bool heredoc_strip {false};
    // Original source span, used for diagnostics.
    std::size_t offset {0};
};

class Lexer {
public:
    explicit Lexer(std::string_view text);

    // Produces the whole token stream up front so a here-document body can be
    // consumed after the line that declared it.
    [[nodiscard]] const std::deque<Token>& tokens() const noexcept { return tokens_; }
    [[nodiscard]] const std::optional<Diagnostic>& error() const noexcept { return error_; }

private:
    struct PendingHeredoc {
        std::string delimiter;
        bool strip {false};
        // Index of the delimiter token the body belongs to. A command may have
        // more words after the delimiter (`cat <<EOF other`), so the body cannot
        // be found by scanning backwards from the newest token.
        std::size_t token_index {0};
    };

    void run();
    void lex_line();
    void read_heredoc_bodies();
    void push_operator(Operator op, std::size_t offset);
    void push_newline(std::size_t offset);
    void lex_word(bool at_word_start);
    void lex_dollar();
    // Fills `fragment` from a ${...} body; false records a diagnostic.
    bool parse_parameter_body(const std::string& body, Expansion& fragment);
    void lex_backquote(bool quoted);
    void lex_single_quote();
    void lex_double_quote(bool word_start);
    void flush_literal();
    void append_literal(char ch, bool word_start);
    void flush_tilde();

    // Scans a balanced construct that opens at `text[index]`, advancing past it.
    Result<std::string> scan_balanced(std::size_t& index, char open, char close, const char* what);
    // Copies a nested $(...) or backquote construct, returning the index past it.
    [[nodiscard]] std::size_t copy_substitution(std::size_t start) const;

    std::string_view text_;
    std::deque<Token> tokens_;
    std::vector<Expansion> fragments_;
    std::string literal_;
    // Quote context of the literal currently being accumulated.
    ExpansionKind literal_kind_ {ExpansionKind::raw};
    bool literal_active_ {false};
    bool tilde_pending_ {false};
    bool expect_heredoc_ {false};
    bool heredoc_strip_pending_ {false};
    std::optional<Diagnostic> error_;
    std::string tilde_spec_;
    std::size_t position_ {0};
    std::vector<PendingHeredoc> pending_heredocs_;
};

} // namespace lsh::cli
