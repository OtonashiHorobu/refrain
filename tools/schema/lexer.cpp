// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/lexer.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::schema {

namespace {

[[nodiscard]] constexpr auto is_identifier_start(char value) noexcept -> bool {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           value == '_';
}

[[nodiscard]] constexpr auto is_identifier_continue(char value) noexcept
    -> bool {
    return is_identifier_start(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] constexpr auto is_digit(char value) noexcept -> bool {
    return value >= '0' && value <= '9';
}

/// Characters allowed in a SemVer prerelease/build suffix
/// (`1.2.3-rc.1+build.7`). The suffix starts after the numeric
/// `major.minor.patch` core; the semver validator rejects malformed
/// identifiers, so the lexer can be permissive and keep one token per version
/// (otherwise `-` would be a lexer error and the parser could not report a
/// readable "invalid version" diagnostic).
[[nodiscard]] constexpr auto is_version_suffix_char(char value) noexcept
    -> bool {
    return is_digit(value) || (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '-' || value == '.' ||
           value == '+';
}

[[nodiscard]] constexpr auto is_whitespace(char value) noexcept -> bool {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

} // namespace

auto token_kind_name(token_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case token_kind::end_of_file:
        return "end of file";
    case token_kind::identifier:
        return "identifier";
    case token_kind::doc_comment:
        return "documentation comment";
    case token_kind::integer:
        return "integer";
    case token_kind::floating:
        return "number";
    case token_kind::string:
        return "string";
    case token_kind::l_brace:
        return "'{'";
    case token_kind::r_brace:
        return "'}'";
    case token_kind::l_paren:
        return "'('";
    case token_kind::r_paren:
        return "')'";
    case token_kind::l_bracket:
        return "'['";
    case token_kind::r_bracket:
        return "']'";
    case token_kind::l_angle:
        return "'<'";
    case token_kind::r_angle:
        return "'>'";
    case token_kind::colon:
        return "':'";
    case token_kind::colon_colon:
        return "'::'";
    case token_kind::semicolon:
        return "';'";
    case token_kind::comma:
        return "','";
    case token_kind::equals:
        return "'='";
    case token_kind::dot:
        return "'.'";
    case token_kind::arrow:
        return "'->'";
    case token_kind::attribute:
        return "'#['";
    }
    return "token";
}

lexer::lexer(std::string_view source, std::string file, diagnostics &sink)
    : source_{source}, file_{std::move(file)}, diagnostics_{sink} {}

auto lexer::at_end() const noexcept -> bool {
    return position_ >= source_.size();
}

auto lexer::peek(std::size_t offset) const noexcept -> char {
    const auto index{position_ + offset};
    return index < source_.size() ? source_[index] : '\0';
}

auto lexer::advance() -> char {
    const auto value{source_[position_]};
    ++position_;
    if (value == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return value;
}

void lexer::skip_whitespace_and_comments() {
    for (;;) {
        if (at_end()) {
            return;
        }
        const auto current{peek()};
        if (is_whitespace(current)) {
            advance();
            continue;
        }
        if (current == '/' && peek(1) == '/' && peek(2) != '/') {
            // Plain line comment: skip to (but not past) the newline.
            while (!at_end() && peek() != '\n') {
                advance();
            }
            continue;
        }
        if (current == '/' && peek(1) == '*') {
            const auto start_line{line_};
            const auto start_column{column_};
            advance();
            advance();
            while (!at_end() && !(peek() == '*' && peek(1) == '/')) {
                advance();
            }
            if (at_end()) {
                diagnostics_.error({file_, start_line, start_column},
                                   "unterminated block comment");
                return;
            }
            advance();
            advance();
            continue;
        }
        return;
    }
}

auto lexer::next() -> token {
    for (;;) {
        skip_whitespace_and_comments();
        if (at_end()) {
            return token{token_kind::end_of_file, {}, 0, line_, column_};
        }

        const auto current{peek()};
        if (is_identifier_start(current)) {
            return lex_identifier(current);
        }
        if (is_digit(current)) {
            return lex_number();
        }
        if (current == '"') {
            return lex_string();
        }
        if (current == '/' && peek(1) == '/' && peek(2) == '/') {
            return lex_doc_comment();
        }

        const auto start_line{line_};
        const auto start_column{column_};
        const auto value{advance()};
        switch (value) {
        case '{':
            return token{token_kind::l_brace, "{", 0, start_line, start_column};
        case '}':
            return token{token_kind::r_brace, "}", 0, start_line, start_column};
        case '(':
            return token{token_kind::l_paren, "(", 0, start_line, start_column};
        case ')':
            return token{token_kind::r_paren, ")", 0, start_line, start_column};
        case '[':
            return token{token_kind::l_bracket, "[", 0, start_line,
                         start_column};
        case ']':
            return token{token_kind::r_bracket, "]", 0, start_line,
                         start_column};
        case '<':
            return token{token_kind::l_angle, "<", 0, start_line, start_column};
        case '>':
            return token{token_kind::r_angle, ">", 0, start_line, start_column};
        case ';':
            return token{token_kind::semicolon, ";", 0, start_line,
                         start_column};
        case ',':
            return token{token_kind::comma, ",", 0, start_line, start_column};
        case '=':
            return token{token_kind::equals, "=", 0, start_line, start_column};
        case '.':
            return token{token_kind::dot, ".", 0, start_line, start_column};
        case ':':
            if (!at_end() && peek() == ':') {
                advance();
                return token{token_kind::colon_colon, "::", 0, start_line,
                             start_column};
            }
            return token{token_kind::colon, ":", 0, start_line, start_column};
        case '-':
            if (!at_end() && peek() == '>') {
                advance();
                return token{token_kind::arrow, "->", 0, start_line,
                             start_column};
            }
            diagnostics_.error({file_, start_line, start_column},
                               "unexpected character '-' (did you mean '->'?)");
            break;
        case '#':
            if (!at_end() && peek() == '[') {
                advance();
                return token{token_kind::attribute, "#[", 0, start_line,
                             start_column};
            }
            diagnostics_.error({file_, start_line, start_column},
                               "unexpected character '#' (attributes start "
                               "with '#[')");
            break;
        default:
            diagnostics_.error({file_, start_line, start_column},
                               std::format("unexpected character '{}'", value));
            break;
        }
    }
}

auto lexer::lex_identifier(char first) -> token {
    const auto start_line{line_};
    const auto start_column{column_};
    auto text{std::string{}};
    text += first;
    advance();
    while (!at_end() && is_identifier_continue(peek())) {
        text += advance();
    }
    return token{token_kind::identifier, std::move(text), 0, start_line,
                 start_column};
}

auto lexer::lex_number() -> token {
    const auto start_line{line_};
    const auto start_column{column_};
    auto text{std::string{}};
    bool hexadecimal{false};

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        hexadecimal = true;
        text += advance();
        text += advance();
        while (!at_end() && ((peek() >= '0' && peek() <= '9') ||
                             (peek() >= 'a' && peek() <= 'f') ||
                             (peek() >= 'A' && peek() <= 'F'))) {
            text += advance();
        }
        if (text.size() == 2) { // "0x" with no digits is not an overflow
            diagnostics_.error({file_, start_line, start_column},
                               std::format("invalid hexadecimal literal '{}' "
                                           "(missing digits after the prefix)",
                                           text));
            return token{token_kind::integer, std::move(text), 0, start_line,
                         start_column};
        }
    } else {
        while (!at_end() && is_digit(peek())) {
            text += advance();
        }
        // A dotted numeric token is a version (`1.0`, `1.0.0`): keep every
        // `.<digits>` group in one token so a full SemVer triple is a single
        // token. A trailing `-prerelease` / `+build` suffix is appended too.
        bool dotted{false};
        while (!at_end() && peek() == '.' && is_digit(peek(1))) {
            dotted = true;
            text += advance(); // '.'
            while (!at_end() && is_digit(peek())) {
                text += advance();
            }
        }
        if (dotted) {
            if (!at_end() && (peek() == '-' || peek() == '+')) {
                while (!at_end() && is_version_suffix_char(peek())) {
                    text += advance();
                }
            }
            return token{token_kind::floating, std::move(text), 0, start_line,
                         start_column};
        }
    }

    std::int64_t value{0};
    auto begin{text.data()};
    auto end{text.data() + text.size()};
    if (hexadecimal) {
        begin += 2;
    }
    const auto [parse_end, parse_error]{
        std::from_chars(begin, end, value, hexadecimal ? 16 : 10)};
    if (parse_error != std::errc{} || parse_end != end) {
        diagnostics_.error(
            {file_, start_line, start_column},
            std::format("integer '{}' is out of range for a 64-bit value",
                        text));
        value = 0;
    }
    return token{token_kind::integer, std::move(text), value, start_line,
                 start_column};
}

auto lexer::lex_string() -> token {
    const auto start_line{line_};
    const auto start_column{column_};
    advance(); // opening quote
    auto text{std::string{}};
    bool terminated{false};
    while (!at_end()) {
        if (peek() == '"') {
            advance(); // closing quote
            terminated = true;
            break;
        }
        if (peek() == '\n' || peek() == '\r') {
            // A raw newline would be injected verbatim into generated `//`
            // comments and escape the comment. Only the `\n` escape may put a
            // newline into an attribute string.
            diagnostics_.error({file_, start_line, start_column},
                               "string literal must not contain a raw newline "
                               "(use the '\\n' escape)");
            break;
        }
        auto value{advance()};
        if (value == '\\') {
            if (at_end()) {
                break;
            }
            const auto escaped{advance()};
            switch (escaped) {
            case '\\':
                text += '\\';
                break;
            case '"':
                text += '"';
                break;
            case 'n':
                text += '\n';
                break;
            case 't':
                text += '\t';
                break;
            case 'r':
                text += '\r';
                break;
            default:
                diagnostics_.error(
                    {file_, line_, column_},
                    std::format("invalid escape sequence '\\{}'", escaped));
                text += escaped;
                break;
            }
            continue;
        }
        text += value;
    }
    if (!terminated) {
        if (at_end()) {
            diagnostics_.error({file_, start_line, start_column},
                               "unterminated string literal");
        }
    }
    return token{token_kind::string, std::move(text), 0, start_line,
                 start_column};
}

auto lexer::lex_doc_comment() -> token {
    const auto start_line{line_};
    const auto start_column{column_};
    advance();
    advance();
    advance();
    if (!at_end() && peek() == ' ') {
        advance();
    }
    auto text{std::string{}};
    while (!at_end() && peek() != '\n') {
        text += advance();
    }
    return token{token_kind::doc_comment, std::move(text), 0, start_line,
                 start_column};
}

} // namespace refrain::schema
