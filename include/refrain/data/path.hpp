// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::data {

// VFS path helpers shared by the pack containers and the layered VFS. Header-
// only: they are small, allocation-light predicates/manipulations that never
// touch the OS, so the same string identifies the same resource on every
// platform. Non-ASCII names are preserved verbatim; `is_valid_utf8()`
// is the well-formedness predicate for containers that spell UTF-8 as part of
// their contract (mod packages validate both member names and manifest keys).
//
// Virtual paths are UTF-8, '/'-only archive-relative strings: no drive letters,
// no leading '/', no '\', no control bytes, no empty/"."/".." segments and no
// trailing '/'. The container contracts (RFSI image, RFBT/RFRX, RFSG) are
// implemented in terms of this header. Zip packs additionally apply the
// stricter portable-name rules of
// `refrain::archive::safe_relative_path()` when they are opened, because
// zip content can be extracted to a host filesystem. `is_safe_relative_path()`
// itself is byte-oriented and does not check UTF-8; callers that require
// UTF-8 names combine it with `is_valid_utf8()`.

/// Join two virtual path fragments with exactly one '/'.
///
/// Empty fragments are skipped (`join_path("", "data") == "data"`,
/// `join_path("data", "") == "data"`); a leading '/' is never added. The
/// arguments are not normalized: callers pass already-safe relative fragments.
[[nodiscard]] inline auto join_path(std::string_view base,
                                    std::string_view name) -> std::string {
    if (base.empty()) {
        return std::string{name};
    }
    if (name.empty()) {
        return std::string{base};
    }
    std::string joined{base};
    if (joined.back() != '/') {
        joined.push_back('/');
    }
    joined.append(name);
    return joined;
}

/// Parent of `path`: everything before the last '/'. Returns an empty view for
/// a single-segment path (and for the empty path). The view points into
/// `path`, so it must not outlive the owning string.
[[nodiscard]] inline auto parent_path(std::string_view path) noexcept
    -> std::string_view {
    const auto slash{path.rfind('/')};
    return slash == std::string_view::npos ? std::string_view{}
                                           : path.substr(0, slash);
}

/// Final segment of `path` (everything after the last '/'). Single-segment
/// paths are returned unchanged. The view points into `path`, so it must not
/// outlive the owning string.
[[nodiscard]] inline auto filename(std::string_view path) noexcept
    -> std::string_view {
    const auto slash{path.rfind('/')};
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// True when `text` is well-formed UTF-8 (RFC 3629): ASCII fast path, 2-4
/// byte sequences only, no overlong encodings, no UTF-16 surrogate code points
/// (U+D800..U+DFFF), nothing above U+10FFFF, and no truncated sequence or
/// invalid/stray continuation byte.
[[nodiscard]] inline auto is_valid_utf8(std::string_view text) noexcept
    -> bool {
    std::size_t index{0};
    while (index < text.size()) {
        const auto lead{static_cast<unsigned char>(text[index])};
        if (lead < 0x80U) {
            ++index;
            continue;
        }
        std::size_t length{0};
        std::uint32_t code_point{0};
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = lead & 0x07U;
        } else {
            return false; // stray continuation byte or 0xF8..0xFF lead
        }
        if (text.size() - index < length) {
            return false; // truncated sequence
        }
        for (std::size_t offset{1}; offset < length; ++offset) {
            const auto continuation{
                static_cast<unsigned char>(text[index + offset])};
            if ((continuation & 0xC0U) != 0x80U) {
                return false; // invalid continuation byte
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        // The smallest code point representable in this many bytes; anything
        // below it is an overlong encoding (e.g. C0 80 for U+0000).
        constexpr std::array<std::uint32_t, 5> minimum{
            {0, 0, 0x80, 0x800, 0x10000}};
        if (code_point < minimum[length] ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU) ||
            code_point > 0x10FFFFU) {
            return false;
        }
        index += length;
    }
    return true;
}

/// True when `path` is a safe archive-relative virtual path: non-empty,
/// '/'-separated, no leading or trailing '/', no '\\' or control bytes
/// (0x00-0x1F, 0x7F), and no empty/'.'/'..' segments. Non-ASCII bytes are
/// allowed (UTF-8 names are opaque and preserved).
[[nodiscard]] inline auto is_safe_relative_path(std::string_view path) noexcept
    -> bool {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        return false;
    }
    for (const char character : path) {
        const auto byte{static_cast<unsigned char>(character)};
        if (character == '\\' || byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }

    std::size_t start{0};
    while (start < path.size()) {
        const auto slash{path.find('/', start)};
        const auto length{
            (slash == std::string_view::npos ? path.size() : slash) - start};
        const auto segment{path.substr(start, length)};
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

} // namespace refrain::data
