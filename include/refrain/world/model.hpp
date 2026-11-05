// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// World data model: chunk columns, sections, palettes, light and heightmaps as
// stored by RWF and exchanged with Anvil.
//
// The structs themselves are generated from `schema/storage/chunk.rsc`; this
// header adds the invariants and codec helpers that every producer/consumer
// shares. The RWF container is `refrain::world::rwf`.
//
// Design rules:
// * Block-state palettes store canonical key strings (`minecraft:stone`,
//   `minecraft:oak_log[axis=y]`), never registry ids: import and export must
//   be lossless before a frozen registry exists. `palette_entry::state` is a
//   runtime-only numeric id (0 in the current format).
// * Packed palettes use the vanilla 1.16+ "padded" layout: each long holds
//   `64 / bits_per_entry` complete entries and never straddles a long. A
//   palette with at most one entry uses `single_value` (zero bits, no data).
// * Validation is fail-closed and bounded: sections/palette/light sizes are
//   checked before packing or decoding so untrusted worlds cannot drive
//   unbounded work.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/schema/storage/chunk.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::world {

using storage::block_pos;
using storage::chunk;
using storage::chunk_status;
using storage::heightmap_record;
using storage::light_section;
using storage::palette_entry;
using storage::saved_tick;
using storage::section_kind;
using storage::section_record;

/// Horizontal edge of a chunk/section column, in blocks.
inline constexpr std::int32_t section_edge = 16;

/// Number of block entries in one section (16×16×16).
inline constexpr std::size_t section_volume = 4096;

/// Number of biome entries in one section: biomes use a 4×4×4 grid.
inline constexpr std::size_t biome_volume = 64;

/// Nibble light array length for one section (4096 nibbles).
inline constexpr std::size_t light_nibble_bytes = 2048;

/// Number of columns in a chunk (16×16).
inline constexpr std::size_t heightmap_columns = 256;

/// Chunk field-set version understood by this build.
///
/// Pre-release policy: the storage format is not frozen before the first
/// public release, so this stays at 1 and incompatible field changes are made
/// in place (the schema hash tracks builds). Do not introduce "v2"/"v3"
/// names or migration scaffolding before the freeze; the version is bumped
/// exactly once, when the format is frozen.
inline constexpr std::uint16_t chunk_format_version = 1;

/// Section Y bounds accepted by the model. Vanilla 26.3 overworld uses
/// -4..19; the bound is dimension-agnostic (any height up to 4096 blocks).
inline constexpr std::int32_t min_section_y = -64;
inline constexpr std::int32_t max_section_y = 63;

/// Upper bounds for untrusted input.
inline constexpr std::size_t max_sections_per_chunk = 128;

/// Upper bound for light layers: one per block section plus vanilla's padding
/// sections (one below `y_pos`, one above the build limit).
inline constexpr std::size_t max_light_sections = max_sections_per_chunk + 2;
inline constexpr std::size_t max_palette_entries = 4096;
inline constexpr std::size_t max_heightmaps = 32;
inline constexpr std::size_t max_ticks = 65535;
inline constexpr std::size_t max_block_entities = 65535;
inline constexpr std::size_t max_post_processing_entries = 4096;
inline constexpr std::size_t max_extension_count = 1024;

/// Maximum size of any single opaque payload (NBT blob, structures, ...).
/// Mirrors the RWF per-record limit; keeps untrusted inputs bounded.
inline constexpr std::size_t max_payload_bytes = 4ULL * 1024ULL * 1024ULL;

/// Horizontal chunk coordinates. Ordering is the deterministic interop order
/// used by CLI output, harness reports and region iteration.
struct chunk_pos {
    std::int32_t x{0};
    std::int32_t z{0};

    auto operator<=>(const chunk_pos &) const = default;
    [[nodiscard]] auto operator==(const chunk_pos &) const -> bool = default;

    /// `"x.z"` — the key used by blob-store file names and reports.
    [[nodiscard]] auto key() const -> std::string;
};

/// `"(x, z)"` for diagnostics and CLI output.
[[nodiscard]] auto format_chunk_pos(const chunk_pos &pos) -> std::string;

/// Parse the `"x.z"` key form (`-1.0`, `12.-3`). Rejects leading `+`, empty
/// components and out-of-range values.
[[nodiscard]] auto parse_chunk_key(std::string_view key) -> result<chunk_pos>;

/// A block state key split into name and sorted properties.
struct block_state_key {
    std::string name;
    std::vector<std::pair<std::string, std::string>> properties;

    auto operator<=>(const block_state_key &) const = default;
};

/// Parse `minecraft:stone` or `minecraft:oak_log[axis=y]`. Properties are
/// sorted by name; duplicate property names and empty components are rejected.
[[nodiscard]] auto parse_block_state_key(std::string_view text)
    -> result<block_state_key>;

/// Canonical text form (`name[k=v,...]`, properties sorted). A key without
/// properties is just the name.
[[nodiscard]] auto format_block_state_key(const block_state_key &key)
    -> std::string;

/// Build and canonicalise a key from an already-split name/properties pair.
[[nodiscard]] auto canonical_block_state_key(
    std::string_view name,
    std::span<const std::pair<std::string, std::string>> properties)
    -> result<std::string>;

/// Bits per packed index for a palette of `palette_size` entries. Returns 0 for
/// an empty/one-entry palette (the `single_value` fast path).
[[nodiscard]] auto bits_for_palette(std::size_t palette_size) -> std::uint8_t;

/// Number of `u64` words needed for `entries` packed indices at
/// `bits_per_entry`. Returns 0 for the single-value case (bits == 0) and
/// `invalid_argument` for bits > 32 (checked by callers).
[[nodiscard]] auto packed_word_count(std::uint8_t bits_per_entry,
                                     std::size_t entries) -> std::size_t;

/// Unpack `entries` palette indices from `words`.
///
/// Fails with `invalid_argument` for an unsupported bit width or a word count
/// that does not match, and `parse_error` when an index is outside the palette.
[[nodiscard]] auto
unpack_palette_indices(std::span<const std::uint64_t> words,
                       std::uint8_t bits_per_entry, std::size_t entries,
                       std::size_t palette_size, std::span<std::uint32_t> out)
    -> status;

/// Pack `indices` into `out` (cleared first); every index must be inside the
/// palette. Fails like `unpack_palette_indices`.
[[nodiscard]] auto pack_palette_indices(std::span<const std::uint32_t> indices,
                                        std::uint8_t bits_per_entry,
                                        std::size_t palette_size,
                                        std::vector<std::uint64_t> &out)
    -> status;

/// Structural validation of a chunk column (no world/registry semantics).
///
/// Checks the field-set version, section ordering/uniqueness, palette/bits
/// consistency, light array sizes, heightmap lengths and every collection
/// bound. Returns the first problem with the offending section in the message.
[[nodiscard]] auto validate(const chunk &value) -> status;

/// Block/biome section with the given section Y, or nullptr.
[[nodiscard]] auto section_at(const chunk &value, std::int32_t section_y)
    -> const section_record *;

/// Light layer with the given section Y, or nullptr.
[[nodiscard]] auto light_at(const chunk &value, std::int32_t section_y)
    -> const light_section *;

/// Stable status name (`"full"`, `"noise"`, ...).
[[nodiscard]] auto chunk_status_name(chunk_status status) -> std::string_view;

/// Parse `"minecraft:full"` or `"full"`; unknown names fail with `parse_error`.
[[nodiscard]] auto parse_chunk_status(std::string_view name)
    -> result<chunk_status>;

} // namespace refrain::world
