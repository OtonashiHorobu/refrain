// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace refrain::command {

/// Byte-oriented cursor over one command line.
///
/// Every position reported by the command module is a byte offset into the
/// original input, matching the offsets used by the network protocol. The
/// reader never throws and never reads outside its view: `peek()` and `read()`
/// return '\0' (and `read()` does not advance) when the cursor is at the end.
class reader {
  public:
    explicit reader(std::string_view input) noexcept;
    reader(std::string_view input, std::size_t cursor) noexcept;

    [[nodiscard]] auto input() const noexcept -> std::string_view;
    [[nodiscard]] auto cursor() const noexcept -> std::size_t;
    [[nodiscard]] auto remaining() const noexcept -> std::size_t;
    [[nodiscard]] auto can_read() const noexcept -> bool;
    [[nodiscard]] auto at_end() const noexcept -> bool;
    [[nodiscard]] auto peek() const noexcept -> char;
    [[nodiscard]] auto read() noexcept -> char;
    void skip() noexcept;
    void set_cursor(std::size_t cursor) noexcept;
    void skip_whitespace() noexcept;
    [[nodiscard]] auto at_whitespace() const noexcept -> bool;

    /// Consume the next non-whitespace run. Returns an empty view at the end.
    [[nodiscard]] auto read_word() noexcept -> std::string_view;

    /// Read a single- or double-quoted string, decoding backslash escapes for
    /// the quote character and for backslash itself. The cursor must be on the
    /// opening quote; other callers use `read_word()`. An unterminated string
    /// is a parse error at the opening quote and leaves the cursor at the end
    /// of the input.
    [[nodiscard]] auto read_quoted() -> result<std::string>;

    /// Consume the rest of the input verbatim and advance to the end.
    [[nodiscard]] auto read_remaining() noexcept -> std::string_view;

    /// Command whitespace: space and tab. Newlines never appear in a command
    /// packet, but treating them as ordinary bytes keeps the grammar explicit.
    [[nodiscard]] static auto is_whitespace(char character) noexcept -> bool;

  private:
    std::string_view input_{};
    std::size_t cursor_{0};
};

} // namespace refrain::command
