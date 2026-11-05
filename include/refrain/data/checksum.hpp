// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace refrain::data {

// Small, dependency-hiding checksum helpers shared by the container, VFS and
// content-store code. The implementations live in checksum.cpp so public
// headers never leak BLAKE3/zlib/crc32c headers.
//
// * BLAKE3 is the content hash of record for `.rfmod`, the content-addressed
//   blob store and the built-in RFSI image. 32-byte output, unkeyed.
// * CRC32C (Castagnoli, reflected 0x82F63B78) is the fast corruption check
//   inside container headers (RFSI/RFBT/RFSG/RFRX) and in RWF records. It is
//   delegated to google/crc32c (`Crc32c::crc32c`, hardware-accelerated with a
//   software fallback) instead of a hand-written table; this header does not
//   include the crc32c library header.

/// A 32-byte BLAKE3 digest.
using digest32 = std::array<std::byte, 32>;

/// Lowercase hex representation (64 characters) of a digest.
[[nodiscard]] auto to_hex(const digest32 &digest) -> std::string;

/// Lowercase hex representation of arbitrary bytes (2 characters per byte).
[[nodiscard]] auto to_hex(std::span<const std::byte> bytes) -> std::string;

/// Parse a 64-character lowercase/uppercase hex string into a digest. Fails
/// with `invalid_argument` for wrong length or non-hex characters.
[[nodiscard]] auto parse_digest(std::string_view hex) -> result<digest32>;

/// Unkeyed BLAKE3 of `data`.
[[nodiscard]] auto blake3(std::span<const std::byte> data) noexcept -> digest32;

/// CRC32C (Castagnoli) of `data`.
[[nodiscard]] auto crc32c(std::span<const std::byte> data) noexcept
    -> std::uint32_t;

/// Incremental CRC32C: start with state 0, feed chunks in order, finish with
/// the returned state. `crc32c_update(0, data) == crc32c(data)`.
[[nodiscard]] auto crc32c_update(std::uint32_t state,
                                 std::span<const std::byte> data) noexcept
    -> std::uint32_t;

} // namespace refrain::data
