// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// RWF chunk record codec.
//
// The payload is a list of TLV fields (`id u16, type u8, flags u8, size u32,
// crc32c u32, reserved u32` + payload padded to 8 bytes). `encode_chunk`
// produces the canonical form (sections sorted by Y, heightmaps sorted by
// name, post-processing permuted with its sections) so the BLAKE3 hash of the
// payload is the semantic chunk identity used by `world diff` and the worldgen
// harness. `decode_chunk` rejects anything `world::validate` rejects, so a
// corrupt world fails at the storage boundary instead of inside the engine.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/palette_table.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace refrain::world {

/// TLV field ids (append-only byte contract).
enum class chunk_field : std::uint16_t {
    status = 1,           ///< status + last_update + inhabited_time
    sections = 2,         ///< block and biome data per section
    heightmaps = 3,       ///< named packed height columns
    ticks = 4,            ///< block + fluid ticks
    block_entities = 5,   ///< opaque NBT compounds
    structures = 6,       ///< opaque NBT compound
    post_processing = 7,  ///< per-section short lists
    blending = 8,         ///< opaque NBT (blending_data/below_zero_retrogen)
    extra = 9,            ///< unmodelled root keys (opaque NBT)
    y_pos = 10,           ///< i32 lowest section Y (vanilla `yPos`)
    light_sections = 11,  ///< light layers keyed by section Y (incl. padding)
    sections_inline = 12, ///< self-contained sections with palette keys inline
};

/// How a chunk payload references block/biome states.
///
/// * `global_ids`: palette entries are u32 ids into a shared world palette
///   table (the RWF storage default; smallest payload).
/// * `inline_keys`: palette entries carry their canonical key strings, so a
///   consumer can decode the payload without the producer's palette table.
///   Used by cross-provider/service payloads (storage/worldgen contracts).
enum class chunk_palette_mode : std::uint8_t {
    global_ids = 0,
    inline_keys = 1,
};

/// Encode a chunk into its canonical TLV payload (uncompressed).
/// `allow_palette_growth == false` requires every block/biome key to already
/// exist in `palette` (parallel import workers share one pre-scanned table);
/// a missing key is an error instead of an unsynchronised insert.
[[nodiscard]] auto
encode_chunk(const chunk &value, palette_table *palette = nullptr,
             bool allow_palette_growth = true,
             chunk_palette_mode mode = chunk_palette_mode::global_ids)
    -> result<std::vector<std::byte>>;

/// Decode a TLV payload produced by `encode_chunk` (or a future compatible
/// writer) and validate it.
[[nodiscard]] auto decode_chunk(std::span<const std::byte> payload,
                                const palette_table *palette = nullptr)
    -> result<chunk>;

/// Canonical semantic hash (lowercase BLAKE3 hex) of a chunk. Two chunks with
/// the same storage-level content hash identically regardless of input order.
[[nodiscard]] auto semantic_hash(const chunk &value) -> result<std::string>;

} // namespace refrain::world
