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

/// Sign-extends the low `bits` of `value` to a 64-bit two's-complement
/// integer. The wire form stores x and z as 26-bit and y as 12-bit
/// two's-complement fields, so a decoder that does not sign-extend turns every
/// negative coordinate into a large positive one.
[[nodiscard]] constexpr auto sign_extend_bits(std::uint64_t value,
                                              unsigned bits) noexcept
    -> std::int64_t {
    const auto shift{64U - bits};
    return static_cast<std::int64_t>(value << shift) >> shift;
}

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

/// Unpacks the wire form into its three fields. Each field is sign-extended
/// from its wire width, so negative x, y and z decode as negative blocks.
[[nodiscard]] constexpr auto unpack_block_position(std::int64_t packed) noexcept
    -> block_position {
    const auto raw{static_cast<std::uint64_t>(packed)};
    return {.x = static_cast<std::int32_t>(sign_extend_bits(raw >> 38, 26)),
            .y = static_cast<std::int32_t>(sign_extend_bits(raw, 12)),
            .z = static_cast<std::int32_t>(sign_extend_bits(raw >> 12, 26))};
}

} // namespace refrain::net
