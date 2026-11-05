// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/checksum.hpp>

#include <blake3.h>
#include <crc32c/crc32c.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace refrain::data {
namespace {

[[nodiscard]] constexpr auto hex_nibble(char character) noexcept -> int {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

auto to_hex(const digest32 &digest) -> std::string {
    return to_hex(std::span<const std::byte>{digest.data(), digest.size()});
}

auto to_hex(std::span<const std::byte> bytes) -> std::string {
    constexpr char digits[]{"0123456789abcdef"};
    std::string text;
    text.resize(bytes.size() * 2U);
    for (std::size_t index{0}; index < bytes.size(); ++index) {
        const auto byte{std::to_integer<unsigned int>(bytes[index])};
        text[2U * index] = digits[(byte >> 4U) & 0xFU];
        text[2U * index + 1U] = digits[byte & 0xFU];
    }
    return text;
}

auto parse_digest(std::string_view hex) -> result<digest32> {
    if (hex.size() != digest32{}.size() * 2U) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "BLAKE3 digest must be 32 bytes (64 hexadecimal characters)")};
    }

    digest32 digest{};
    for (std::size_t index{0}; index < digest.size(); ++index) {
        const int high{hex_nibble(hex[2U * index])};
        const int low{hex_nibble(hex[2U * index + 1U])};
        if (high < 0 || low < 0) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                "BLAKE3 digest must be hexadecimal characters only")};
        }
        digest[index] = static_cast<std::byte>((high << 4) | low);
    }
    return digest;
}

auto blake3(std::span<const std::byte> data) noexcept -> digest32 {
    blake3_hasher hasher{};
    blake3_hasher_init(&hasher);
    if (!data.empty()) {
        blake3_hasher_update(&hasher, data.data(), data.size());
    }
    digest32 digest{};
    blake3_hasher_finalize(&hasher,
                           reinterpret_cast<std::uint8_t *>(digest.data()),
                           digest.size());
    return digest;
}

auto crc32c_update(std::uint32_t state,
                   std::span<const std::byte> data) noexcept -> std::uint32_t {
    // CRC32C (Castagnoli, reflected 0x82F63B78) is delegated to google/crc32c
    // (CCI `crc32c/1.1.2`, BSD-3-Clause), which selects a hardware path
    // (SSE4.2/AVX-512 `crc32` instructions or ARMv8 CRC32) at runtime and falls
    // back to a portable software implementation. This replaces a
    // hand-written 256-entry table.
    // The library stays an implementation detail of `refrain-data`;
    // `<crc32c/crc32c.h>` is included here and never from `checksum.hpp`.
    //
    // `state` is a finalized CRC32C value (as returned by crc32c/this function)
    // and 0 denotes "no bytes consumed yet" (CRC32C of the empty input is also
    // 0, so the two cases coincide). `crc32c::Extend` uses exactly that
    // finalized-value convention: it un-complements the state, runs the
    // reflected register with a 0xFFFFFFFF initial value, and complements the
    // result, matching the published Castagnoli/RFC 3720 vectors.
    if (data.empty()) {
        return state;
    }
    return crc32c::Extend(state,
                          reinterpret_cast<const std::uint8_t *>(data.data()),
                          data.size());
}

auto crc32c(std::span<const std::byte> data) noexcept -> std::uint32_t {
    return crc32c_update(0, data);
}

} // namespace refrain::data
