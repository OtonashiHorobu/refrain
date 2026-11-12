// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/world/store.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::world;
using refrain::worldgen::jint;

auto make_registry() -> worldgen::block_state_registry {
    worldgen::block_state_registry states;
    (void)states.intern("minecraft:air", {.air = true});
    (void)states.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern("minecraft:water",
                        {.air = false, .fluid = true, .water = true});
    return states;
}

auto paletted_state(const ::refrain::storage::section_record &section,
                    std::size_t index) -> std::string {
    if (section.kind == ::refrain::storage::section_kind::single_value) {
        return section.palette.empty() ? "" : section.palette.front().key;
    }
    std::vector<std::uint32_t> indices(4096, 0);
    if (!world::unpack_palette_indices(section.data, section.bits_per_entry,
                                       4096, section.palette.size(), indices)) {
        return "";
    }
    const auto id = indices[index];
    return id < section.palette.size() ? section.palette[id].key : "";
}

auto packed_height(const ::refrain::storage::heightmap_record &record,
                   std::size_t index) -> std::optional<std::int64_t> {
    constexpr std::size_t per_long = 64U / 9U;
    const auto word = index / per_long;
    const auto offset = (index % per_long) * 9U;
    if (word >= record.data.size()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>((record.data[word] >> offset) & 0x1FFU);
}

auto section_for_y(const chunk &value, std::int8_t section_y)
    -> const ::refrain::storage::section_record * {
    for (const auto &section : value.sections) {
        if (section.y == section_y) {
            return &section;
        }
    }
    return nullptr;
}

auto same_block_contents(const chunk &left, const chunk &right) -> bool {
    for (const auto &section : left.sections) {
        const auto *other = section_for_y(right, section.y);
        if (other == nullptr) {
            return false;
        }
        for (std::size_t index = 0; index < 4096; ++index) {
            if (paletted_state(section, index) !=
                paletted_state(*other, index)) {
                return false;
            }
        }
    }
    return true;
}

auto same_heightmap(const chunk &left, const chunk &right,
                    std::string_view name) -> bool {
    const auto find = [&](const chunk &value) {
        for (const auto &record : value.heightmaps) {
            if (record.name == name) {
                return &record;
            }
        }
        return static_cast<const ::refrain::storage::heightmap_record *>(
            nullptr);
    };
    const auto *a = find(left);
    const auto *b = find(right);
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    for (std::size_t index = 0; index < 256; ++index) {
        if (packed_height(*a, index) != packed_height(*b, index)) {
            return false;
        }
    }
    return true;
}

} // namespace

