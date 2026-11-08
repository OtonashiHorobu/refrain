// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/reader.hpp>

#include <algorithm>

namespace refrain::command {

reader::reader(std::string_view input) noexcept : input_{input} {}

reader::reader(std::string_view input, std::size_t cursor) noexcept
    : input_{input}, cursor_{std::min(cursor, input.size())} {}

auto reader::input() const noexcept -> std::string_view { return input_; }

auto reader::cursor() const noexcept -> std::size_t { return cursor_; }

auto reader::remaining() const noexcept -> std::size_t {
    return input_.size() - cursor_;
}

auto reader::can_read() const noexcept -> bool {
    return cursor_ < input_.size();
}

auto reader::at_end() const noexcept -> bool { return !can_read(); }

auto reader::peek() const noexcept -> char {
    return can_read() ? input_[cursor_] : '\0';
}

auto reader::read() noexcept -> char {
    if (!can_read()) {
        return '\0';
    }
    return input_[cursor_++];
}

void reader::skip() noexcept {
    if (can_read()) {
        ++cursor_;
    }
}

void reader::set_cursor(std::size_t cursor) noexcept {
    cursor_ = std::min(cursor, input_.size());
}

void reader::skip_whitespace() noexcept {
    while (can_read() && is_whitespace(peek())) {
        ++cursor_;
    }
}

auto reader::at_whitespace() const noexcept -> bool {
    return !can_read() || is_whitespace(peek());
}

auto reader::read_word() noexcept -> std::string_view {
    const auto start{cursor_};
    while (can_read() && !is_whitespace(peek())) {
        ++cursor_;
    }
    return input_.substr(start, cursor_ - start);
}

auto reader::read_quoted() -> result<std::string> {
    if (!can_read() || (peek() != '"' && peek() != '\'')) {
        return std::unexpected{
            make_error(error_code::parse_error, "expected a quoted string")};
    }
    const auto quote{read()};
    std::string out;
    out.reserve(remaining());
    while (can_read()) {
        const auto character{read()};
        if (character == quote) {
            return out;
        }
        if (character != '\\') {
            out.push_back(character);
            continue;
        }
        if (!can_read()) {
            break;
        }
        const auto escaped{read()};
        if (escaped == quote || escaped == '\\') {
            out.push_back(escaped);
        } else {
            // Only the quote and backslash are escapes; every other backslash
            // sequence is preserved verbatim.
            out.push_back('\\');
            out.push_back(escaped);
        }
    }
    return std::unexpected{
        make_error(error_code::parse_error, "unterminated quoted string")};
}

auto reader::read_remaining() noexcept -> std::string_view {
    const auto rest{input_.substr(cursor_)};
    cursor_ = input_.size();
    return rest;
}

auto reader::is_whitespace(char character) noexcept -> bool {
    return character == ' ' || character == '\t';
}

} // namespace refrain::command
