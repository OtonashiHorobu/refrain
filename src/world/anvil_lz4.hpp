// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Private decoder for the Anvil LZ4 chunk stream (compression id 4). 26.3
// writes lz4-java's `LZ4BlockOutputStream`, not the standard LZ4 frame:
// `"LZ4Block"` magic, 13-byte per-block header (method/level, compressed and
// original lengths, XXHash32), and one terminal empty block.

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace refrain::world::anvil::detail {

/// Decode a complete lz4-java block stream. `max_output` bounds the total
/// decompressed size before any block allocation. Fail-closed on malformed
/// headers, truncation, invalid LZ4 data, checksum mismatch or trailing data.
[[nodiscard]] auto decode_lz4_block_stream(std::span<const std::byte> input,
                                           std::size_t max_output)
    -> result<std::vector<std::byte>>;

} // namespace refrain::world::anvil::detail