suite<"worldgen/proto-bridge"> proto_bridge_tests = [] {
    "a generated column becomes a storage chunk"_test = [] {
        auto states = make_registry();
        worldgen::proto_chunk source{states, 0, 0, -64, 384};
        const auto stone = *states.find("minecraft:stone");
        const auto water = *states.find("minecraft:water");
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = -64; y <= 9; ++y) {
                    source.set_state(x, y, z, stone);
                }
                source.set_state(x, 10, z, water);
            }
        }
        source.set_status(worldgen::chunk_status::terrain);
        source.set_biome(0, 0, 0, 0);
        source.set_block_entity(
            worldgen::block_pos{.x = 2, .y = 10, .z = 3},
            data::nbt::compound{
                {"id", data::nbt::value{std::string{"minecraft:chest"}}}});
        source.mark_post_processing(1, 10, 1);

        const auto converted = from_proto_chunk(source, {"minecraft:plains"});
        expect(converted.has_value());
        if (!converted) {
            return;
        }
        expect(converted->sections.size() == 24U);
        expect(converted->y_pos == -4);
        const auto &bottom = converted->sections.front();
        expect(bottom.y == -4);
        expect(paletted_state(bottom, 0) == "minecraft:stone");
        expect(converted->sections[15].y == 11);
        expect(paletted_state(converted->sections[15], 0) == "minecraft:air");
        expect(bottom.block_count == 16U * 16U * 16U);
        expect(bottom.biome_palette.size() == 1U);
        expect(bottom.biome_palette.front() == "minecraft:plains");

        bool world_surface = false;
        bool ocean_floor = false;
        for (const auto &record : converted->heightmaps) {
            if (record.name == "WORLD_SURFACE") {
                world_surface = true;
                expect(packed_height(record, 0) == 75); // 11 - (-64)
            } else if (record.name == "OCEAN_FLOOR") {
                ocean_floor = true;
                expect(packed_height(record, 0) == 74); // 10 - (-64)
            }
        }
        expect(world_surface);
        expect(ocean_floor);
        expect(converted->block_entities.size() == 1U);
        expect(converted->post_processing[4].size() == 1U);
        expect(converted->post_processing[4].front() ==
               static_cast<std::int16_t>(1 | (10 << 4) | (1 << 8)));

        const auto payload = encode_chunk(*converted, nullptr, true,
                                          chunk_palette_mode::inline_keys);
        expect(payload.has_value());
        if (payload) {
            const auto decoded = decode_chunk(*payload);
            expect(decoded.has_value());
            if (decoded) {
                expect(decoded->sections.size() == converted->sections.size());
                expect(world::validate(*decoded).has_value());
            }
        }
    };

    "the converted column round trips through the store"_test = [] {
        auto states = make_registry();
        worldgen::proto_chunk source{states, 0, 0, -64, 384};
        const auto stone = *states.find("minecraft:stone");
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = -64; y < 5; ++y) {
                    source.set_state(x, y, z, stone);
                }
            }
        }
        source.set_status(worldgen::chunk_status::terrain);
        const auto converted = from_proto_chunk(source, {"minecraft:plains"});
        expect(converted.has_value());
        if (!converted) {
            return;
        }
        const refrain::test_support::scratch_directory scratch;
        const auto root = scratch.path() / "store";
        auto store = world_store::create(root.string());
        expect(store.has_value());
        if (!store) {
            return;
        }
        data::nbt::compound level{};
        level["DataVersion"] = data::nbt::value{std::int32_t{5023}};
        data::nbt::compound meta{};
        meta["level"] = data::nbt::value{std::move(level)};
        const auto meta_bytes =
            data::nbt::write(data::nbt::value{std::move(meta)}, "",
                             data::nbt::compression::none);
        expect(meta_bytes.has_value());
        if (!meta_bytes) {
            return;
        }
        expect(store->write_meta(*meta_bytes).has_value());
        expect(store->write_chunk({.x = 0, .z = 0}, *converted).has_value());
        const auto read = store->read_chunk({.x = 0, .z = 0});
        expect(read.has_value());
        if (read) {
            const auto before = semantic_hash(*converted);
            const auto after = semantic_hash(*read);
            expect(before.has_value() && after.has_value());
            if (before && after) {
                expect(*before == *after);
            }
            expect(read->sections.size() == 24U);
        }
        expect(store->commit().has_value());
        const auto export_path = scratch.path() / "anvil";
        const auto exported_result =
            anvil::export_world(root.string(), export_path.string(), true);
        expect(exported_result.has_value());
        if (!exported_result) {
            return;
        }
        {
            const auto imported_path = scratch.path() / "rwf";
            auto imported = anvil::import_world(export_path.string(),
                                                imported_path.string(), true);
            expect(imported.has_value());
            if (imported) {
                auto reopened = world_store::open(imported_path.string());
                expect(reopened.has_value());
                if (reopened) {
                    const auto back = reopened->read_chunk({.x = 0, .z = 0});
                    expect(back.has_value());
                    if (back) {
                        expect(same_block_contents(*converted, *back));
                        expect(
                            same_heightmap(*converted, *back, "WORLD_SURFACE"));
                    }
                }
            }
        }
    };
};
