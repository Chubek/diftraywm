#pragma once

// Layer 1 -- expansion.
//
// Word decomposition, tilde expansion, parameter expansion, field splitting,
// pathname expansion, and arithmetic evaluation. The model is POSIX-shaped:
//
//   * a Word is a typed Fragment list, so quote provenance survives parsing;
//   * expansion produces Fragments carrying per-origin split/glob eligibility
//     rather than one flat string, so `a"$x"` and `$x` are distinguishable;
//   * field splitting and pathname expansion are applied afterwards, over the
//     regions that were *not* quote-protected.
//
// This layer knows nothing about IR or execution; it only materializes text.

#include "Core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lsh {

enum class ExpansionKind : std::uint8_t {
    raw,            // unquoted literal
    single_quoted,  // '...'
    double_quoted,  // "..."
    variable,       // $NAME ${NAME...}
    arithmetic,     // $((...))
    command,        // $(...) or `...`
    lua,            // inline QaMRpp
    glob,           // explicit pattern (DSL-constructed)
    tilde,          // leading ~ / ~user / ~+ / ~-
    special,        // $? $$ $! $# $* $@ $-
    positional,     // $0 .. $9
};

// POSIX 2.6.2 parameter expansion operators.
enum class ParameterOp : std::uint8_t {
    none,      // $x
    length,    // ${#x}
    alternate, // ${x-word}  ${x:-word}
    assign,    // ${x=word}  ${x:=word}
    error,     // ${x?word}  ${x:?word}
    plus,      // ${x+word}  ${x:+word}
};

// Prefix/suffix removal forms of ${x#pat} / ${x##pat} / ${x%pat} / ${x%%pat}.
enum class TrimOp : std::uint8_t {
    none,
    prefix,
    longest_prefix,
    suffix,
    longest_suffix,
};

struct Expansion {
    ExpansionKind kind {ExpansionKind::raw};
    // Literal payload, variable name, arithmetic body, substitution body, tilde
    // spec, or special-parameter character depending on `kind`.
    std::string text;

    // Unquoted expandable fragment: the result participates in field splitting
    // and pathname expansion. Quote-protected fragments set this to false.
    bool field_splitting {false};

    // The fragment sits unquoted at a word start (or after the first ':' of an
    // assignment), so tilde expansion applies.
    bool tilde_prefix {false};

    // Pathname expansion is active: '*', '?' and '[' keep their metacharacter
    // meaning. Distinct from `field_splitting`, which governs IFS splitting of
    // an expansion *result* -- a literal is globbed but never split.
    bool globbable {false};

    ParameterOp op {ParameterOp::none};
    // ':' variant of op: tests "unset or null" instead of "unset".
    bool colon {false};
    bool has_operand {false};
    std::string operand;

    TrimOp trim {TrimOp::none};
    bool has_pattern {false};
    std::string pattern;
};

// Expansion construction helper. Expansion is a wide flat struct; building it
// through one factory keeps call sites readable and keeps designated-initializer
// warnings from firing as fields are added.
[[nodiscard]] inline Expansion make_expansion(ExpansionKind kind, std::string text = {}) {
    Expansion expansion;
    expansion.kind = kind;
    expansion.text = std::move(text);
    return expansion;
}

struct Argument {
    std::vector<Expansion> fragments;

    static Argument raw(std::string value) {
        return Argument {{make_expansion(ExpansionKind::raw, std::move(value))}};
    }
};

// One materialized region of an expanded word. `globbable` marks the region as
// one where '*', '?' and '[' retain pathname-expansion meaning; quoted regions
// are literal. `splittable` marks a region that came from an unquoted
// substitution and therefore undergoes IFS field splitting. `field_boundary`
// marks a region that expands to zero or more complete fields ("$@"/"$*"),
// which the field assembler forwards verbatim.
struct ExpandedFragment {
    std::string text;
    std::vector<std::string> fields;
    // Per-character mask of `text` marking positions where glob metacharacters
    // are still active. Only meaningful when `globbable` is set.
    std::vector<bool> meta;
    bool splittable {false};
    bool globbable {false};
    bool field_boundary {false};
};

namespace detail {

[[nodiscard]] inline std::optional<std::string> parse_long_long(std::string_view text, int base) {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string owned(text);
    char* stop = nullptr;
    const long long value = std::strtoll(owned.c_str(), &stop, base);
    if (stop == owned.c_str() || *stop != '\0') {
        return std::nullopt;
    }
    return std::to_string(value);
}

// ---- tilde ----------------------------------------------------------------

// Resolves a tilde specification. Returning nullopt means "not mine": the
// specification is left literal, which is the POSIX behaviour for an unknown
// login name.
class TildeResolver {
public:
    virtual ~TildeResolver() = default;
    virtual Result<std::optional<std::string>> resolve(std::string_view spec) const = 0;
};

// $HOME / $PWD / $OLDPWD based resolver. `~user` is left literal unless a
// platform resolver (see LibShell-Posix.hpp) is installed.
class EnvironmentTildeResolver final : public TildeResolver {
public:
    explicit EnvironmentTildeResolver(const Environment& environment) : environment_(&environment) {}

