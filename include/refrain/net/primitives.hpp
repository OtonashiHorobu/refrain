// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstddef>
#include <cstdint>

namespace refrain::net {

/// Encoded length of a 32-bit VarInt: seven payload bits per byte, so the
/// 32-bit range ends at five bytes.
[[nodiscard]] constexpr auto varint_size(std::uint32_t value) noexcept
    -> std::size_t {
    std::size_t size{1};
    while (value > 0x7FU) {
        value >>= 7U;
        ++size;
    }
    return size;
}

/// Encoded length of a 64-bit VarLong: the 64-bit range ends at ten bytes.
[[nodiscard]] constexpr auto varlong_size(std::uint64_t value) noexcept
    -> std::size_t {
    std::size_t size{1};
    while (value > 0x7FULL) {
        value >>= 7U;
        ++size;
    }
    return size;
}

/// A block position in the packed 64-bit wire form: 26 bits of x, 26 bits of
/// z and 12 bits of y, most significant field first.
struct block_position {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t z{0};

    friend auto operator==(const block_position &, const block_position &)
        -> bool = default;
};

/// Packs coordinates into the wire form. Only the low 26 bits of x and z and
/// the low 12 bits of y can be carried; the wire layout keeps the packed value
/// identical to the vanilla encoding.
[[nodiscard]] constexpr auto pack_block_position(std::int32_t x, std::int32_t y,
                                                 std::int32_t z) noexcept
    -> std::int64_t {
    return ((static_cast<std::int64_t>(x) & 0x3FFFFFF) << 38) |
           ((static_cast<std::int64_t>(z) & 0x3FFFFFF) << 12) |
           (static_cast<std::int64_t>(y) & 0xFFF);
}

/// Unpacks the wire form into its three fields.
[[nodiscard]] constexpr auto unpack_block_position(std::int64_t packed) noexcept
    -> block_position {
    return {.x = static_cast<std::int32_t>((packed >> 38) & 0x3FFFFFF),
            .y = static_cast<std::int32_t>(packed & 0xFFF),
            .z = static_cast<std::int32_t>((packed >> 12) & 0x3FFFFFF)};
}

} // namespace refrain::net
