// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 4a5dc6c6c8f9b026

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_STORAGE_CHUNK_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_STORAGE_CHUNK_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace refrain::storage {

/// Chunk lifecycle status, aligned with the vanilla status pipeline but stored
/// as Refrain's own format.
enum class chunk_status : std::uint8_t {
    empty = 0,
    structure_starts = 1,
    structure_references = 2,
    biomes = 3,
    terrain = 4,
    features = 5,
    initialize_light = 6,
    light = 7,
    spawn = 8,
    full = 9,
};

/// How a block/biome section stores its values: one palette entry
/// (bits_per_entry == 0, `data` empty) or a packed palette index array.
enum class section_kind : std::uint8_t {
    single_value = 0,
    palette = 1,
};

struct block_pos {
    /// Absolute world X.
    std::int32_t x;
    /// Absolute world Y (signed: dimensions may extend below zero).
    std::int16_t y;
    /// Absolute world Z.
    std::int32_t z;
};

/// One block-state palette entry.
/// 
/// `key` is the canonical state string used by the interop format:
/// `minecraft:stone` or `minecraft:oak_log[axis=y]` (properties sorted by
/// name). `count` is the number of blocks using this state (0 when unknown)
/// and `state` is the per-world numeric id assigned by the registry cache;
/// neither participates in Anvil import/export.
struct palette_entry {
    std::string key;
    std::uint16_t count;
    std::uint32_t state;
};

struct section_record {
    /// Section Y index inside the chunk column.
    // attributes: #[bits(8)]
    std::int8_t y;
    /// Number of non-air blocks in the section (0 when unknown).
    std::uint16_t block_count;
    /// Block-state encoding.
    ::refrain::storage::section_kind kind;
    /// Bits per packed block-state index (0 for `single_value`).
    std::uint8_t bits_per_entry;
    /// Block-state palette; `data` holds indices into it.
    std::vector<::refrain::storage::palette_entry> palette;
    /// Packed block-state indices (4096 entries, vanilla padded layout).
    // attributes: #[varint]
    std::vector<std::uint64_t> data;
    /// Biome encoding (same `single_value`/`palette` rules as blocks).
    ::refrain::storage::section_kind biome_kind;
    /// Bits per packed biome index (0 for `single_value`).
    std::uint8_t biome_bits_per_entry;
    /// Biome palette keys (`minecraft:plains`, ...), not numeric ids.
    std::vector<std::string> biome_palette;
    /// Packed biome indices (4096 entries, vanilla padded layout).
    // attributes: #[varint]
    std::vector<std::uint64_t> biome_data;
};

/// One light layer keyed by section Y. Light is independent of the block
/// column in vanilla (`LevelLightEngine` vs `ChunkAccess`): it may exist at
/// `y_pos - 1` or one above the top block section, where there is no
/// `LevelChunkSection`, and such padding layers are written to Anvil by
/// `SerializableChunkData.copyOf`.
struct light_section {
    /// Section Y of this light layer (may be outside the block section range).
    // attributes: #[bits(8)]
    std::int8_t y;
    /// Nibble block-light array, 2048 bytes; empty when absent.
    std::vector<std::byte> block_light;
    /// Nibble sky-light array, 2048 bytes; empty when absent.
    std::vector<std::byte> sky_light;
};

/// One heightmap: vanilla keeps them as a compound of packed long arrays whose
/// bit width depends on the world height, so both are stored explicitly.
struct heightmap_record {
    /// `MOTION_BLOCKING`, `WORLD_SURFACE`, `OCEAN_FLOOR`, ...
    std::string name;
    /// Bits per column entry (vanilla 26.3: 9).
    std::uint8_t bits_per_entry;
    /// Packed column values (vanilla padded layout, 256 entries).
    std::vector<std::uint64_t> data;
};

struct saved_tick {
    /// Block (or fluid) registry key the tick belongs to.
    std::string id;
    ::refrain::storage::block_pos pos;
    /// Vanilla `TickPriority` value: -3 (extremely high) .. 3 (extremely low).
    std::int8_t priority;
    /// 0 = block tick, 1 = fluid tick (Refrain-internal).
    std::uint8_t kind;
    // attributes: #[varint]
    std::uint32_t remaining_ticks;
};

/// One persisted chunk column. Field order is the chunk field-set ABI: append
/// new fields at the end. `format_version` stays 1 until the pre-release format
/// freeze; a schema-hash change is not a format-version bump.
struct chunk {
    /// RWF record marker; field order is the storage ABI.
    /// Chunk field-set version (independent of the RWF container version).
    std::uint16_t format_version; // default: 1
    ::refrain::storage::chunk_status status;
    std::int64_t last_update;
    std::int64_t inhabited_time;
    /// Block/biome sections sorted by `y`; light is stored separately below.
    std::vector<::refrain::storage::section_record> sections;
    std::vector<::refrain::storage::heightmap_record> heightmaps;
    std::vector<::refrain::storage::saved_tick> block_ticks;
    std::vector<::refrain::storage::saved_tick> fluid_ticks;
    /// Block entities as opaque NBT compounds (the NBT payload includes
    /// `id`/`x`/`y`/`z`, so no coordinates are duplicated here).
    std::vector<std::vector<std::byte>> block_entities;
    /// `structures` NBT compound (starts + references), empty when absent.
    std::vector<std::byte> structures;
    /// Per-section post-processing short lists, in section order.
    std::vector<std::vector<std::int16_t>> post_processing;
    /// `blending_data` / `below_zero_retrogen` NBT, empty when absent.
    std::vector<std::byte> blending;
    /// Unmodelled root keys, preserved so import/export does not lose data.
    std::vector<std::byte> extra;
    /// Reserved for per-mod sidecar fields; unused in v1.
    // attributes: #[sidecar]
    std::vector<std::vector<std::byte>> extensions;
    /// Lowest section Y of the column (vanilla `yPos`): -4 for the overworld,
    /// 0 for the nether/end and custom dimensions. Stored so Anvil export
    /// round-trips dimensions whose section base differs from the overworld.
    std::int32_t y_pos;
    /// Light layers sorted by `y`; includes vanilla's one-section padding
    /// below `y_pos` and above the build limit. Empty arrays are omitted.
    std::vector<::refrain::storage::light_section> light_sections;
};
} // namespace refrain::storage

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_STORAGE_CHUNK_HPP