    Result<std::optional<std::string>> resolve(std::string_view spec) const override {
        if (spec == "~") {
            return std::optional<std::string> {environment_->get("HOME")};
        }
        if (spec == "~+") {
            if (auto value = environment_->get("PWD"); value && !value->empty()) {
                return std::optional<std::string> {*value};
            }
            return std::optional<std::string> {};
        }
        if (spec == "~-") {
            return std::optional<std::string> {environment_->get("OLDPWD")};
        }
        return std::optional<std::string> {};
    }

private:
    const Environment* environment_;
};

// ---- field splitting ------------------------------------------------------

[[nodiscard]] inline bool ifs_is_space(char ch, std::string_view ifs) {
    return (ch == ' ' || ch == '\t' || ch == '\n') && ifs.find(ch) != std::string_view::npos;
}

[[nodiscard]] inline bool ifs_is_delimiter(char ch, std::string_view ifs) {
    return ifs.find(ch) != std::string_view::npos;
}

// POSIX field splitting: sequences of IFS whitespace delimit fields and are
// ignored at the boundaries; non-whitespace IFS characters each delimit a
// field on their own.
inline void split_fields(std::string_view text, std::string_view ifs, std::vector<std::string>& out) {
    if (text.empty()) {
        return;
    }
    if (ifs.empty()) {
        out.emplace_back(text);
        return;
    }

    std::size_t index = 0;
    while (index < text.size() && ifs_is_space(text[index], ifs)) {
        ++index;
    }
    if (index == text.size()) {
        return;
    }

    std::string current;
    while (index < text.size()) {
        const char ch = text[index];
        if (ifs_is_space(ch, ifs)) {
            out.push_back(current);
            current.clear();
            while (index < text.size() && ifs_is_space(text[index], ifs)) {
                ++index;
            }
            continue;
        }
        if (ifs_is_delimiter(ch, ifs)) {
            // A non-whitespace delimiter ends the current field; the whitespace
            // that follows does not open an additional field.
            out.push_back(current);
            current.clear();
            ++index;
            while (index < text.size() && ifs_is_space(text[index], ifs)) {
                ++index;
            }
            continue;
        }
        current.push_back(ch);
        ++index;
    }
    out.push_back(current);
}

// ---- pathname expansion ---------------------------------------------------

// Matches one path component. `meta` marks positions where glob metacharacters
// are active; positions where it is false are literal (quoted) characters.
// Supports '*', '?', and bracket expressions with '!'/'^' negation and ranges.
[[nodiscard]] inline bool glob_component_matches(std::string_view name, std::string_view pattern, const std::vector<bool>& meta) {
    const std::size_t name_size = name.size();
    const std::size_t pattern_size = pattern.size();
    if (meta.size() != pattern_size) {
        return false;
    }

    // Scans a bracket expression starting at pattern[p] == '['. On success
    // returns the index of the closing ']' and sets `matched`; on an
    // unterminated expression returns p so the caller can treat '[' literally.
    auto bracket = [&](std::size_t p, char ch, bool& matched) -> std::size_t {
        std::size_t q = p + 1;
        bool negate = false;
        if (q < pattern_size && (pattern[q] == '!' || pattern[q] == '^')) {
            negate = true;
            ++q;
        }
        bool hit = false;
        bool first = true;
        while (q < pattern_size && (pattern[q] != ']' || first)) {
            first = false;
            if (q + 2 < pattern_size && pattern[q + 1] == '-' && pattern[q + 2] != ']') {
                const auto lo = static_cast<unsigned char>(pattern[q]);
                const auto hi = static_cast<unsigned char>(pattern[q + 2]);
                const auto value = static_cast<unsigned char>(ch);
                hit = hit || (value >= lo && value <= hi);
                q += 3;
                continue;
            }
            hit = hit || pattern[q] == ch;
            ++q;
        }
        if (q >= pattern_size) {
            matched = false;
            return p;
        }
        matched = negate ? !hit : hit;
        return q;
    };

    // '*' backtracking: remember the star's pattern index and the name index it
    // was first tried at, then consume one more name character and retry.
    std::size_t star_pattern = static_cast<std::size_t>(-1);
    std::size_t star_name = 0;
    std::size_t p = 0;
    std::size_t n = 0;

    while (n < name_size) {
        if (p < pattern_size && meta[p] && pattern[p] == '*') {
            star_pattern = p;
            star_name = n;
            ++p;
            continue;
        }
        if (p < pattern_size && meta[p] && pattern[p] == '?') {
            ++p;
            ++n;
            continue;
        }
        if (p < pattern_size && meta[p] && pattern[p] == '[') {
            bool matched = false;
            const std::size_t close = bracket(p, name[n], matched);
            if (close != p) {
                if (matched) {
                    p = close + 1;
                    ++n;
                    continue;
                }
            } else if (name[n] == '[') {
                ++p;
                ++n;
                continue;
            }
        }
        if (p < pattern_size && name[n] == pattern[p]) {
            ++p;
            ++n;
            continue;
        }
        if (star_pattern != static_cast<std::size_t>(-1)) {
            p = star_pattern + 1;
            n = ++star_name;
            continue;
        }
        return false;
    }
    while (p < pattern_size && meta[p] && pattern[p] == '*') {
        ++p;
    }
    return p == pattern_size;
}

[[nodiscard]] inline bool has_active_glob_meta(const std::string& text, const std::vector<bool>& meta) {
    const std::size_t limit = std::min(text.size(), meta.size());
    for (std::size_t index = 0; index < limit; ++index) {
        if (!meta[index]) {
            continue;
        }
        const char ch = text[index];
        if (ch == '*' || ch == '?') {
            return true;
        }
        if (ch == '[') {
            // A bracket expression is a metacharacter only if it is terminated.
            for (std::size_t scan = index + 1; scan < text.size() && scan < meta.size(); ++scan) {
                if (meta[scan] && text[scan] == ']') {
                    return true;
                }
            }
        }
    }
    return false;
}

// Expands `pattern` against the filesystem. `meta[i]` false means the
// character at that offset is quote-protected and therefore literal. The
// pattern is decomposed on '/', which is structural and never a metacharacter.
// An unmatched pattern is returned unchanged, per POSIX.
// `base` is the directory a relative pattern is resolved against. Pathname
// expansion follows the shell's working directory, which is not the process's.
[[nodiscard]] inline std::vector<std::string> expand_glob(
    const std::string& pattern, const std::vector<bool>& meta, const std::filesystem::path* base = nullptr) {
    if (!has_active_glob_meta(pattern, meta)) {
        return {pattern};
    }

    std::size_t offset = 0;
    std::size_t component_begin = 0;
    const bool relative = pattern.empty() || pattern.front() != '/';
    std::vector<std::string> candidates {relative ? std::string(base != nullptr ? base->string() : ".") : "/"};

    while (component_begin <= pattern.size()) {
        std::size_t component_end = pattern.find('/', component_begin);
        const bool last = component_end == std::string::npos;
        if (last) {
            component_end = pattern.size();
        }
        const std::string component = pattern.substr(component_begin, component_end - component_begin);
        const std::vector<bool> slice(meta.begin() + static_cast<std::ptrdiff_t>(offset),
                                      meta.begin() + static_cast<std::ptrdiff_t>(offset + component.size()));

        if (component == "..") {
            std::vector<std::string> parents;
            for (const std::string& candidate : candidates) {
                if (candidate == "/") {
                    parents.emplace_back("/");
                    continue;
                }
                const std::filesystem::path parent = std::filesystem::path(candidate).parent_path();
                parents.push_back(parent.empty() ? std::string(".") : parent.string());
            }
            candidates = std::move(parents);
        } else if (!component.empty() && component != ".") {
            const bool dynamic = has_active_glob_meta(component, slice);
            std::error_code ec;
            std::vector<std::string> next;
            for (const std::string& candidate : candidates) {
                if (!dynamic) {
                    const std::filesystem::path full = std::filesystem::path(candidate) / component;
                    if (std::filesystem::exists(full, ec)) {
                        next.push_back(full.string());
                    }
                    continue;
                }
                if (!std::filesystem::is_directory(candidate, ec)) {
                    continue;
                }
                for (std::filesystem::directory_iterator it(candidate, ec), end; it != end; it.increment(ec)) {
                    if (ec) {
                        break;
                    }
                    const std::string name = it->path().filename().string();
                    // A leading dot is matched only by an explicit dot in the
                    // pattern component.
                    if (name.empty() || (name.front() == '.' && component.front() != '.')) {
                        continue;
                    }
                    if (glob_component_matches(name, component, slice)) {
                        next.push_back(it->path().string());
                    }
                }
            }
            candidates = std::move(next);
        }

        if (candidates.empty()) {
            return {pattern};
        }
        if (last) {
            break;
        }
        component_begin = component_end + 1;
        offset = component_begin;
    }

    std::vector<std::string> matches;
    matches.reserve(candidates.size());
    const std::string prefix = base != nullptr ? base->string() : std::string(".");
    for (const std::string& candidate : candidates) {
        // The directory the search was seeded with is an artifact of resolving a
        // relative pattern and is not part of the expansion result.
        if (relative && candidate.size() > prefix.size() + 1 && candidate.compare(0, prefix.size(), prefix) == 0) {
            matches.push_back(candidate.substr(prefix.size() + 1));
            continue;
        }
        if (candidate.size() > 2 && candidate.compare(0, 2, "./") == 0) {
            matches.push_back(candidate.substr(2));
            continue;
        }
        matches.push_back(candidate);
    }
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    if (matches.empty()) {
        return {pattern};
    }
    return matches;
}

[[nodiscard]] inline std::vector<std::string> expand_glob(std::string_view pattern) {
    std::string text(pattern);
    return expand_glob(text, std::vector<bool>(text.size(), true));
}

// ---- arithmetic -----------------------------------------------------------

// Recursive-descent evaluator for POSIX arithmetic. Variables are read through
// `lookup`; unset variables evaluate to 0. Division and modulus by zero are
// reported rather than trapping, and every operation is performed on unsigned
// values so overflow is defined.
class ArithmeticEvaluator {
public:
    using Lookup = std::function<Result<long long>(std::string_view)>;
    using Store = std::function<Result<void>(std::string_view, long long)>;

    explicit ArithmeticEvaluator(std::string_view expression) : expression_(expression) {}

    void set_lookup(Lookup lookup) { lookup_ = std::move(lookup); }
    void set_store(Store store) { store_ = std::move(store); }

    [[nodiscard]] Result<long long> run() {
        auto value = parse_comma();
        if (!value) {
            return value;
        }
        skip_space();
        if (position_ != expression_.size()) {
            return error("trailing characters in arithmetic expression");
        }
        return value;
    }

private:
    static constexpr std::uint64_t max_exponent = 4096;

    [[nodiscard]] Result<long long> error(std::string message) const {
        return failure(ErrorCode::bad_arithmetic, std::move(message), std::string(expression_));
    }

    void skip_space() {
        while (position_ < expression_.size() && std::isspace(static_cast<unsigned char>(expression_[position_]))) {
            ++position_;
        }
    }

    [[nodiscard]] bool looking_at(std::string_view op) const {
        return expression_.compare(position_, op.size(), op) == 0;
    }

