// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

// Shared helpers for the split discovery translation units (scan vs lock).
// Keep these inline and internal to `src/mod`; they are not public API.

#include <refrain/core/error.hpp>
#include <refrain/mod/discovery.hpp>

#include <fmt/format.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::mod::detail {

[[nodiscard]] auto
resolve_catalog(const mod::gamepack_spec &spec,
                std::span<const discovered_mod> discovered,
                const discover_options &options,
                std::span<const std::string_view> required_services,
                bool preview) -> result<composition>;

[[nodiscard]] auto
resolve_catalog(const mod::gamepack_spec &spec,
                std::span<const discovered_mod> discovered,
                const discover_options &options,
                std::span<const std::string_view> required_services,
                bool preview) -> result<composition>;

[[nodiscard]] inline auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

[[nodiscard]] inline auto annotate(error original, std::string_view context)
    -> error {
    if (original.message.find(context) != std::string::npos) {
        return original;
    }
    original.message = std::string{context} + ": " + original.message;
    return original;
}

[[nodiscard]] inline auto lower_id(std::string_view id) -> std::string {
    std::string lower;
    lower.reserve(id.size());
    for (const char character : id) {
        if (character >= 'A' && character <= 'Z') {
            lower.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            lower.push_back(character);
        }
    }
    return lower;
}

[[nodiscard]] inline auto toml_escape(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size() + 8U);
    for (const char raw : text) {
        const auto character{static_cast<unsigned char>(raw)};
        switch (character) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (character < 0x20U || character == 0x7FU) {
                out += fmt::format("\\u{:04X}",
                                   static_cast<unsigned int>(character));
            } else {
                out.push_back(raw);
            }
            break;
        }
    }
    return out;
}

[[nodiscard]] inline auto toml_string(std::string_view text) -> std::string {
    return fmt::format("\"{}\"", toml_escape(text));
}

[[nodiscard]] inline auto comment_text(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto character{static_cast<unsigned char>(raw)};
        out.push_back(character < 0x20U || character == 0x7FU ? ' ' : raw);
    }
    return out;
}

} // namespace refrain::mod::detail
