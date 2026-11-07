// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/core/md5.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace refrain::worldgen {
namespace {

constexpr std::array<std::uint32_t, 64> k_shift{
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

constexpr std::array<std::uint32_t, 64> k_table{
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU,
    0x4787c62aU, 0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU,
    0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU,
    0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
    0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U,
    0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
    0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
    0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
    0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U,
    0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U, 0x432aff97U,
    0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU,
    0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U,
};

constexpr auto rotl(std::uint32_t value, std::uint32_t bits) noexcept
    -> std::uint32_t {
    return (value << bits) | (value >> (32U - bits));
}

auto load_le32(const std::uint8_t *p) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

void process_block(const std::uint8_t *block,
                   std::array<std::uint32_t, 4> &state) noexcept {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = load_le32(block + (i * 4));
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];

    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        std::uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = ((5U * i) + 1U) % 16U;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = ((3U * i) + 5U) % 16U;
        } else {
            f = c ^ (b | ~d);
            g = (7U * i) % 16U;
        }

        f += a + k_table[i] + words[g];
        a = d;
        d = c;
        c = b;
        b += rotl(f, k_shift[i]);
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace

auto md5(std::span<const std::uint8_t> data) noexcept
    -> std::array<std::uint8_t, 16> {
    std::array<std::uint32_t, 4> state{0x67452301U, 0xefcdab89U, 0x98badcfeU,
                                       0x10325476U};

    const std::size_t full_blocks = data.size() / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        process_block(data.data() + (i * 64), state);
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t remainder = data.size() - (full_blocks * 64);
    if (remainder != 0) {
        std::memcpy(tail.data(), data.data() + (full_blocks * 64), remainder);
    }
    tail[remainder] = 0x80U;

    const std::size_t tail_length = remainder < 56 ? 64 : 128;
    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tail_length - 8 + i] =
            static_cast<std::uint8_t>((bit_length >> (8U * i)) & 0xFFU);
    }

    for (std::size_t offset = 0; offset < tail_length; offset += 64) {
        process_block(tail.data() + offset, state);
    }

    std::array<std::uint8_t, 16> digest{};
    for (std::size_t i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] & 0xFFU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xFFU);
        digest[i * 4 + 2] =
            static_cast<std::uint8_t>((state[i] >> 16U) & 0xFFU);
        digest[i * 4 + 3] =
            static_cast<std::uint8_t>((state[i] >> 24U) & 0xFFU);
    }
    return digest;
}

auto md5(std::string_view text) noexcept -> std::array<std::uint8_t, 16> {
    return md5(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

} // namespace refrain::worldgen