    [[nodiscard]] Result<long long> parse_comma() {
        auto value = parse_assignment();
        if (!value) {
            return value;
        }
        for (;;) {
            skip_space();
            if (!looking_at(",")) {
                return value;
            }
            ++position_;
            value = parse_assignment();
            if (!value) {
                return value;
            }
        }
    }

    [[nodiscard]] Result<long long> parse_assignment() {
        const std::size_t start = position_;
        std::size_t name_end = position_;
        while (name_end < expression_.size()
               && (std::isalnum(static_cast<unsigned char>(expression_[name_end])) || expression_[name_end] == '_')) {
            ++name_end;
        }
        // A bare '=' assigns; '==' is a comparison and must be left alone.
        if (name_end > start && name_end < expression_.size() && expression_[name_end] == '='
            && expression_.compare(name_end, 2, "==") != 0) {
            const std::string name(expression_.substr(start, name_end - start));
            position_ = name_end + 1;
            auto value = parse_assignment();
            if (!value) {
                return value;
            }
            if (store_) {
                if (auto assigned = store_(name, value.value()); !assigned) {
                    return assigned.error();
                }
            }
            return value;
        }
        position_ = start;
        return parse_ternary();
    }

    [[nodiscard]] Result<long long> parse_ternary() {
        auto condition = parse_binary(0);
        if (!condition) {
            return condition;
        }
        skip_space();
        if (!looking_at("?")) {
            return condition;
        }
        ++position_;
        auto when_true = parse_assignment();
        if (!when_true) {
            return when_true;
        }
        skip_space();
        if (!looking_at(":")) {
            return error("missing ':' in arithmetic conditional");
        }
        ++position_;
        auto when_false = parse_assignment();
        if (!when_false) {
            return when_false;
        }
        return condition.value() != 0 ? when_true : when_false;
    }

    // Precedence-climbing over the binary levels, from `||` down to `+`/`-`.
    // The multiplicative level is handled by parse_multiplicative because its
    // operands are powers, not plain primaries; `**` binds tighter than every
    // operator in this table.
    [[nodiscard]] Result<long long> parse_binary(std::size_t level) {
        static const std::vector<std::vector<std::string_view>> levels {
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<=", ">=", "<", ">"}, {"<<", ">>"}, {"+", "-"},
        };
        if (level >= levels.size()) {
            return parse_multiplicative();
        }
        auto lhs = parse_binary(level + 1);
        if (!lhs) {
            return lhs;
        }
        for (;;) {
            skip_space();
            const std::vector<std::string_view>& operators = levels[level];
            std::string_view matched;
            for (const std::string_view op : operators) {
                if (!looking_at(op)) {
                    continue;
                }
                if (op.size() == 1 && position_ + 1 < expression_.size() && expression_[position_ + 1] == op.front()) {
                    // `&` must not swallow the first half of `&&`, nor `|` of
                    // `||`; the two-character forms bind looser and are handled
                    // by their own level.
                    continue;
                }
                matched = op;
                break;
            }
            if (matched.empty()) {
                return lhs;
            }
            // '**' is not the multiplicative operator; parse_power owns it.
            if (matched == "*" && looking_at("**")) {
                return lhs;
            }
            // '<<' and '>>' must not be consumed as '<' and '>' by the
            // relational level; the table order already prevents that.
            position_ += matched.size();
            auto rhs = parse_binary(level + 1);
            if (!rhs) {
                return rhs;
            }
            if (auto combined = combine(matched, lhs.value(), rhs.value()); !combined) {
                return combined.error();
            } else {
                lhs.value() = combined.value();
            }
        }
    }

    [[nodiscard]] Result<long long> combine(std::string_view op, long long lhs, long long rhs) const {
        const auto ulhs = static_cast<unsigned long long>(lhs);
        const auto urhs = static_cast<unsigned long long>(rhs);
        if (op == "+") {
            return static_cast<long long>(ulhs + urhs);
        }
        if (op == "-") {
            return static_cast<long long>(ulhs - urhs);
        }
        if (op == "*") {
            return static_cast<long long>(ulhs * urhs);
        }
        if (op == "/") {
            if (rhs == 0) {
                return error("division by zero in arithmetic expansion");
            }
            // The single overflowing pair wraps rather than trapping.
            if (lhs == std::numeric_limits<long long>::min() && rhs == -1) {
                return std::numeric_limits<long long>::min();
            }
            return lhs / rhs;
        }
        if (op == "%") {
            if (rhs == 0) {
                return error("division by zero in arithmetic expansion");
            }
            if (lhs == std::numeric_limits<long long>::min() && rhs == -1) {
                return 0;
            }
            return lhs % rhs;
        }
        if (op == "<<") {
            if (rhs < 0 || urhs >= 64) {
                return 0;
            }
            return static_cast<long long>(ulhs << urhs);
        }
        if (op == ">>") {
            if (rhs < 0) {
                return static_cast<long long>(ulhs >> (64 - 1));
            }
            if (urhs >= 64) {
                return 0;
            }
            return static_cast<long long>(ulhs >> urhs);
        }
        if (op == "<") {
            return static_cast<long long>(lhs < rhs);
        }
        if (op == ">") {
            return static_cast<long long>(lhs > rhs);
        }
        if (op == "<=") {
            return static_cast<long long>(lhs <= rhs);
        }
        if (op == ">=") {
            return static_cast<long long>(lhs >= rhs);
        }
        if (op == "==") {
            return static_cast<long long>(lhs == rhs);
        }
        if (op == "!=") {
            return static_cast<long long>(lhs != rhs);
        }
        if (op == "&") {
            return static_cast<long long>(lhs & rhs);
        }
        if (op == "^") {
            return static_cast<long long>(lhs ^ rhs);
        }
        if (op == "|") {
            return static_cast<long long>(lhs | rhs);
        }
        if (op == "&&") {
            return static_cast<long long>(lhs != 0 && rhs != 0);
        }
        if (op == "||") {
            return static_cast<long long>(lhs != 0 || rhs != 0);
        }
        return error("unhandled arithmetic operator");
    }

    [[nodiscard]] Result<long long> parse_multiplicative() {
        auto lhs = parse_power();
        if (!lhs) {
            return lhs;
        }
        for (;;) {
            skip_space();
            if (position_ >= expression_.size()) {
                return lhs;
            }
            const char op = expression_[position_];
            if (op != '*' && op != '/' && op != '%') {
                return lhs;
            }
            // '**' is a power, not a product.
            if (op == '*' && looking_at("**")) {
                return lhs;
            }
            ++position_;
            auto rhs = parse_power();
            if (!rhs) {
                return rhs;
            }
            auto combined = combine(std::string_view(&op, 1), lhs.value(), rhs.value());
            if (!combined) {
                return combined.error();
            }
            lhs.value() = combined.value();
        }
    }

    [[nodiscard]] Result<long long> parse_power() {
        auto base = parse_unary();
        if (!base) {
            return base;
        }
        skip_space();
        if (!looking_at("**")) {
            return base;
        }
        position_ += 2;
        auto exponent = parse_unary();
        if (!exponent) {
            return exponent;
        }
        if (exponent.value() < 0 || static_cast<std::uint64_t>(exponent.value()) > max_exponent) {
            return error("exponent out of range in arithmetic expansion");
        }
        unsigned long long result = 1;
        const auto multiplier = static_cast<unsigned long long>(base.value());
        for (auto count = static_cast<unsigned long long>(exponent.value()); count > 0; --count) {
            result *= multiplier;
        }
        return static_cast<long long>(result);
    }

    [[nodiscard]] Result<long long> parse_unary() {
        skip_space();
        if (position_ == expression_.size()) {
            return error("unexpected end of arithmetic expression");
        }
        const char ch = expression_[position_];
        if (ch == '+') {
            ++position_;
            return parse_unary();
        }
        if (ch == '-') {
            ++position_;
            auto value = parse_unary();
            if (!value) {
                return value;
            }
            return static_cast<long long>(0ULL - static_cast<unsigned long long>(value.value()));
        }
        if (ch == '!') {
            ++position_;
            auto value = parse_unary();
            if (!value) {
                return value;
            }
            return static_cast<long long>(value.value() == 0);
        }
        if (ch == '~') {
            ++position_;
            auto value = parse_unary();
            if (!value) {
                return value;
            }
            return ~value.value();
        }
        if (ch == '(') {
            ++position_;
            auto value = parse_comma();
            if (!value) {
                return value;
            }
            skip_space();
            if (position_ == expression_.size() || expression_[position_] != ')') {
                return error("missing closing parenthesis in arithmetic expression");
            }
            ++position_;
            return value;
        }
        return parse_primary();
    }

