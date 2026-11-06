// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// MD5 (RFC 1321), used only to derive positional random streams from a name.
//
// The reference implementation hashes resource location strings with MD5 and
// splits the 16-byte digest into two big-endian 64-bit halves. MD5 is not used
// for integrity or authentication here; `refrain::data` keeps BLAKE3 for that.
// The implementation is self-contained so the worldgen core has no dependency
// on a crypto library.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace refrain::worldgen {

/// Raw 16-byte MD5 digest of `data`.
[[nodiscard]] auto md5(std::span<const std::uint8_t> data) noexcept
    -> std::array<std::uint8_t, 16>;

/// Convenience overload for text. The bytes are hashed verbatim; callers pass
/// UTF-8 encoded names.
[[nodiscard]] auto md5(std::string_view text) noexcept
    -> std::array<std::uint8_t, 16>;

} // namespace refrain::worldgen
