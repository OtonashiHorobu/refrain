// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Hand-written lexer for RSC/RIDL. Deliberately small (no third-party parser
// toolkit) because the grammar is fixed, the tool ships in the engine binary
// and the diagnostics must point at schema source locations.

#pragma once

#include <schema/diagnostic.hpp>
#include <schema/token.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::schema {

class lexer {
  public:
    lexer(std::string_view source, std::string file, diagnostics &sink);

    lexer(const lexer &) = delete;
    lexer &operator=(const lexer &) = delete;

    lexer(lexer &&) = delete;
    lexer &operator=(lexer &&) = delete;

    ~lexer() = default;

    /// Next token; returns `end_of_file` forever after the source is consumed.
    [[nodiscard]] auto next() -> token;

  private:
    [[nodiscard]] auto at_end() const noexcept -> bool;
    [[nodiscard]] auto peek(std::size_t offset = 0) const noexcept -> char;
    auto advance() -> char;
    void skip_whitespace_and_comments();
    [[nodiscard]] auto make_token(token_kind kind, std::string text) -> token;
    [[nodiscard]] auto lex_identifier(char first) -> token;
    [[nodiscard]] auto lex_number() -> token;
    [[nodiscard]] auto lex_string() -> token;
    [[nodiscard]] auto lex_doc_comment() -> token;

    std::string_view source_;
    std::string file_;
    diagnostics &diagnostics_;
    std::size_t position_{0};
    std::uint32_t line_{1};
    std::uint32_t column_{1};
};

} // namespace refrain::schema