    [[nodiscard]] Result<long long> parse_primary() {
        skip_space();
        if (position_ == expression_.size()) {
            return error("unexpected end of arithmetic expression");
        }
        const char ch = expression_[position_];
        if (ch == '$') {
            // A redundant sigil is permitted: $(( $x + 1 )).
            ++position_;
            return parse_primary();
        }
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            return parse_number();
        }
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            std::size_t end = position_;
            while (end < expression_.size()
                   && (std::isalnum(static_cast<unsigned char>(expression_[end])) || expression_[end] == '_')) {
                ++end;
            }
            const std::string name(expression_.substr(position_, end - position_));
            position_ = end;
            if (!lookup_) {
                return 0;
            }
            return lookup_(name);
        }
        return error("invalid token in arithmetic expression");
    }

    [[nodiscard]] Result<long long> parse_number() {
        int base = 10;
        if (expression_[position_] == '0' && position_ + 1 < expression_.size()) {
            const char marker = expression_[position_ + 1];
            if (marker == 'x' || marker == 'X') {
                base = 16;
                position_ += 2;
            } else if (std::isdigit(static_cast<unsigned char>(marker))) {
                base = 8;
                ++position_;
            }
        }
        const std::size_t digits_start = position_;
        while (position_ < expression_.size() && std::isalnum(static_cast<unsigned char>(expression_[position_]))) {
            const int digit = std::isdigit(static_cast<unsigned char>(expression_[position_])) ? expression_[position_] - '0'
                : (std::islower(static_cast<unsigned char>(expression_[position_])) ? expression_[position_] - 'a' + 10
                                                                                   : expression_[position_] - 'A' + 10);
            if (digit >= base) {
                break;
            }
            ++position_;
        }
        if (position_ == digits_start) {
            return error("malformed numeric literal in arithmetic expression");
        }
        const std::string digits(expression_.substr(digits_start, position_ - digits_start));

        // base#value form; `base` was read as a decimal literal above.
        if (position_ < expression_.size() && expression_[position_] == '#') {
            int explicit_base = 0;
            for (char ch : digits) {
                explicit_base = explicit_base * 10 + (ch - '0');
            }
            if (explicit_base < 2 || explicit_base > 36) {
                return error("invalid arithmetic base");
            }
            const std::size_t value_start = position_ + 1;
            std::size_t value_end = value_start;
            unsigned long long parsed = 0;
            while (value_end < expression_.size() && std::isalnum(static_cast<unsigned char>(expression_[value_end]))) {
                const char ch = expression_[value_end];
                int digit = 0;
                if (ch >= '0' && ch <= '9') {
                    digit = ch - '0';
                } else if (ch >= 'a' && ch <= 'z') {
                    digit = ch - 'a' + 10;
                } else {
                    digit = ch - 'A' + 10;
                }
                if (digit >= explicit_base) {
                    return error("invalid digit for arithmetic base");
                }
                parsed = parsed * static_cast<unsigned long long>(explicit_base) + static_cast<unsigned long long>(digit);
                ++value_end;
            }
            if (value_end == value_start) {
                return error("missing digits after arithmetic base");
            }
            position_ = value_end;
            return static_cast<long long>(parsed);
        }

        unsigned long long value = 0;
        for (char ch : digits) {
            int digit = 0;
            if (ch >= '0' && ch <= '9') {
                digit = ch - '0';
            } else if (ch >= 'a' && ch <= 'f') {
                digit = ch - 'a' + 10;
            } else {
                digit = ch - 'A' + 10;
            }
            value = value * static_cast<unsigned long long>(base) + static_cast<unsigned long long>(digit);
        }
        return static_cast<long long>(value);
    }

    std::string_view expression_;
    Lookup lookup_;
    Store store_;
    std::size_t position_ {0};
};

} // namespace detail

// ---- redirection targets ---------------------------------------------------

struct StdioTarget {
    StdioTargetKind kind {StdioTargetKind::inherit};
    std::optional<std::string> file;
    std::optional<int> fd;
    std::shared_ptr<Writer> memory;   // output sink (memory or sinklet payload)
    std::shared_ptr<Sinklet> sinklet;
    std::shared_ptr<Reader> input;    // input source (heredoc, here-string, capture)
    // A redirection path that has not been expanded yet. The Shell resolves it
    // into a concrete `file` target, so a quoted or parameterised path still
    // works.
    std::optional<Argument> deferred;
    // A here-string word, resolved into an in-memory payload. Distinct from
    // `deferred` because the expansion result is data, not a name.
    std::optional<Argument> here_string;
};

struct Redirection {
    RedirectStream stream {RedirectStream::stdout_stream};
    RedirectMode mode {RedirectMode::truncate};
    StdioTarget target;
    // Explicit source descriptor, as in `3>file` or `2>&1`. When absent the
    // stream selector determines the descriptor.
    std::optional<int> source_fd;
};

[[nodiscard]] inline std::optional<std::size_t> stream_index(RedirectStream stream) {
    switch (stream) {
    case RedirectStream::stdin_stream:
        return std::size_t {0};
    case RedirectStream::stdout_stream:
        return std::size_t {1};
    case RedirectStream::stderr_stream:
        return std::size_t {2};
    }
    return std::nullopt;
}

// ---- expansion context ----------------------------------------------------

// Supplies the non-environment state that expansion depends on: positional
// parameters, special parameters, working directory, and the scripting backend.
// Bundled into one value so the Expander signature stays small and so the
// embedded-language path is explicit rather than ambient.
struct ExpandContext {
    Environment* environment {nullptr};
    const std::vector<std::string>* positional {nullptr};
    std::string shell_name {"libsh"};
    std::string dash_options {"-c"};
    int last_status {0};
    int last_background_pid {0};
    int shell_pid {0};
    const std::filesystem::path* cwd {nullptr};
    std::shared_ptr<detail::TildeResolver> tilde;
    std::shared_ptr<class ScriptingBackend> scripting;

    [[nodiscard]] std::string ifs() const {
        if (environment) {
            if (auto value = environment->get("IFS"); value) {
                return *value;
            }
        }
        return " \t\n";
    }
};

// Backend for substitutions that need the shell to re-enter itself
// (command substitution) or the embedded Lua engine.
class ScriptingBackend {
public:
    virtual ~ScriptingBackend() = default;
    virtual Result<std::string> eval_command(std::string_view script, const Environment& environment) = 0;
    virtual Result<std::string> eval_lua(std::string_view script, const Environment& environment) = 0;
};

// Backend that rejects both substitution kinds. The default for a Shell whose
// owner has not supplied an implementation: substitutions fail loudly instead
// of silently expanding to nothing.
class NullScriptingBackend final : public ScriptingBackend {
public:
    Result<std::string> eval_command(std::string_view script, const Environment&) override {
        return failure(ErrorCode::bad_expansion, "command substitution backend is unavailable", std::string(script));
    }

    Result<std::string> eval_lua(std::string_view script, const Environment&) override {
        return failure(ErrorCode::bad_expansion, "Lua backend is unavailable", std::string(script));
    }
};

// Materializes words. Every entry point returns diagnostics on malformed input
// rather than producing a partially expanded string.
class Expander {
public:
    Expander() = default;
    explicit Expander(std::shared_ptr<ScriptingBackend> scripting) : scripting_(std::move(scripting)) {}

    void set_scripting(std::shared_ptr<ScriptingBackend> scripting) { scripting_ = std::move(scripting); }
    [[nodiscard]] const std::shared_ptr<ScriptingBackend>& scripting() const noexcept { return scripting_; }

    // Expands a single fragment to text. The split/glob metadata is discarded;
    // use expand_word for argv materialization.
    virtual Result<std::string> expand_fragment(const Expansion& expansion, const ExpandContext& context) const;

