// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otoru Horobu

#include <refrain/world/proto_bridge.hpp>

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/heightmap.hpp>
#include <refrain/worldgen/chunk/status.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace refrain::world {
namespace {

using worldgen::block_state_id;
using worldgen::jint;

constexpr std::size_t section_blocks = 4096;

auto pack_heightmap(const worldgen::heightmap &source, jint min_y)
    -> ::refrain::storage::heightmap_record {
    constexpr std::uint8_t bits = 9;
    constexpr std::size_t per_long = 64U / bits;
    constexpr std::size_t entries = 256;
    ::refrain::storage::heightmap_record record{};
    record.bits_per_entry = bits;
    record.data.assign((entries + per_long - 1) / per_long, 0);
    for (std::size_t index = 0; index < entries; ++index) {
        const auto x = static_cast<jint>(index % 16U);
        const auto z = static_cast<jint>(index / 16U);
        const auto value =
            static_cast<std::uint64_t>(source.first_available(x, z) - min_y);
        const auto word = index / per_long;
        const auto offset = (index % per_long) * bits;
        record.data[word] |= (value & 0x1FFU) << offset;
    }
    return record;
}

auto pack_indices(const std::vector<std::uint32_t> &indices, std::uint8_t bits)
    -> std::vector<std::uint64_t> {
    if (bits == 0) {
        return {};
    }
    constexpr std::size_t per_long = 64U;
    std::vector<std::uint64_t> data;
    const auto per = per_long / bits;
    data.assign((indices.size() + per - 1) / per, 0);
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1U;
    for (std::size_t index = 0; index < indices.size(); ++index) {
        data[index / per] |= (static_cast<std::uint64_t>(indices[index]) & mask)
                             << ((index % per) * bits);
    }
    return data;
}

} // namespace

auto from_proto_chunk(const worldgen::proto_chunk &source,
                      const std::vector<std::string> &biome_names,
                      chunk_status status) -> result<chunk> {
    chunk out{};
    out.format_version = chunk_format_version;
    out.status = static_cast<::refrain::storage::chunk_status>(status);
    out.last_update = 0;
    out.inhabited_time = 0;
    out.y_pos = source.min_y() >> 4;

    const jint min_y = source.min_y();
    const jint section_count = source.height() / 16;
    const auto &states = source.table();
    out.sections.reserve(static_cast<std::size_t>(section_count));
    out.post_processing.resize(static_cast<std::size_t>(section_count));
    for (jint section = 0; section < section_count; ++section) {
        const jint base_y = min_y + section * 16;
        ::refrain::storage::section_record record{};
        record.y = static_cast<std::int8_t>(base_y >> 4);
        std::map<block_state_id, std::uint32_t> palette_index;
        std::vector<worldgen::block_state_id> palette;
        std::vector<std::uint32_t> indices(section_blocks, 0);
        std::uint16_t block_count = 0;
        for (std::size_t i = 0; i < section_blocks; ++i) {
            const auto y = base_y + static_cast<jint>(i / 256U);
            const auto z = static_cast<jint>((i % 256U) / 16U);
            const auto x = static_cast<jint>(i % 16U);
            const auto state = source.state_at(x, y, z);
            if (!states.traits(state).air) {
                ++block_count;
            }
            const auto found = palette_index.find(state);
            if (found != palette_index.end()) {
                indices[i] = found->second;
            } else {
                const auto next = static_cast<std::uint32_t>(palette.size());
                palette_index.emplace(state, next);
                palette.push_back(state);
                indices[i] = next;
            }
        }
        record.block_count = block_count;
        record.palette.reserve(palette.size());
        for (const auto state : palette) {
            const auto raw_key = states.name(state);
            std::string key{raw_key};
            if (const auto parsed = parse_block_state_key(raw_key); parsed) {
                key = format_block_state_key(*parsed);
            }
            record.palette.push_back(
                {.key = std::move(key),
                 .count = 0,
                 .state = static_cast<std::uint32_t>(state)});
        }
        record.bits_per_entry =
            static_cast<std::uint8_t>(bits_for_palette(palette.size()));
        record.kind = record.bits_per_entry == 0
                          ? ::refrain::storage::section_kind::single_value
                          : ::refrain::storage::section_kind::palette;
        if (record.kind == ::refrain::storage::section_kind::palette) {
            record.data = pack_indices(indices, record.bits_per_entry);
        }

        // The storage model stores one biome per 4x4x4 cell, so a section has
        // 64 entries; sampling every block would pack 4096 indices into a
        // section the reader expects to hold 64.
        std::map<std::uint16_t, std::uint32_t> biome_index;
        std::vector<std::uint16_t> biome_palette;
        std::vector<std::uint32_t> biome_indices(::refrain::world::biome_volume,
                                                 0);
        for (std::size_t i = 0; i < ::refrain::world::biome_volume; ++i) {
            const auto y = base_y + static_cast<jint>((i / 16U) * 4U);
            const auto z = static_cast<jint>(((i % 16U) / 4U) * 4U);
            const auto x = static_cast<jint>((i % 4U) * 4U);
            const auto biome = source.biome_at(x, y, z);
            const auto found = biome_index.find(biome);
            if (found != biome_index.end()) {
                biome_indices[i] = found->second;
            } else {
                const auto next =
                    static_cast<std::uint32_t>(biome_palette.size());
                biome_index.emplace(biome, next);
                biome_palette.push_back(biome);
                biome_indices[i] = next;
            }
        }
        record.biome_bits_per_entry =
            static_cast<std::uint8_t>(bits_for_palette(biome_palette.size()));
        record.biome_kind = record.biome_bits_per_entry == 0
                                ? ::refrain::storage::section_kind::single_value
                                : ::refrain::storage::section_kind::palette;
        for (const auto biome : biome_palette) {
            record.biome_palette.push_back(biome < biome_names.size()
                                               ? biome_names[biome]
                                               : "minecraft:plains");
        }
        if (record.biome_kind == ::refrain::storage::section_kind::palette) {
            record.biome_data =
                pack_indices(biome_indices, record.biome_bits_per_entry);
        }
        if (static_cast<std::size_t>(section) <
            source.post_processing().size()) {
            for (const auto packed :
                 source.post_processing()[static_cast<std::size_t>(section)]) {
                out.post_processing[static_cast<std::size_t>(section)]
                    .push_back(static_cast<std::int16_t>(packed));
            }
        }
        out.sections.push_back(std::move(record));
    }

    const std::array<std::pair<worldgen::heightmap_kind, const char *>, 4>
        heightmaps{{
            {worldgen::heightmap_kind::world_surface, "WORLD_SURFACE"},
            {worldgen::heightmap_kind::ocean_floor, "OCEAN_FLOOR"},
            {worldgen::heightmap_kind::motion_blocking, "MOTION_BLOCKING"},
            {worldgen::heightmap_kind::motion_blocking_no_leaves,
             "MOTION_BLOCKING_NO_LEAVES"},
        }};
    for (const auto &[kind, name] : heightmaps) {
        auto record = pack_heightmap(source.heightmap_at(kind), min_y);
        record.name = name;
        out.heightmaps.push_back(std::move(record));
    }

    for (const auto &[pos, entity] : source.block_entities()) {
        auto encoded = data::nbt::write(data::nbt::value{entity}, "",
                                        data::nbt::compression::none);
        if (!encoded) {
            return std::unexpected{encoded.error()};
        }
        (void)pos;
        out.block_entities.push_back(std::move(*encoded));
    }
    return out;
}

} // namespace refrain::world
