// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace refrain::core::hex {

/// 16 lowercase hex digits of a 64-bit id, most significant nibble first.
/// This is the sealed-segment file-name convention shared by RFSG and RFSI.
[[nodiscard]] inline auto hex_u64(std::uint64_t value) -> std::string {
    constexpr std::size_t hex_chars{16};
    constexpr char digits[]{"0123456789abcdef"};
    std::string text(hex_chars, '0');
    for (std::size_t index{0}; index < hex_chars; ++index) {
        const auto shift{4U * (hex_chars - 1U - index)};
        text[index] = digits[(value >> shift) & 0xFU];
    }
    return text;
}

} // namespace refrain::core::hex