    // Expands a word into the fragments that survive quote removal, carrying
    // split/glob eligibility per region.
    virtual Result<std::vector<ExpandedFragment>> expand_word(const Argument& argument, const ExpandContext& context) const;

    // Full argv materialization: quote removal, tilde expansion, parameter
    // expansion, field splitting, then pathname expansion.
    virtual Result<std::vector<std::string>> expand_argv(const std::vector<Argument>& arguments, const ExpandContext& context) const;

    Result<std::vector<std::string>> expand_argv(const std::vector<Argument>& arguments, const Environment& environment) const {
        ExpandContext context;
        context.environment = const_cast<Environment*>(&environment);
        context.scripting = scripting_;
        return expand_argv(arguments, context);
    }

protected:
    [[nodiscard]] virtual Result<std::string> expand_variable(const Expansion& expansion, const ExpandContext& context) const;
    [[nodiscard]] virtual Result<std::string> expand_special(const Expansion& expansion, const ExpandContext& context) const;
    [[nodiscard]] virtual Result<std::string> expand_tilde(const Expansion& expansion, const ExpandContext& context) const;
    [[nodiscard]] virtual Result<std::string> expand_arithmetic(const Expansion& expansion, const ExpandContext& context) const;
    [[nodiscard]] virtual Result<std::string> expand_command(const Expansion& expansion, const ExpandContext& context) const;

    // Applies parameter operators and trimming to an already-resolved value.
    // `set` implements ${x=word} / ${x:=word}.
    [[nodiscard]] Result<std::string> apply_parameter(
        const Expansion& expansion,
        const std::string& value,
        const bool is_set,
        const ExpandContext& context,
        const std::function<Result<void>(std::string_view, const std::string&)>& set) const;

    // Expands a nested word (the `word` of ${x:-word}) with the outer quoting
    // rules suspended for its own expansions, which are themselves expanded.
    [[nodiscard]] Result<std::string> expand_operand(const std::string& operand, const ExpandContext& context) const;

    [[nodiscard]] Result<std::vector<std::string>> glob_field(const ExpandedFragment& assembled, const std::filesystem::path& base) const;

    // Field splitting plus pathname expansion over the materialized word.
    [[nodiscard]] static Result<std::vector<ExpandedFragment>> assemble_fields(
        std::vector<ExpandedFragment> fragments, const std::string& ifs);

    // Expands one `$...` or `` `...` `` construct inside a parameter operand,
    // advancing `index` past it.
    [[nodiscard]] Result<std::string> parse_operand_expansion(
        std::string_view text, std::size_t& index, const ExpandContext& context) const;

    // Parses the interior of a `${...}` in an operand, including the length,
    // trim, and operator forms.
    [[nodiscard]] Result<std::string> parse_parameter_operand(
        const std::string& body, std::string_view original, const ExpandContext& context) const;

    std::shared_ptr<ScriptingBackend> scripting_ {std::make_shared<NullScriptingBackend>()};
};

// Convenience builders -------------------------------------------------------

inline Argument literal(std::string value) { return Argument::raw(std::move(value)); }

inline Argument single_quoted(std::string value) {
    return Argument {{make_expansion(ExpansionKind::single_quoted, std::move(value))}};
}

inline Argument double_quoted(std::string value) {
    return Argument {{make_expansion(ExpansionKind::double_quoted, std::move(value))}};
}

inline Argument variable(std::string name, bool field_splitting = false) {
    Expansion fragment = make_expansion(ExpansionKind::variable, std::move(name));
    fragment.field_splitting = field_splitting;
    fragment.globbable = field_splitting;
    return Argument {{std::move(fragment)}};
}

// ${NAME OP word} / ${NAME#pattern}. `colon` selects the ':' variant.
inline Argument parameter(std::string name, ParameterOp op, std::string operand = {}, bool colon = false) {
    Expansion fragment = make_expansion(ExpansionKind::variable, std::move(name));
    fragment.op = op;
    fragment.colon = colon;
    fragment.has_operand = op != ParameterOp::none;
    fragment.operand = std::move(operand);
    return Argument {{std::move(fragment)}};
}

inline Argument parameter_trim(std::string name, TrimOp trim, std::string pattern) {
    Expansion fragment = make_expansion(ExpansionKind::variable, std::move(name));
    fragment.trim = trim;
    fragment.has_pattern = true;
    fragment.pattern = std::move(pattern);
    return Argument {{std::move(fragment)}};
}

inline Argument arithmetic(std::string expression) {
    return Argument {{make_expansion(ExpansionKind::arithmetic, std::move(expression))}};
}

inline Argument command_substitution(std::string script, bool field_splitting = true) {
    Expansion fragment = make_expansion(ExpansionKind::command, std::move(script));
    fragment.field_splitting = field_splitting;
    return Argument {{std::move(fragment)}};
}

inline Argument lua(std::string script) {
    return Argument {{make_expansion(ExpansionKind::lua, std::move(script))}};
}

inline Argument glob(std::string pattern) {
    Expansion fragment = make_expansion(ExpansionKind::glob, std::move(pattern));
    fragment.globbable = true;
    return Argument {{std::move(fragment)}};
}

inline Argument tilde(std::string spec) {
    Expansion fragment = make_expansion(ExpansionKind::tilde, std::move(spec));
    fragment.tilde_prefix = true;
    return Argument {{std::move(fragment)}};
}

// Pathname expansion over a bare pattern. Exposed for callers that want POSIX
// glob semantics without constructing an Argument.
[[nodiscard]] inline std::vector<std::string> pathname_expand(std::string_view pattern) {
    return detail::expand_glob(pattern);
}

// ---- expansion implementation ---------------------------------------------

namespace detail {

// Applies a trim operator to `value`. `pattern` is a shell pattern; `*` and
// `?` match, everything else is literal. Returns the trimmed result.
// One field under construction: text plus a per-character mask recording where
// glob metacharacters are still active. Quoted regions are masked off, so
// `a"$b"*` globs with '*' active but never inside the quoted span.
struct FieldBuilder {
    std::string text;
    std::vector<bool> meta;

    void append(std::string_view chunk, bool globbable) {
        text.append(chunk);
        meta.insert(meta.end(), chunk.size(), globbable);
    }
    [[nodiscard]] bool empty() const noexcept { return text.empty(); }
};

[[nodiscard]] inline std::string trim_value(std::string_view value, TrimOp trim, std::string_view pattern) {
    if (pattern.empty()) {
        return std::string(value);
    }
    const auto matches = [pattern](std::string_view subject) {
        return glob_component_matches(subject, pattern, std::vector<bool>(pattern.size(), true));
    };
    const std::string text(value);

    // The shortest form removes the *shortest* matching text, the longest form
    // the longest. Both are a scan over candidate lengths; they differ only in
    // which end of the range wins.
    const bool longest = trim == TrimOp::longest_prefix || trim == TrimOp::longest_suffix;
    const bool prefix = trim == TrimOp::prefix || trim == TrimOp::longest_prefix;
    // A '*' in the pattern matches any run of characters, so a candidate can be
    // as long as the whole value; the scan is bounded by the value, not the
    // pattern.
    const std::size_t limit = text.size();

    std::size_t best = 0;
    bool found = false;
    for (std::size_t length = 1; length <= limit; ++length) {
        const std::string_view candidate =
            prefix ? std::string_view(text).substr(0, length) : std::string_view(text).substr(text.size() - length);
        if (!matches(candidate)) {
            continue;
        }
        if (!found) {
            best = length;
            found = true;
            if (!longest) {
                break; // shortest match wins immediately
            }
        } else {
            best = length; // keep scanning: the longest match is the largest
        }
    }
    if (!found) {
        return text;
    }
    return prefix ? text.substr(best) : text.substr(0, text.size() - best);
}

} // namespace detail

