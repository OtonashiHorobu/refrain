// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/checksum.hpp>
#include <refrain/world/chunk_codec.hpp>

#include <boost/ut.hpp>

#include <cstdio>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;
namespace world = refrain::world;

using world::chunk;
using world::chunk_status;
using world::heightmap_record;
using world::light_section;
using world::palette_entry;
using world::saved_tick;
using world::section_kind;
using world::section_record;

[[nodiscard]] auto make_single_section(std::int8_t y,
                                       std::string air = "minecraft:air")
    -> section_record {
    section_record section{};
    section.y = y;
    section.kind = section_kind::single_value;
    section.bits_per_entry = 0;
    section.palette.push_back(
        palette_entry{.key = std::move(air), .count = 4096, .state = 0});
    section.biome_kind = section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    return section;
}

[[nodiscard]] auto make_palette_section(std::int8_t y, std::size_t entries)
    -> section_record {
    section_record section{};
    section.y = y;
    section.block_count = 123;
    section.kind = section_kind::palette;
    section.bits_per_entry = world::bits_for_palette(entries);
    for (std::size_t index{0}; index < entries; ++index) {
        section.palette.push_back(palette_entry{
            .key = "minecraft:test_block_" + std::to_string(index),
            .count = static_cast<std::uint16_t>(index % 100),
            .state = static_cast<std::uint32_t>(index + 7)});
    }
    std::vector<std::uint32_t> indices(world::section_volume);
    for (std::size_t index{0}; index < indices.size(); ++index) {
        indices[index] = static_cast<std::uint32_t>(index % entries);
    }
    expect(static_cast<bool>(world::pack_palette_indices(
        indices, section.bits_per_entry, entries, section.data)));
    section.biome_kind = section_kind::palette;
    section.biome_bits_per_entry = 1;
    section.biome_palette = {"minecraft:plains", "minecraft:desert"};
    section.biome_data.assign(1, 0);
    return section;
}

[[nodiscard]] auto make_light(std::int8_t y, std::uint8_t block,
                              std::uint8_t sky) -> light_section {
    light_section light{};
    light.y = y;
    if (block != 0) {
        light.block_light.assign(world::light_nibble_bytes,
                                 static_cast<std::byte>(block));
    }
    if (sky != 0) {
        light.sky_light.assign(world::light_nibble_bytes,
                               static_cast<std::byte>(sky));
    }
    return light;
}

[[nodiscard]] auto make_full_chunk() -> chunk {
    chunk value{};
    value.format_version = world::chunk_format_version;
    value.status = chunk_status::full;
    value.y_pos = -4;
    value.last_update = 123456789;
    value.inhabited_time = 9001;
    value.sections.push_back(make_single_section(-4));
    value.sections.push_back(make_palette_section(0, 17));
    value.sections.push_back(make_palette_section(1, 3));
    value.light_sections.push_back(make_light(-5, 0x00, 0x0F));
    value.light_sections.push_back(make_light(0, 0x21, 0xFF));
    value.light_sections.push_back(make_light(1, 0x00, 0x11));

    heightmap_record motion{};
    motion.name = "MOTION_BLOCKING";
    motion.bits_per_entry = 9;
    motion.data.assign(37, 0x0102030405060708ULL);
    value.heightmaps.push_back(motion);
    heightmap_record surface{};
    surface.name = "WORLD_SURFACE";
    surface.bits_per_entry = 9;
    surface.data.assign(37, 0);
    value.heightmaps.push_back(surface);

    saved_tick block_tick{};
    block_tick.id = "minecraft:redstone_wire";
    block_tick.pos = {.x = 3, .y = 70, .z = -5};
    block_tick.priority = 0;
    block_tick.kind = 1;
    block_tick.remaining_ticks = 20;
    value.block_ticks.push_back(block_tick);
    saved_tick fluid_tick{};
    fluid_tick.id = "minecraft:water";
    fluid_tick.pos = {.x = 0, .y = 64, .z = 0};
    fluid_tick.priority = -2;
    fluid_tick.kind = 3;
    fluid_tick.remaining_ticks = 5;
    value.fluid_ticks.push_back(fluid_tick);

    value.block_entities.push_back({std::byte{0x0A}, std::byte{0x00}});
    value.block_entities.push_back(
        {std::byte{0x0A}, std::byte{0x01}, std::byte{0x02}});
    value.structures = {std::byte{0x0A}, std::byte{0x00}};
    value.blending = {std::byte{0x08}, std::byte{0x00}, std::byte{0x01}};
    value.extra = {std::byte{0x0A}, std::byte{0x00}};
    value.post_processing = {
        {std::int16_t{15}}, {}, {std::int16_t{1}, std::int16_t{2}}};
    return value;
}

[[nodiscard]] auto same_palette(const palette_entry &left,
                                const palette_entry &right) -> bool {
    return left.key == right.key && left.count == right.count &&
           left.state == right.state;
}

