// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Line-oriented output buffer shared by the schema emitters. Formatting writes
// directly into the backing string (`std::format_to`) so emitting a generated
// line does not allocate a temporary `std::string` that is then copied into the
// buffer.

#pragma once

#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::schema {

class code_writer {
  public:
    /// Append one formatted line. Every active `indent()` level contributes
    /// two leading spaces; existing emitters that hard-code their own leading
    /// spaces simply never call `indent()`.
    template <typename... Args>
    void line(std::format_string<Args...> fmt, Args &&...args) {
        buf_.append(indent_ * 2, ' ');
        std::format_to(std::back_inserter(buf_), fmt,
                       std::forward<Args>(args)...);
        buf_ += '\n';
    }

    /// Append text verbatim: no formatting, no indentation, no newline.
    void raw(std::string_view text) { buf_ += text; }

    /// Append one empty line (a bare '\n').
    void blank() { buf_ += '\n'; }

    void indent() { ++indent_; }

    /// Remove one indentation level; a no-op at level zero.
    void dedent() {
        if (indent_ != 0) {
            --indent_;
        }
    }

    /// Return the accumulated output and leave the writer empty.
    [[nodiscard]] auto take() -> std::string { return std::move(buf_); }

  private:
    std::string buf_{};
    std::size_t indent_{0};
};

} // namespace refrain::schema