inline Result<std::string> Expander::expand_fragment(const Expansion& expansion, const ExpandContext& context) const {
    switch (expansion.kind) {
    case ExpansionKind::raw:
    case ExpansionKind::single_quoted:
    case ExpansionKind::double_quoted:
        // Tilde expansion applies only to an unquoted word-initial fragment; a
        // quoted '~' is a literal tilde.
        if (expansion.tilde_prefix && expansion.kind == ExpansionKind::raw) {
            return expand_tilde(expansion, context);
        }
        return expansion.text;
    case ExpansionKind::tilde:
        return expand_tilde(expansion, context);
    case ExpansionKind::variable:
        return expand_variable(expansion, context);
    case ExpansionKind::special:
        return expand_special(expansion, context);
    case ExpansionKind::positional: {
        const std::size_t index = static_cast<std::size_t>(std::strtoul(expansion.text.c_str(), nullptr, 10));
        if (index == 0) {
            // $0 is the shell name, not a positional parameter.
            return context.shell_name;
        }
        // The vector holds $1 at index 0; $0 is not stored in it.
        if (!context.positional || index > context.positional->size()) {
            return std::string {};
        }
        return context.positional->at(index - 1);
    }
    case ExpansionKind::arithmetic:
        return expand_arithmetic(expansion, context);
    case ExpansionKind::command:
        return expand_command(expansion, context);
    case ExpansionKind::lua:
        if (scripting_) {
            return scripting_->eval_lua(expansion.text, context.environment ? *context.environment : Environment {});
        }
        return failure(ErrorCode::bad_expansion, "Lua backend is unavailable", expansion.text);
    case ExpansionKind::glob:
        return expansion.text;
    }
    return failure(ErrorCode::bad_expansion, "unknown expansion kind", expansion.text);
}

inline Result<std::string> Expander::expand_special(const Expansion& expansion, const ExpandContext& context) const {
    const std::string& ch = expansion.text;
    if (ch == "?") {
        return std::to_string(context.last_status);
    }
    if (ch == "$") {
        return std::to_string(context.shell_pid);
    }
    if (ch == "!") {
        return std::to_string(context.last_background_pid);
    }
    if (ch == "#") {
        return std::to_string(context.positional ? context.positional->size() : 0);
    }
    if (ch == "-") {
        return context.dash_options;
    }
    if (ch == "0") {
        return context.shell_name;
    }
    if (ch == "@" || ch == "*") {
        // Handled by expand_word as a field boundary; reaching here means the
        // fragment was consumed standalone.
        std::string joined;
        if (context.positional) {
            const std::string ifs = context.ifs();
            const char separator = ifs.empty() ? ' ' : ifs.front();
            for (std::size_t index = 0; index < context.positional->size(); ++index) {
                if (index > 0) {
                    joined.push_back(separator);
                }
                joined += context.positional->at(index);
            }
        }
        return joined;
    }
    return failure(ErrorCode::bad_expansion, "unrecognized special parameter", ch);
}

inline Result<std::string> Expander::expand_tilde(const Expansion& expansion, const ExpandContext& context) const {
    std::string_view spec = expansion.text;
    if (spec.empty()) {
        return std::string {};
    }
    if (context.tilde) {
        auto resolved = context.tilde->resolve(spec);
        if (!resolved) {
            return resolved.error();
        }
        if (resolved.value().has_value()) {
            return *resolved.value();
        }
    }
    if (spec == "~") {
        return context.environment ? context.environment->get("HOME").value_or(std::string {}) : std::string {};
    }
    if (spec == "~+") {
        if (context.environment) {
            if (auto pwd = context.environment->get("PWD"); pwd && !pwd->empty()) {
                return *pwd;
            }
        }
        return context.cwd ? context.cwd->string() : std::string {};
    }
    if (spec == "~-") {
        return context.environment ? context.environment->get("OLDPWD").value_or(std::string {}) : std::string {};
    }
    // "~user" without a resolver: leave literal, per POSIX.
    return std::string(spec);
}

inline Result<std::string> Expander::expand_arithmetic(const Expansion& expansion, const ExpandContext& context) const {
    // Variables referenced by the expression are read from the environment;
    // unset names evaluate to 0, as POSIX requires.
    detail::ArithmeticEvaluator evaluator(expansion.text);
    if (context.environment) {
        evaluator.set_lookup([&context](std::string_view name) -> Result<long long> {
            auto value = context.environment->get(name);
            if (!value || value->empty()) {
                return 0;
            }
            const std::string text = *value;
            // An unset or empty variable is 0. A non-numeric non-empty value is
            // an error rather than a silent zero, so typos surface.
            if (detail::parse_long_long(text, 0)) {
                return std::strtoll(text.c_str(), nullptr, 0);
            }
            return failure(ErrorCode::bad_arithmetic, "non-numeric value in arithmetic expansion", text);
        });
    }
    auto value = evaluator.run();
    if (!value) {
        return value.error();
    }
    return std::to_string(value.value());
}

inline Result<std::string> Expander::expand_command(const Expansion& expansion, const ExpandContext& context) const {
    if (!scripting_) {
        return failure(ErrorCode::bad_expansion, "command substitution backend is unavailable", expansion.text);
    }
    Environment environment;
    if (context.environment) {
        environment = *context.environment;
    }
    return scripting_->eval_command(expansion.text, environment);
}

inline Result<std::string> Expander::apply_parameter(
    const Expansion& expansion,
    const std::string& value,
    const bool is_set,
    const ExpandContext& context,
    const std::function<Result<void>(std::string_view, const std::string&)>& set) const {
    const std::string& name = expansion.text;

    switch (expansion.op) {
    case ParameterOp::none:
        return value;
    case ParameterOp::length: {
        // "${#}" and "${##}" reach here as the positional count; anything else
        // is the byte length of the parameter's value.
        if (name == "#" || name == "##") {
            return std::to_string(context.positional ? context.positional->size() : 0);
        }
        return std::to_string(value.size());
    }
    case ParameterOp::alternate: {
        const bool use_word = expansion.colon ? (value.empty() || !is_set) : !is_set;
        if (!use_word) {
            return value;
        }
        return expand_operand(expansion.operand, context);
    }
    case ParameterOp::assign: {
        const bool assign = expansion.colon ? (value.empty() || !is_set) : !is_set;
        if (!assign) {
            return value;
        }
        auto word = expand_operand(expansion.operand, context);
        if (!word) {
            return word.error();
        }
        // Both forms assign. The colon variant additionally triggers on a
        // variable that is set but empty; the plain form triggers only when the
        // variable is unset, which is exactly the decision made above.
        if (set) {
            if (auto result = set(name, word.value()); !result) {
                return result.error();
            }
        }
        return word.value();
    }
    case ParameterOp::error: {
        const bool fail = expansion.colon ? (value.empty() || !is_set) : !is_set;
        if (!fail) {
            return value;
        }
        std::string message = name + ": parameter ";
        message += expansion.colon ? "null or not set" : "not set";
        if (expansion.has_operand && !expansion.operand.empty()) {
            auto word = expand_operand(expansion.operand, context);
            if (!word) {
                return word.error();
            }
            message += ": " + word.value();
        }
        return failure(ErrorCode::bad_expansion, std::move(message), name);
    }
    case ParameterOp::plus: {
        const bool present = expansion.colon ? (!value.empty() && is_set) : is_set;
        if (!present) {
            return std::string {};
        }
        return expand_operand(expansion.operand, context);
    }
    }
    return value;
}