[[nodiscard]] auto same_section(const section_record &left,
                                const section_record &right) -> bool {
    return left.y == right.y && left.block_count == right.block_count &&
           left.kind == right.kind &&
           left.bits_per_entry == right.bits_per_entry &&
           left.palette.size() == right.palette.size() &&
           std::equal(left.palette.begin(), left.palette.end(),
                      right.palette.begin(), same_palette) &&
           left.data == right.data && left.biome_kind == right.biome_kind &&
           left.biome_bits_per_entry == right.biome_bits_per_entry &&
           left.biome_palette == right.biome_palette &&
           left.biome_data == right.biome_data;
}

[[nodiscard]] auto same_light(const light_section &left,
                              const light_section &right) -> bool {
    return left.y == right.y && left.block_light == right.block_light &&
           left.sky_light == right.sky_light;
}

[[nodiscard]] auto same_chunk(const chunk &left, const chunk &right) -> bool {
    if (left.format_version != right.format_version ||
        left.status != right.status || left.last_update != right.last_update ||
        left.inhabited_time != right.inhabited_time ||
        left.sections.size() != right.sections.size() ||
        left.light_sections.size() != right.light_sections.size() ||
        left.heightmaps.size() != right.heightmaps.size() ||
        left.block_ticks.size() != right.block_ticks.size() ||
        left.fluid_ticks.size() != right.fluid_ticks.size() ||
        left.block_entities != right.block_entities ||
        left.structures != right.structures ||
        left.post_processing != right.post_processing ||
        left.blending != right.blending || left.extra != right.extra) {
        return false;
    }
    for (std::size_t index{0}; index < left.sections.size(); ++index) {
        if (!same_section(left.sections[index], right.sections[index])) {
            return false;
        }
    }
    for (std::size_t index{0}; index < left.light_sections.size(); ++index) {
        if (!same_light(left.light_sections[index],
                        right.light_sections[index])) {
            return false;
        }
    }
    for (std::size_t index{0}; index < left.heightmaps.size(); ++index) {
        if (left.heightmaps[index].name != right.heightmaps[index].name ||
            left.heightmaps[index].bits_per_entry !=
                right.heightmaps[index].bits_per_entry ||
            left.heightmaps[index].data != right.heightmaps[index].data) {
            return false;
        }
    }
    const auto same_tick = [](const saved_tick &a, const saved_tick &b) {
        return a.id == b.id && a.pos.x == b.pos.x && a.pos.y == b.pos.y &&
               a.pos.z == b.pos.z && a.priority == b.priority &&
               a.kind == b.kind && a.remaining_ticks == b.remaining_ticks;
    };
    return std::equal(left.block_ticks.begin(), left.block_ticks.end(),
                      right.block_ticks.begin(), same_tick) &&
           std::equal(left.fluid_ticks.begin(), left.fluid_ticks.end(),
                      right.fluid_ticks.begin(), same_tick);
}

} // namespace

