// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Token model shared by the RSC and RIDL dialects. Both schema families use
// the same lexer; the parser selects the declaration grammar by dialect.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::schema {

enum class token_kind : std::uint8_t {
    end_of_file,
    identifier,
    doc_comment, ///< `/// ...` line, one token per line
    integer,     ///< decimal or 0x-prefixed
    floating,    ///< `1.0`, `1.0.0`, `1.0.0-rc.1+build.7` (versions)
    string,      ///< "quoted"
    l_brace,
    r_brace,
    l_paren,
    r_paren,
    l_bracket,
    r_bracket,
    l_angle,
    r_angle,
    colon,
    colon_colon,
    semicolon,
    comma,
    equals,
    dot,
    arrow,     ///< `->`
    attribute, ///< `#[`
};

[[nodiscard]] auto token_kind_name(token_kind kind) noexcept
    -> std::string_view;

struct token {
    token_kind kind{token_kind::end_of_file};
    std::string text;
    std::int64_t integer{0};
    std::uint32_t line{1};
    std::uint32_t column{1};
};

} // namespace refrain::schema
