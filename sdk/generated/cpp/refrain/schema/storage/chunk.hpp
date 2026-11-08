// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 927eab4ffcbbd2c4

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_STORAGE_CHUNK_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_STORAGE_CHUNK_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

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
[[nodiscard]] inline auto encode_block_pos(::refrain::net::writer &out, const block_pos &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.x));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.y));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.z));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_pos(::refrain::net::reader &in, block_pos &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = static_cast<decltype(value.x)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = static_cast<decltype(value.y)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = static_cast<decltype(value.z)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_palette_entry(::refrain::net::writer &out, const palette_entry &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.key, 32767); !written) {
      return written;
    }
  }
  {
    out.write_u16(value.count);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.state));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_palette_entry(::refrain::net::reader &in, palette_entry &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key = std::move(*value_0);
  }
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.count = static_cast<decltype(value.count)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.state = static_cast<decltype(value.state)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_section_record(::refrain::net::writer &out, const section_record &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.y));
  }
  {
    out.write_u16(value.block_count);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.kind));
  }
  {
    out.write_u8(value.bits_per_entry);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.palette.size()));
    for (const auto &item_0 : value.palette) {
      if (auto written = ::refrain::storage::encode_palette_entry(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.data.size()));
    for (const auto &item_0 : value.data) {
      out.write_varint(static_cast<std::uint32_t>(item_0));
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.biome_kind));
  }
  {
    out.write_u8(value.biome_bits_per_entry);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.biome_palette.size()));
    for (const auto &item_0 : value.biome_palette) {
      if (auto written = out.write_string(item_0, 32767); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.biome_data.size()));
    for (const auto &item_0 : value.biome_data) {
      out.write_varint(static_cast<std::uint32_t>(item_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_section_record(::refrain::net::reader &in, section_record &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = static_cast<decltype(value.y)>(*value_0);
  }
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.block_count = static_cast<decltype(value.block_count)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.kind = static_cast<decltype(value.kind)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.bits_per_entry = static_cast<decltype(value.bits_per_entry)>(*value_0);
  }
  {
    value.palette.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::storage::palette_entry value_0{};
      if (auto decoded = ::refrain::storage::decode_palette_entry(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.palette.push_back(std::move(value_0));
    }
  }
  {
    value.data.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::uint64_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = static_cast<decltype(value_0)>(*value_1);
      value.data.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.biome_kind = static_cast<decltype(value.biome_kind)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.biome_bits_per_entry = static_cast<decltype(value.biome_bits_per_entry)>(*value_0);
  }
  {
    value.biome_palette.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::string value_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.biome_palette.push_back(std::move(value_0));
    }
  }
  {
    value.biome_data.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::uint64_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = static_cast<decltype(value_0)>(*value_1);
      value.biome_data.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_light_section(::refrain::net::writer &out, const light_section &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.y));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.block_light.size()));
    out.write_bytes(std::span<const std::byte>{value.block_light });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sky_light.size()));
    out.write_bytes(std::span<const std::byte>{value.sky_light });
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_light_section(::refrain::net::reader &in, light_section &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = static_cast<decltype(value.y)>(*value_0);
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.block_light = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sky_light = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_heightmap_record(::refrain::net::writer &out, const heightmap_record &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.name, 32767); !written) {
      return written;
    }
  }
  {
    out.write_u8(value.bits_per_entry);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.data.size()));
    for (const auto &item_0 : value.data) {
      out.write_varint(static_cast<std::uint32_t>(item_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_heightmap_record(::refrain::net::reader &in, heightmap_record &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.bits_per_entry = static_cast<decltype(value.bits_per_entry)>(*value_0);
  }
  {
    value.data.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::uint64_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = static_cast<decltype(value_0)>(*value_1);
      value.data.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_saved_tick(::refrain::net::writer &out, const saved_tick &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.id, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::storage::encode_block_pos(out, value.pos); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.priority));
  }
  {
    out.write_u8(value.kind);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.remaining_ticks));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_saved_tick(::refrain::net::reader &in, saved_tick &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = std::move(*value_0);
  }
  {
    if (auto decoded = ::refrain::storage::decode_block_pos(in, value.pos); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.priority = static_cast<decltype(value.priority)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.kind = static_cast<decltype(value.kind)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.remaining_ticks = static_cast<decltype(value.remaining_ticks)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chunk(::refrain::net::writer &out, const chunk &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u16(value.format_version);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.status));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.last_update));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.inhabited_time));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sections.size()));
    for (const auto &item_0 : value.sections) {
      if (auto written = ::refrain::storage::encode_section_record(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.heightmaps.size()));
    for (const auto &item_0 : value.heightmaps) {
      if (auto written = ::refrain::storage::encode_heightmap_record(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.block_ticks.size()));
    for (const auto &item_0 : value.block_ticks) {
      if (auto written = ::refrain::storage::encode_saved_tick(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.fluid_ticks.size()));
    for (const auto &item_0 : value.fluid_ticks) {
      if (auto written = ::refrain::storage::encode_saved_tick(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.block_entities.size()));
    for (const auto &item_0 : value.block_entities) {
      out.write_varint(static_cast<std::uint32_t>(item_0.size()));
      out.write_bytes(std::span<const std::byte>{item_0 });
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.structures.size()));
    out.write_bytes(std::span<const std::byte>{value.structures });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.post_processing.size()));
    for (const auto &item_0 : value.post_processing) {
      out.write_varint(static_cast<std::uint32_t>(item_0.size()));
      for (const auto &item_1 : item_0) {
        out.write_i64(static_cast<std::int64_t>(item_1));
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.blending.size()));
    out.write_bytes(std::span<const std::byte>{value.blending });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.extra.size()));
    out.write_bytes(std::span<const std::byte>{value.extra });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.extensions.size()));
    for (const auto &item_0 : value.extensions) {
      out.write_varint(static_cast<std::uint32_t>(item_0.size()));
      out.write_bytes(std::span<const std::byte>{item_0 });
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.y_pos));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.light_sections.size()));
    for (const auto &item_0 : value.light_sections) {
      if (auto written = ::refrain::storage::encode_light_section(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chunk(::refrain::net::reader &in, chunk &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.format_version = static_cast<decltype(value.format_version)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.status = static_cast<decltype(value.status)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.last_update = static_cast<decltype(value.last_update)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.inhabited_time = static_cast<decltype(value.inhabited_time)>(*value_0);
  }
  {
    value.sections.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::storage::section_record value_0{};
      if (auto decoded = ::refrain::storage::decode_section_record(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.sections.push_back(std::move(value_0));
    }
  }
  {
    value.heightmaps.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::storage::heightmap_record value_0{};
      if (auto decoded = ::refrain::storage::decode_heightmap_record(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.heightmaps.push_back(std::move(value_0));
    }
  }
  {
    value.block_ticks.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::storage::saved_tick value_0{};
      if (auto decoded = ::refrain::storage::decode_saved_tick(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.block_ticks.push_back(std::move(value_0));
    }
  }
  {
    value.fluid_ticks.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::storage::saved_tick value_0{};
      if (auto decoded = ::refrain::storage::decode_saved_tick(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.fluid_ticks.push_back(std::move(value_0));
    }
  }
  {
    value.block_entities.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::vector<std::byte> value_0{};
      auto length_1 = in.read_varint();
      if (!length_1) return std::unexpected{std::move(length_1.error())};
      auto value_1 = in.read_bytes(*length_1);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::vector<std::byte>{value_1->begin(), value_1->end()};
      value.block_entities.push_back(std::move(value_0));
    }
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.structures = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    value.post_processing.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::vector<std::int16_t> value_0{};
      value_0.clear();
      auto count_1 = in.read_varint();
      if (!count_1) return std::unexpected{std::move(count_1.error())};
      for (std::uint32_t index_1 = 0; index_1 < *count_1; ++index_1) {
        std::int16_t value_1{};
        auto value_2 = in.read_i64();
        if (!value_2) return std::unexpected{std::move(value_2.error())};
        value_1 = static_cast<decltype(value_1)>(*value_2);
        value_0.push_back(std::move(value_1));
      }
      value.post_processing.push_back(std::move(value_0));
    }
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.blending = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.extra = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    value.extensions.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::vector<std::byte> value_0{};
      auto length_1 = in.read_varint();
      if (!length_1) return std::unexpected{std::move(length_1.error())};
      auto value_1 = in.read_bytes(*length_1);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::vector<std::byte>{value_1->begin(), value_1->end()};
      value.extensions.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_pos = static_cast<decltype(value.y_pos)>(*value_0);
  }
  {
    value.light_sections.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::storage::light_section value_0{};
      if (auto decoded = ::refrain::storage::decode_light_section(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.light_sections.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

} // namespace refrain::storage

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_STORAGE_CHUNK_HPP