suite chunk_codec = [] {
    world::palette_table palette;
    "encode_decode_roundtrip"_test = [&palette] {
        const auto original{make_full_chunk()};
        const auto payload{world::encode_chunk(original, &palette)};
        expect(static_cast<bool>(payload));
        const auto decoded{world::decode_chunk(*payload, &palette)};
        expect(static_cast<bool>(decoded));
        if (decoded) {
            expect(same_chunk(original, *decoded));
        }
    };

    "inline_palette_payload_is_self_contained"_test = [] {
        const auto original{make_full_chunk()};
        const auto payload{world::encode_chunk(
            original, nullptr, true, world::chunk_palette_mode::inline_keys)};
        expect(static_cast<bool>(payload));
        if (!payload) {
            return;
        }
        // No palette table is passed: the payload names every state.
        const auto decoded{world::decode_chunk(*payload, nullptr)};
        expect(static_cast<bool>(decoded));
        if (decoded) {
            expect(same_chunk(original, *decoded));
        }
    };

    "encode_is_canonical"_test = [&palette] {
        auto unsorted{make_full_chunk()};
        std::swap(unsorted.sections[0], unsorted.sections[2]);
        std::swap(unsorted.light_sections[0], unsorted.light_sections[2]);
        std::swap(unsorted.heightmaps[0], unsorted.heightmaps[1]);
        const auto sorted{make_full_chunk()};
        const auto unsorted_hash{world::semantic_hash(unsorted)};
        const auto sorted_hash{world::semantic_hash(sorted)};
        expect(static_cast<bool>(unsorted_hash));
        expect(static_cast<bool>(sorted_hash));
        expect(*unsorted_hash == *sorted_hash);

        world::palette_table canonical_palette;
        const auto decoded{world::decode_chunk(
            *world::encode_chunk(unsorted, &canonical_palette),
            &canonical_palette)};
        expect(static_cast<bool>(decoded));
        if (decoded) {
            expect(decoded->sections[0].y == -4);
            expect(decoded->sections[1].y == 0);
            expect(decoded->sections[2].y == 1);
            expect(decoded->light_sections[0].y == -5);
            expect(decoded->light_sections[1].y == 0);
            expect(decoded->light_sections[2].y == 1);
            expect(decoded->heightmaps[0].name == "MOTION_BLOCKING");
            expect(decoded->heightmaps[1].name == "WORLD_SURFACE");
        }
    };

    "semantic_hash_detects_changes"_test = [&palette] {
        const auto baseline{make_full_chunk()};
        const auto hash{world::semantic_hash(baseline)};
        expect(static_cast<bool>(hash));
        auto changed{baseline};
        changed.sections[1].palette[0].key = "minecraft:changed";
        const auto changed_hash{world::semantic_hash(changed)};
        expect(static_cast<bool>(changed_hash));
        expect(*hash != *changed_hash);

        auto lit{baseline};
        lit.light_sections[1].sky_light[0] = std::byte{0x00};
        expect(*world::semantic_hash(lit) != *hash);

        auto status{baseline};
        status.status = chunk_status::light;
        expect(*world::semantic_hash(status) != *hash);
    };

    "empty_chunk_roundtrip"_test = [&palette] {
        chunk value{};
        value.format_version = world::chunk_format_version;
        value.status = chunk_status::empty;
        const auto payload{world::encode_chunk(value, &palette)};
        expect(static_cast<bool>(payload));
        const auto decoded{world::decode_chunk(*payload, &palette)};
        expect(static_cast<bool>(decoded));
        if (decoded) {
            expect(decoded->sections.empty());
            expect(decoded->heightmaps.empty());
            expect(decoded->status == chunk_status::empty);
        }
    };

    "encode_rejects_invalid_chunk"_test = [&palette] {
        auto value{make_full_chunk()};
        value.sections[1].bits_per_entry = 2;
        expect(!world::encode_chunk(value, &palette));
        expect(!world::semantic_hash(value));
    };

    "decode_rejects_truncated_payload"_test = [&palette] {
        const auto payload{*world::encode_chunk(make_full_chunk(), &palette)};
        expect(!world::decode_chunk(
            std::span<const std::byte>{payload}.first(payload.size() - 3)));
        expect(!world::decode_chunk(
            std::span<const std::byte>{payload}.first(10)));
        expect(!world::decode_chunk({}, &palette));
    };

    "decode_rejects_corrupt_field"_test = [&palette] {
        auto payload{*world::encode_chunk(make_full_chunk(), &palette)};
        payload[20] = static_cast<std::byte>(
            static_cast<std::uint8_t>(payload[20]) ^ 0x5A);
        const auto decoded{world::decode_chunk(payload, &palette)};
        expect(!decoded);
        if (!decoded) {
            expect(decoded.error().code == refrain::error_code::parse_error);
        }
    };

    "decode_rejects_duplicate_fields"_test = [&palette] {
        auto payload{*world::encode_chunk(make_full_chunk(), &palette)};
        payload.insert(payload.end(), payload.begin(), payload.end());
        expect(!world::decode_chunk(payload, &palette));
    };

    "decode_rejects_missing_sections"_test = [&palette] {
        // Status-only payload: field id 1, type 1, 17 bytes, CRC.
        std::vector<std::byte> payload(16, std::byte{0});
        payload[0] = std::byte{1};
        payload[4] = std::byte{17};
        const auto decoded{world::decode_chunk(payload, &palette)};
        expect(!decoded);
        if (!decoded) {
            expect(decoded.error().code == refrain::error_code::parse_error);
        }
    };

    "decode_rejects_oversized_palette"_test = [&palette] {
        auto payload{*world::encode_chunk(make_full_chunk(), &palette)};
        // The section field payload starts after the status field (16 + 24
        // bytes); corrupt its palette count to a huge value.
        constexpr std::size_t section_field_offset = 16 + 24;
        constexpr std::size_t section_count_offset =
            section_field_offset + 16 + 7;
        expect(payload.size() > section_count_offset + 2);
        payload[section_count_offset] = std::byte{0xFF};
        payload[section_count_offset + 1] = std::byte{0x7F};
        // Recompute the field CRC so the palette bound is what rejects it.
        const auto field_offset = section_field_offset;
        const auto field_size =
            static_cast<std::size_t>(payload[field_offset + 4]) |
            (static_cast<std::size_t>(payload[field_offset + 5]) << 8U) |
            (static_cast<std::size_t>(payload[field_offset + 6]) << 16U) |
            (static_cast<std::size_t>(payload[field_offset + 7]) << 24U);
        auto body{std::span<const std::byte>{payload}.subspan(field_offset + 16,
                                                              field_size)};
        const auto crc{refrain::data::crc32c(body)};
        for (std::size_t index{0}; index < 4; ++index) {
            payload[field_offset + 8 + index] =
                static_cast<std::byte>((crc >> (index * 8U)) & 0xFFU);
        }
        expect(!world::decode_chunk(payload, &palette));
    };
};