// Scans a balanced `$(...)`, `` `...` `` or `${...}` starting at `text[index]`.
// Advances `index` past the construct and yields its inner body.
[[nodiscard]] inline Result<std::string> scan_balanced(std::string_view text, std::size_t& index, bool arithmetic) {
    const std::size_t start = index;
    std::string body;
    int depth = 0;
    std::size_t scan = index;
    while (scan < text.size()) {
        const char ch = text[scan];
        if (ch == '\\' && scan + 1 < text.size()) {
            body.push_back(ch);
            body.push_back(text[scan + 1]);
            scan += 2;
            continue;
        }
        if (ch == '\'') {
            const auto close = text.find('\'', scan + 1);
            if (close == std::string::npos) {
                return failure(ErrorCode::bad_expansion, "unterminated single quote in expansion operand");
            }
            body.append(text.substr(scan, close - scan + 1));
            scan = close + 1;
            continue;
        }
        if (ch == '"') {
            const auto close = text.find('"', scan + 1);
            if (close == std::string::npos) {
                return failure(ErrorCode::bad_expansion, "unterminated double quote in expansion operand");
            }
            body.append(text.substr(scan, close - scan + 1));
            scan = close + 1;
            continue;
        }
        if (ch == '`') {
            const auto close = text.find('`', scan + 1);
            if (close == std::string::npos) {
                return failure(ErrorCode::bad_expansion, "unterminated backquote in expansion operand");
            }
            body.append(text.substr(scan, close - scan + 1));
            scan = close + 1;
            continue;
        }
        if (ch == '$' && scan + 1 < text.size() && text[scan + 1] == '(') {
            depth += 1;
            body.append("$(");
            scan += 2;
            continue;
        }
        if (ch == '$' && scan + 1 < text.size() && text[scan + 1] == '{') {
            body.append("${");
            scan += 2;
            continue;
        }
        if (ch == ')' || ch == '}') {
            if (depth == 0) {
                break;
            }
            --depth;
            body.push_back(ch);
            ++scan;
            continue;
        }
        body.push_back(ch);
        ++scan;
    }
    if (index < text.size() && text[index] == '`') {
        if (scan >= text.size()) {
            return failure(ErrorCode::bad_expansion, "unterminated backquote in expansion operand");
        }
        index = scan + 1;
        return body;
    }
    if (arithmetic) {
        if (scan >= text.size() || text[scan] != ')') {
            return failure(ErrorCode::bad_expansion, "unterminated arithmetic in expansion operand");
        }
        // $(( )) -- the outer pair closes with a second ')'.
        if (scan + 1 >= text.size() || text[scan + 1] != ')') {
            return failure(ErrorCode::bad_expansion, "unterminated arithmetic expansion in operand");
        }
        index = scan + 2;
        return body;
    }
    if (scan >= text.size() || (text[scan] != '}' && text[scan] != ')')) {
        return failure(ErrorCode::bad_expansion, "unterminated parameter expansion in operand", std::string(text.substr(start)));
    }
    index = scan + 1;
    return body;
}

inline Result<std::string> Expander::parse_operand_expansion(std::string_view text, std::size_t& index, const ExpandContext& context) const {
    if (text[index] == '`') {
        auto body = scan_balanced(text, index, /*arithmetic=*/false);
        if (!body) {
            return body.error();
        }
        return expand_command(make_expansion(ExpansionKind::command, std::move(body).value()), context);
    }

    // text[index] == '$'
    if (index + 1 >= text.size()) {
        index = text.size();
        return std::string("$");
    }
    const char next = text[index + 1];

    // scan_balanced starts *inside* the construct, so the introducer is consumed
    // here; leaving it in place would make the scanner treat the substitution's
    // own opening as a nested one and run off the end.
    if (next == '(') {
        const bool arithmetic = index + 2 < text.size() && text[index + 2] == '(';
        index += arithmetic ? 3 : 2;
        auto body = scan_balanced(text, index, arithmetic);
        if (!body) {
            return body.error();
        }
        if (arithmetic) {
            return expand_arithmetic(make_expansion(ExpansionKind::arithmetic, std::move(body).value()), context);
        }
        return expand_command(make_expansion(ExpansionKind::command, std::move(body).value()), context);
    }

    if (next == '{') {
        index += 2;
        auto body = scan_balanced(text, index, /*arithmetic=*/false);
        if (!body) {
            return body.error();
        }
        return parse_parameter_operand(body.value(), text, context);
    }

    if (next == '@' || next == '*' || next == '?' || next == '$' || next == '!' || next == '#' || next == '-') {
        const std::string spec(1, next);
        index += 2;
        return expand_special(make_expansion(ExpansionKind::special, spec), context);
    }

    if (std::isdigit(static_cast<unsigned char>(next))) {
        std::size_t end = index + 1;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        const std::string digits(text.substr(index + 1, end - index - 1));
        index = end;
        return expand_fragment(make_expansion(ExpansionKind::positional, digits), context);
    }

    if (std::isalpha(static_cast<unsigned char>(next)) || next == '_') {
        std::size_t end = index + 1;
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
            ++end;
        }
        const std::string name(text.substr(index + 1, end - index - 1));
        index = end;
        return expand_variable(make_expansion(ExpansionKind::variable, name), context);
    }

    // A bare '$' is literal.
    index += 1;
    return std::string("$");
}

inline Result<std::string> Expander::parse_parameter_operand(
    const std::string& body, std::string_view original, const ExpandContext& context) const {
    // `${#name}` is a length; `${#}` and `${##}` are the positional count.
    if (!body.empty() && body.front() == '#' && body.size() > 1) {
        const std::string rest = body.substr(1);
        if (rest == "}") {
            return expand_special(make_expansion(ExpansionKind::special, "#"), context);
        }
    }

    // Locate the end of the parameter name.
    std::size_t cursor = 0;
    std::string name;
    if (cursor < body.size() && body[cursor] == '#') {
        ++cursor;
    }
    if (cursor < body.size() && (std::isdigit(static_cast<unsigned char>(body[cursor])))) {
        while (cursor < body.size() && std::isdigit(static_cast<unsigned char>(body[cursor]))) {
            ++cursor;
        }
        name = body.substr(0, cursor);
        return expand_fragment(make_expansion(ExpansionKind::positional, name.substr(1)), context);
    }
    while (cursor < body.size() && (std::isalnum(static_cast<unsigned char>(body[cursor])) || body[cursor] == '_')) {
        ++cursor;
    }
    name = body.substr(0, cursor);
    if (name.empty()) {
        return failure(ErrorCode::bad_expansion, "parameter expansion without a parameter name", std::string(original));
    }

    Expansion expansion = make_expansion(ExpansionKind::variable, name);

    if (!body.empty() && body.front() == '#' && cursor == 1) {
        // "${#name}" -- length of the parameter.
        expansion.op = ParameterOp::length;
        expansion.colon = true;
        return expand_variable(expansion, context);
    }

    const std::string rest = body.substr(cursor);
    if (rest.empty()) {
        return expand_variable(expansion, context);
    }

    // Trim forms: #, ##, %, %%.
    if (rest == "#" || rest == "##" || rest == "%" || rest == "%%") {
        expansion.trim = rest == "#"    ? TrimOp::prefix
            : rest == "##"              ? TrimOp::longest_prefix
            : rest == "%"                ? TrimOp::suffix
                                         : TrimOp::longest_suffix;
        expansion.has_pattern = true;
        expansion.pattern = rest.substr(rest.size() - 1);
        return expand_variable(expansion, context);
    }

    // Operator forms: the operator is ':' optionally followed by '-', '=', '?',
    // or '+'.
    std::size_t op_cursor = 0;
    bool colon = false;
    if (rest[op_cursor] == ':') {
        colon = true;
        ++op_cursor;
    }
    if (op_cursor >= rest.size()) {
        return failure(ErrorCode::bad_expansion, "parameter expansion operator without an operand", std::string(original));
    }
    const char op = rest[op_cursor];
    if (op == '-') {
        expansion.op = ParameterOp::alternate;
    } else if (op == '=') {
        expansion.op = ParameterOp::assign;
    } else if (op == '?') {
        expansion.op = ParameterOp::error;
    } else if (op == '+') {
        expansion.op = ParameterOp::plus;
    } else {
        return failure(ErrorCode::bad_expansion, "unsupported parameter expansion operator", std::string(original));
    }
    expansion.colon = colon;
    expansion.has_operand = true;
    expansion.operand = rest.substr(op_cursor + 1);
    return expand_variable(expansion, context);
}

inline Result<std::string> Expander::expand_operand(const std::string& operand, const ExpandContext& context) const {
    // The operand of ${x:-word} is itself expanded: tilde, parameter,
    // arithmetic, and command substitution all apply inside it. A dedicated
    // scanner keeps this layer independent of the CLI parser.
    if (operand.empty()) {
        return std::string {};
    }

    std::string out;
    std::size_t index = 0;
    bool word_start = true;

    while (index < operand.size()) {
        const char ch = operand[index];
        if (ch == '\\' && index + 1 < operand.size()) {
            out.push_back(operand[index + 1]);
            index += 2;
            word_start = false;
            continue;
        }
        if (ch == '\'') {
            const auto close = operand.find('\'', index + 1);
            if (close == std::string::npos) {
                return failure(ErrorCode::bad_expansion, "unterminated single quote in expansion operand");
            }
            out.append(operand, index + 1, close - index - 1);
            index = close + 1;
            word_start = false;
            continue;
        }
        if (ch == '"') {
            index += 1;
            while (index < operand.size() && operand[index] != '"') {
                if (operand[index] == '\\' && index + 1 < operand.size()) {
                    out.push_back(operand[index + 1]);
                    index += 2;
                    continue;
                }
                if (operand[index] == '$' || operand[index] == '`') {
                    auto nested = this->parse_operand_expansion(operand, index, context);
                    if (!nested) {
                        return nested.error();
                    }
                    out += nested.value();
                    continue;
                }
                out.push_back(operand[index]);
                index += 1;
            }
            if (index >= operand.size()) {
                return failure(ErrorCode::bad_expansion, "unterminated double quote in expansion operand");
            }
            ++index;
            word_start = false;
            continue;
        }
        if (ch == '$' || ch == '`') {
            auto nested = this->parse_operand_expansion(operand, index, context);
            if (!nested) {
                return nested.error();
            }
            out += nested.value();
            word_start = false;
            continue;
        }
        if (ch == '~' && word_start) {
            const auto stop = operand.find_first_of("/: \t", index);
            const std::string spec = operand.substr(index, stop == std::string::npos ? std::string::npos : stop - index);
            auto expanded = expand_tilde(make_expansion(ExpansionKind::tilde, spec), context);
            if (!expanded) {
                return expanded.error();
            }
            out += expanded.value();
            index += spec.size();
            word_start = false;
            continue;
        }
        out.push_back(ch);
        word_start = (ch == ':');
        index += 1;
    }
    return out;
}

inline Result<std::string> Expander::expand_variable(const Expansion& expansion, const ExpandContext& context) const {
    const std::string& name = expansion.text;
    const bool is_set = context.environment && context.environment->contains(name);
    const std::string value = context.environment ? context.environment->get(name).value_or(std::string {}) : std::string {};

    // Trim forms bypass the operator table.
    if (expansion.trim != TrimOp::none) {
        auto pattern = expand_operand(expansion.pattern, context);
        if (!pattern) {
            return pattern.error();
        }
        return detail::trim_value(value, expansion.trim, pattern.value());
    }

    // ${x=word} and ${x:=word} write back to the environment; "=" additionally
    // requires that the variable currently be unset.
    const auto setter = [&context](std::string_view target, const std::string& new_value) -> Result<void> {
        if (!context.environment) {
            return failure(ErrorCode::bad_expansion, "parameter assignment without an environment", std::string(target));
        }
        return context.environment->assign(std::string(target), new_value);
    };

    return apply_parameter(expansion, value, is_set, context, setter);
}

// Splits a word's materialized fragments into argv fields: field-boundary
// regions contribute whole fields, unquoted regions are split on IFS, and each
// resulting field keeps its glob-active mask for pathname expansion.
inline Result<std::vector<ExpandedFragment>> Expander::assemble_fields(
    std::vector<ExpandedFragment> fragments, const std::string& ifs) {
    std::vector<ExpandedFragment> fields;
    detail::FieldBuilder current;

    auto flush = [&] {
        ExpandedFragment field;
        field.text = std::move(current.text);
        field.meta = std::move(current.meta);
        field.globbable = true;
        fields.push_back(std::move(field));
        current = detail::FieldBuilder {};
    };

    for (const ExpandedFragment& fragment : fragments) {
        if (fragment.field_boundary) {
            // "$@" contributes each element as its own field; quoted "$@"
            // preserves empty elements, unquoted ones are split on IFS.
            for (const std::string& element : fragment.fields) {
                if (!fragment.globbable) {
                    ExpandedFragment field;
                    field.text = element;
                    field.globbable = false;
                    fields.push_back(std::move(field));
                    continue;
                }
                std::vector<std::string> parts;
                detail::split_fields(element, ifs, parts);
                if (parts.empty()) {
                    // An unquoted empty element vanishes entirely.
                    continue;
                }
                for (std::string& part : parts) {
                    ExpandedFragment field;
                    field.text = std::move(part);
                    field.globbable = true;
                    field.meta.assign(field.text.size(), true);
                    fields.push_back(std::move(field));
                }
            }
            continue;
        }
        if (fragment.splittable && !fragment.text.empty()) {
            std::vector<std::string> parts;
            detail::split_fields(fragment.text, ifs, parts);
            if (parts.empty()) {
                // Only separators: the region contributes no field but must not
                // discard the surrounding literal text.
                continue;
            }
            for (std::size_t index = 0; index < parts.size(); ++index) {
                if (index > 0) {
                    flush();
                }
                current.append(parts[index], true);
            }
            continue;
        }
        current.append(fragment.text, fragment.globbable);
    }

    // A word made only of unquoted expansions that produced nothing contributes
    // no field (POSIX removes an unquoted empty expansion). A word with any
    // literal or quoted text always contributes at least one field, even an
    // empty one.
    if (!current.empty()) {
        flush();
    } else if (fields.empty()) {
        bool had_literal = false;
        for (const ExpandedFragment& fragment : fragments) {
            if (!fragment.splittable && !fragment.field_boundary) {
                had_literal = true;
                break;
            }
        }
        if (had_literal || fragments.empty()) {
            flush();
        }
    }
    return fields;
}

inline Result<std::vector<ExpandedFragment>> Expander::expand_word(const Argument& argument, const ExpandContext& context) const {
    std::vector<ExpandedFragment> fragments;
    fragments.reserve(argument.fragments.size());

    for (const Expansion& expansion : argument.fragments) {
        // "$@" and "$*" expand to zero or more complete fields, which the field
        // assembler must not merge with neighbouring literals.
        if (expansion.kind == ExpansionKind::special
            && (expansion.text == "@" || expansion.text == "*")) {
            ExpandedFragment boundary;
            boundary.field_boundary = true;
            if (context.positional) {
                if (expansion.text == "@") {
                    boundary.fields = *context.positional;
                } else {
                    const std::string ifs = context.ifs();
                    std::string joined;
                    for (std::size_t index = 0; index < context.positional->size(); ++index) {
                        if (index > 0 && !ifs.empty()) {
                            joined.push_back(ifs.front());
                        }
                        joined += context.positional->at(index);
                    }
                    boundary.fields.push_back(std::move(joined));
                }
            }
            fragments.push_back(std::move(boundary));
            continue;
        }

        auto expanded = expand_fragment(expansion, context);
        if (!expanded) {
            return expanded.error();
        }
        ExpandedFragment fragment;
        fragment.text = std::move(expanded).value();
        fragment.globbable = expansion.globbable;
        fragment.splittable = expansion.field_splitting;
        if (fragment.globbable) {
            fragment.meta.assign(fragment.text.size(), true);
        }
        fragments.push_back(std::move(fragment));
    }
    return fragments;
}

inline Result<std::vector<std::string>> Expander::glob_field(const ExpandedFragment& assembled, const std::filesystem::path& base) const {
    if (assembled.meta.size() != assembled.text.size()) {
        return std::vector<std::string> {assembled.text};
    }
    if (detail::has_active_glob_meta(assembled.text, assembled.meta)) {
        return detail::expand_glob(assembled.text, assembled.meta, &base);
    }
    return std::vector<std::string> {assembled.text};
}

inline Result<std::vector<std::string>> Expander::expand_argv(const std::vector<Argument>& arguments, const ExpandContext& context) const {
    std::vector<std::string> argv;
    argv.reserve(arguments.size());

    for (const Argument& argument : arguments) {
        auto fragments = expand_word(argument, context);
        if (!fragments) {
            return fragments.error();
        }
        auto fields = assemble_fields(std::move(fragments).value(), context.ifs());
        if (!fields) {
            return fields.error();
        }
        // Pathname expansion is relative to the shell's working directory.
        const std::filesystem::path base = context.cwd != nullptr ? *context.cwd : std::filesystem::path {};
        for (const ExpandedFragment& field : fields.value()) {
            auto expanded = glob_field(field, base);
            if (!expanded) {
                return expanded.error();
            }
            for (std::string& value : expanded.value()) {
                argv.push_back(std::move(value));
            }
        }
    }
    return argv;
}

} // namespace lsh
