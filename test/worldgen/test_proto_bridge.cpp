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

[[nodiscard]] auto entity_tag(std::string id, std::vector<double> pos,
                              std::vector<float> rotation)
    -> data::nbt::compound {
    data::nbt::compound entity{};
    entity["id"] = data::nbt::value{std::move(id)};
    std::vector<data::nbt::value> coordinates{};
    coordinates.reserve(pos.size());
    for (const auto value : pos) {
        coordinates.push_back(data::nbt::value{value});
    }
    entity["Pos"] = data::nbt::make_list(data::nbt::tag_type::double_,
                                         std::move(coordinates));
    std::vector<data::nbt::value> angles{};
    angles.reserve(rotation.size());
    for (const auto value : rotation) {
        angles.push_back(data::nbt::value{value});
    }
    entity["Rotation"] =
        data::nbt::make_list(data::nbt::tag_type::float_, std::move(angles));
    return entity;
}

// --- structure fixtures ---------------------------------------------------

using nbt_value = data::nbt::value;
using worldgen::structure_piece_record;
using worldgen::structure_start;

auto nbt_child(const data::nbt::compound &entries, std::string_view key)
    -> const nbt_value * {
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : &found->second;
}

auto packed_chunk_pos(std::int32_t x, std::int32_t z) -> std::int64_t {
    const auto low = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x));
    const auto high = static_cast<std::uint64_t>(static_cast<std::uint32_t>(z))
                      << 32U;
    return static_cast<std::int64_t>(low | high);
}

auto make_piece(std::string type, worldgen::structure::structure_bbox bounds,
                std::int32_t orientation) -> structure_piece_record {
    structure_piece_record piece{};
    piece.type = std::move(type);
    piece.min_x = bounds.min_x;
    piece.min_y = bounds.min_y;
    piece.min_z = bounds.min_z;
    piece.max_x = bounds.max_x;
    piece.max_y = bounds.max_y;
    piece.max_z = bounds.max_z;
    piece.orientation = orientation;
    piece.height_position = -1;
    piece.height_offset = 0;
    piece.integrity = 1.0F;
    piece.overwrite = true;
    return piece;
}

/// A shipwreck start whose single piece reaches from chunk (0, 0) into chunk
/// (1, 0): the column that owns the start can only place the crossing piece
/// in its neighbour, which is the continuation the stored references encode.
auto shipwreck_start() -> structure_start {
    structure_start start{};
    start.structure = "minecraft:shipwreck";
    start.step = "surface_structures";
    start.chunk_x = 0;
    start.chunk_z = 0;
    auto piece = make_piece("minecraft:shipwreck",
                            {.min_x = 10,
                             .min_y = 55,
                             .min_z = 12,
                             .max_x = 22,
                             .max_y = 64,
                             .max_z = 28},
                            2);
    piece.template_id = "minecraft:shipwreck/with_mast";
    piece.template_x = 10;
    piece.template_y = 55;
    piece.template_z = 12;
    piece.template_rotation = 1;
    piece.template_mirror = 0;
    piece.template_pivot_x = 4;
    piece.template_pivot_y = 0;
    piece.template_pivot_z = 15;
    piece.beached = false;
    start.min_x = piece.min_x;
    start.min_y = piece.min_y;
    start.min_z = piece.min_z;
    start.max_x = piece.max_x;
    start.max_y = piece.max_y;
    start.max_z = piece.max_z;
    start.pieces.push_back(std::move(piece));
    return start;
}

auto mineshaft_start() -> structure_start {
    structure_start start{};
    start.structure = "minecraft:mineshaft";
    start.step = "underground_structures";
    start.chunk_x = 0;
    start.chunk_z = 0;
    auto room = make_piece("minecraft:msroom",
                           {.min_x = 0,
                            .min_y = 40,
                            .min_z = 0,
                            .max_x = 7,
                            .max_y = 48,
                            .max_z = 8},
                           -1);
    room.mesa = true;
    room.two_floored = false;
    room.entrances.push_back(worldgen::structure::structure_bbox{.min_x = 0,
                                                                 .min_y = 41,
                                                                 .min_z = 8,
                                                                 .max_x = 2,
                                                                 .max_y = 44,
                                                                 .max_z = 8});
    auto corridor = make_piece("minecraft:mscorridor",
                               {.min_x = 0,
                                .min_y = 40,
                                .min_z = 8,
                                .max_x = 2,
                                .max_y = 42,
                                .max_z = 23},
                               0);
    corridor.mesa = true;
    corridor.sections = 3;
    corridor.rails = true;
    corridor.spider = true;
    start.min_x = 0;
    start.min_y = 40;
    start.min_z = 0;
    start.max_x = 7;
    start.max_y = 48;
    start.max_z = 23;
    start.pieces.push_back(std::move(room));
    start.pieces.push_back(std::move(corridor));
    return start;
}

auto jigsaw_start() -> structure_start {
    structure_start start{};
    start.structure = "minecraft:village_plains";
    start.step = "surface_structures";
    start.chunk_x = 0;
    start.chunk_z = 0;
    auto piece = make_piece("minecraft:jigsaw",
                            {.min_x = -2,
                             .min_y = 64,
                             .min_z = 5,
                             .max_x = 14,
                             .max_y = 70,
                             .max_z = 21},
                            -1);
    piece.template_x = -2;
    piece.template_y = 64;
    piece.template_z = 5;
    piece.template_rotation = 2;
    piece.template_mirror = 0;
    piece.template_pivot_x = 0;
    piece.template_pivot_y = 0;
    piece.template_pivot_z = 0;
    worldgen::structure::jigsaw_element_info element{};
    element.kind = worldgen::structure::pool_element_kind::single;
    element.projection = worldgen::structure::pool_projection::rigid;
    element.template_id =
        "minecraft:village/plains/houses/plains_small_house_1";
    worldgen::structure::jigsaw_piece_data data{};
    data.element = std::move(element);
    data.ground_level_delta = 1;
    data.liquid = worldgen::structure::liquid_settings::ignore_waterlogging;
    data.junctions.push_back(worldgen::structure::jigsaw_junction_info{
        .source_x = 4,
        .source_ground_y = 63,
        .source_z = 9,
        .delta_y = -2,
        .dest_projection =
            worldgen::structure::pool_projection::terrain_matching});
    piece.jigsaw = std::move(data);
    start.min_x = piece.min_x;
    start.min_y = piece.min_y;
    start.min_z = piece.min_z;
    start.max_x = piece.max_x;
    start.max_y = piece.max_y;
    start.max_z = piece.max_z;
    start.pieces.push_back(std::move(piece));
    return start;
}

/// The three starts plus references from the two horizontal neighbours; the
/// shipwreck piece crosses the x=16 border.
void fill_structure_fixture(worldgen::proto_chunk &chunk) {
    chunk.add_structure_start(shipwreck_start());
    chunk.add_structure_start(mineshaft_start());
    chunk.add_structure_start(jigsaw_start());
    chunk.add_structure_reference("minecraft:shipwreck", 1, 0, 1);
    chunk.add_structure_reference("minecraft:village_plains", -1, 0, 2);
    chunk.add_structure_reference("minecraft:mineshaft", 0, 1, 1);
}

auto step_of(std::string_view structure_id) -> std::string {
    return structure_id == "minecraft:mineshaft" ? "underground_structures"
                                                 : "surface_structures";
}

auto same_bounds(const worldgen::structure::structure_bbox &left,
                 const worldgen::structure::structure_bbox &right) -> bool {
    return left.min_x == right.min_x && left.min_y == right.min_y &&
           left.min_z == right.min_z && left.max_x == right.max_x &&
           left.max_y == right.max_y && left.max_z == right.max_z;
}

auto same_bounds_list(
    const std::vector<worldgen::structure::structure_bbox> &left,
    const std::vector<worldgen::structure::structure_bbox> &right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_bounds(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

auto same_piece(const structure_piece_record &left,
                const structure_piece_record &right) -> bool {
    if (left.type != right.type || left.min_x != right.min_x ||
        left.min_y != right.min_y || left.min_z != right.min_z ||
        left.max_x != right.max_x || left.max_y != right.max_y ||
        left.max_z != right.max_z || left.orientation != right.orientation ||
        left.template_id != right.template_id ||
        left.template_x != right.template_x ||
        left.template_y != right.template_y ||
        left.template_z != right.template_z ||
        left.template_rotation != right.template_rotation ||
        left.template_mirror != right.template_mirror ||
        left.template_pivot_x != right.template_pivot_x ||
        left.template_pivot_y != right.template_pivot_y ||
        left.template_pivot_z != right.template_pivot_z ||
        left.height_position != right.height_position ||
        left.height_offset != right.height_offset ||
        left.integrity != right.integrity || left.large != right.large ||
        left.beached != right.beached || left.overwrite != right.overwrite ||
        left.sections != right.sections || left.rails != right.rails ||
        left.spider != right.spider || left.two_floored != right.two_floored ||
        left.mesa != right.mesa ||
        !same_bounds_list(left.entrances, right.entrances) ||
        left.children.size() != right.children.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.children.size(); ++index) {
        if (!same_piece(left.children[index], right.children[index])) {
            return false;
        }
    }
    if (left.jigsaw.has_value() != right.jigsaw.has_value()) {
        return false;
    }
    if (left.jigsaw) {
        const auto &a = *left.jigsaw;
        const auto &b = *right.jigsaw;
        if (a.element.kind != b.element.kind ||
            a.element.projection != b.element.projection ||
            a.element.template_id != b.element.template_id ||
            a.element.processor_list != b.element.processor_list ||
            a.element.feature != b.element.feature ||
            a.ground_level_delta != b.ground_level_delta ||
            a.liquid != b.liquid || a.junctions != b.junctions) {
            return false;
        }
    }
    return true;
}

auto same_start(const structure_start &left, const structure_start &right)
    -> bool {
    if (left.structure != right.structure || left.step != right.step ||
        left.chunk_x != right.chunk_x || left.chunk_z != right.chunk_z ||
        left.min_x != right.min_x || left.min_y != right.min_y ||
        left.min_z != right.min_z || left.max_x != right.max_x ||
        left.max_y != right.max_y || left.max_z != right.max_z ||
        left.pieces.size() != right.pieces.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.pieces.size(); ++index) {
        if (!same_piece(left.pieces[index], right.pieces[index])) {
            return false;
        }
    }
    return true;
}

auto restored_starts(const chunk &stored) -> std::vector<structure_start> {
    worldgen::block_state_registry states{};
    (void)states.intern("minecraft:air", {.air = true});
    worldgen::proto_chunk restored{states, 0, 0, -64, 384};
    const auto status = restore_structures(stored, restored, step_of);
    expect(status.has_value());
    return std::vector<structure_start>{restored.structure_starts().begin(),
                                        restored.structure_starts().end()};
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

    "a generated column carries its template entities as entity records"_test =
        [] {
            auto states = make_registry();
            worldgen::proto_chunk source{states, 0, 0, -64, 384};
            source.set_status(worldgen::chunk_status::terrain);
            source.set_biome(0, 0, 0, 0);
            // The placement writes these tags: the entity type, the world
            // position, the rotated body angles and (when the template named
            // one) the block position, plus the template components.
            auto pig{
                entity_tag("minecraft:pig", {0.5, 65.0, 0.5}, {90.0F, 0.0F})};
            pig["CustomName"] = data::nbt::value{std::string{"Bob"}};
            source.add_entity(std::move(pig));
            auto shulker{entity_tag("minecraft:shulker", {2.5, 66.0, 3.5},
                                    {180.0F, 10.0F})};
            shulker["Color"] = data::nbt::value{std::int32_t{3}};
            shulker["block_pos"] = data::nbt::make_list(
                data::nbt::tag_type::int_, {data::nbt::value{std::int32_t{2}},
                                            data::nbt::value{std::int32_t{66}},
                                            data::nbt::value{std::int32_t{3}}});
            source.add_entity(std::move(shulker));

            const auto converted =
                from_proto_chunk(source, {"minecraft:plains"});
            expect(converted.has_value());
            if (!converted) {
                return;
            }
            expect(converted->entities.size() == 2U);
            if (converted->entities.size() != 2U) {
                return;
            }
            // Canonical order is position order: the pig sits west of the
            // shulker, so it comes first regardless of placement order.
            expect(converted->entities[0].id == "minecraft:pig");
            expect(converted->entities[0].x == 0.5);
            expect(converted->entities[0].y == 65.0);
            expect(converted->entities[0].z == 0.5);
            expect(converted->entities[0].yaw == 90.0F);
            expect(converted->entities[0].pos.x == 0);
            expect(converted->entities[0].pos.y == 65);
            expect(converted->entities[0].pos.z == 0);
            const auto pig_components{
                data::nbt::read(converted->entities[0].data)};
            expect(pig_components.has_value());
            if (pig_components) {
                const auto *name{pig_components->get("CustomName")};
                expect(name != nullptr &&
                       name->as_string() ==
                           std::optional<std::string_view>{"Bob"});
                expect(pig_components->get("id") == nullptr);
                expect(pig_components->get("Pos") == nullptr);
            }
            expect(converted->entities[1].id == "minecraft:shulker");
            expect(converted->entities[1].pitch == 10.0F);
            expect(converted->entities[1].pos.x == 2);
            expect(converted->entities[1].pos.z == 3);
            const auto shulker_components{
                data::nbt::read(converted->entities[1].data)};
            expect(shulker_components.has_value());
            if (shulker_components) {
                expect(shulker_components->get("block_pos") == nullptr);
                const auto *colour{shulker_components->get("Color")};
                expect(colour != nullptr &&
                       colour->as_int() == std::optional<std::int32_t>{3});
            }

            // Two generation runs that place the same entities in a
            // different order store the same bytes: the bridge canonises the
            // order, so a saved-and-reloaded column hashes identically.
            auto reversed_states = make_registry();
            worldgen::proto_chunk reversed{reversed_states, 0, 0, -64, 384};
            reversed.set_status(worldgen::chunk_status::terrain);
            reversed.set_biome(0, 0, 0, 0);
            auto reversed_shulker{entity_tag(
                "minecraft:shulker", {2.5, 66.0, 3.5}, {180.0F, 10.0F})};
            reversed_shulker["Color"] = data::nbt::value{std::int32_t{3}};
            reversed_shulker["block_pos"] = data::nbt::make_list(
                data::nbt::tag_type::int_, {data::nbt::value{std::int32_t{2}},
                                            data::nbt::value{std::int32_t{66}},
                                            data::nbt::value{std::int32_t{3}}});
            reversed.add_entity(std::move(reversed_shulker));
            auto reversed_pig{
                entity_tag("minecraft:pig", {0.5, 65.0, 0.5}, {90.0F, 0.0F})};
            reversed_pig["CustomName"] = data::nbt::value{std::string{"Bob"}};
            reversed.add_entity(std::move(reversed_pig));
            const auto reversed_converted =
                from_proto_chunk(reversed, {"minecraft:plains"});
            expect(reversed_converted.has_value());
            if (reversed_converted) {
                expect(semantic_hash(*converted).value_or("") ==
                       semantic_hash(*reversed_converted).value_or(""));
            }

            // The records survive the RWF codec.
            const auto payload = encode_chunk(*converted, nullptr, true,
                                              chunk_palette_mode::inline_keys);
            expect(payload.has_value());
            if (!payload) {
                return;
            }
            const auto decoded = decode_chunk(*payload);
            expect(decoded.has_value());
            if (decoded) {
                expect(decoded->entities.size() == 2U);
                expect(decoded->entities[0].id == "minecraft:pig");
                expect(decoded->entities[0].data ==
                       converted->entities[0].data);
            }
        };

    "a malformed template entity record fails the conversion"_test = [] {
        auto states = make_registry();
        const auto convert = [&states](data::nbt::compound entity) {
            worldgen::proto_chunk source{states, 0, 0, -64, 384};
            source.set_status(worldgen::chunk_status::terrain);
            source.add_entity(std::move(entity));
            return from_proto_chunk(source, {"minecraft:plains"});
        };
        // No type id: the record cannot become a persisted entity.
        auto missing{convert(data::nbt::compound{
            {"Pos", data::nbt::make_list(data::nbt::tag_type::double_,
                                         {data::nbt::value{0.5},
                                          data::nbt::value{65.0},
                                          data::nbt::value{0.5}})}})};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::parse_error);
        }
        // A non-numeric position is equally unrepresentable.
        data::nbt::compound bad_pos{};
        bad_pos["id"] = data::nbt::value{std::string{"minecraft:pig"}};
        bad_pos["Pos"] = data::nbt::value{std::string{"not-a-position"}};
        auto malformed{convert(std::move(bad_pos))};
        expect(!malformed);
        if (!malformed) {
            expect(malformed.error().code == refrain::error_code::parse_error);
        }
        // A short rotation list is a malformed record, not a default.
        data::nbt::compound bad_rotation{
            {"id", data::nbt::value{std::string{"minecraft:pig"}}},
            {"Pos", data::nbt::make_list(data::nbt::tag_type::double_,
                                         {data::nbt::value{0.5},
                                          data::nbt::value{65.0},
                                          data::nbt::value{0.5}})},
            {"Rotation", data::nbt::make_list(data::nbt::tag_type::float_,
                                              {data::nbt::value{90.0F}})}};
        auto bad_angles{convert(std::move(bad_rotation))};
        expect(!bad_angles);
        if (!bad_angles) {
            expect(bad_angles.error().code == refrain::error_code::parse_error);
        }
    };

    "a column stores its structure starts and references"_test = [&] {
        auto states = make_registry();
        worldgen::proto_chunk source{states, 0, 0, -64, 384};
        source.set_status(worldgen::chunk_status::terrain);
        fill_structure_fixture(source);

        const auto converted = from_proto_chunk(source, {"minecraft:plains"});
        expect(converted.has_value());
        if (!converted) {
            return;
        }
        expect(!converted->structures.empty());

        const auto document = data::nbt::read(converted->structures);
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto *root = document->as_compound();
        expect(root != nullptr);
        if (root == nullptr) {
            return;
        }
        const auto *starts = nbt_child(*root, "starts");
        const auto *references = nbt_child(*root, "References");
        expect(starts != nullptr && starts->as_compound() != nullptr);
        expect(references != nullptr && references->as_compound() != nullptr);
        if (starts == nullptr || references == nullptr) {
            return;
        }
        const auto &start_entries = *starts->as_compound();
        const auto &reference_entries = *references->as_compound();
        expect(start_entries.size() == 3U);
        expect(reference_entries.size() == 3U);

        // The shipwreck start keeps its vanilla template piece tag.
        const auto wreck = start_entries.find("minecraft:shipwreck");
        expect(wreck != start_entries.end());
        if (wreck != start_entries.end()) {
            const auto &tag = *wreck->second.as_compound();
            expect(tag.at("id").as_string() ==
                   std::optional<std::string_view>{"minecraft:shipwreck"});
            expect(tag.at("ChunkX").as_int() == std::optional<std::int32_t>{0});
            expect(tag.at("ChunkZ").as_int() == std::optional<std::int32_t>{0});
            const auto *children = nbt_child(tag, "Children");
            expect(children != nullptr && children->as_list() != nullptr);
            if (children != nullptr && children->as_list() != nullptr) {
                const auto &list = *children->as_list();
                expect(list.items.size() == 1U);
                if (!list.items.empty()) {
                    const auto &piece = *list.items.front().as_compound();
                    expect(
                        piece.at("id").as_string() ==
                        std::optional<std::string_view>{"minecraft:shipwreck"});
                    expect(piece.at("Template").as_string() ==
                           std::optional<std::string_view>{
                               "minecraft:shipwreck/with_mast"});
                    expect(piece.at("Rot").as_string() ==
                           std::optional<std::string_view>{"CLOCKWISE_90"});
                    expect(piece.at("TPX").as_int() ==
                           std::optional<std::int32_t>{10});
                    expect(piece.at("isBeached").as_byte() ==
                           std::optional<std::int8_t>{0});
                    const auto box = piece.at("BB").as_int_array();
                    expect(box.has_value() && box->size() == 6U);
                    if (box && box->size() == 6U) {
                        expect((*box)[0] == 10 && (*box)[3] == 22);
                    }
                }
            }
        }

        // The mineshaft start keeps its procedural piece tags.
        const auto shaft = start_entries.find("minecraft:mineshaft");
        expect(shaft != start_entries.end());
        if (shaft != start_entries.end()) {
            const auto &tag = *shaft->second.as_compound();
            const auto *children = nbt_child(tag, "Children");
            expect(children != nullptr && children->as_list() != nullptr);
            if (children != nullptr && children->as_list() != nullptr) {
                const auto &list = *children->as_list();
                expect(list.items.size() == 2U);
                if (list.items.size() == 2U) {
                    const auto &room = *list.items[0].as_compound();
                    const auto &corridor = *list.items[1].as_compound();
                    expect(room.at("id").as_string() ==
                           std::optional<std::string_view>{"minecraft:msroom"});
                    expect(room.at("MST").as_int() ==
                           std::optional<std::int32_t>{1});
                    expect(room.at("Entrances").as_list() != nullptr);
                    expect(corridor.at("id").as_string() ==
                           std::optional<std::string_view>{
                               "minecraft:mscorridor"});
                    expect(corridor.at("Num").as_int() ==
                           std::optional<std::int32_t>{3});
                    expect(corridor.at("hr").as_byte() ==
                           std::optional<std::int8_t>{1});
                    expect(corridor.at("sc").as_byte() ==
                           std::optional<std::int8_t>{1});
                }
            }
        }

        // The jigsaw start keeps its pool element and junction list.
        const auto village = start_entries.find("minecraft:village_plains");
        expect(village != start_entries.end());
        if (village != start_entries.end()) {
            const auto &tag = *village->second.as_compound();
            const auto *children = nbt_child(tag, "Children");
            expect(children != nullptr && children->as_list() != nullptr);
            if (children != nullptr && children->as_list() != nullptr &&
                !children->as_list()->items.empty()) {
                const auto &piece =
                    *children->as_list()->items.front().as_compound();
                expect(piece.at("id").as_string() ==
                       std::optional<std::string_view>{"minecraft:jigsaw"});
                expect(piece.at("PosX").as_int() ==
                       std::optional<std::int32_t>{-2});
                expect(piece.at("rotation").as_string() ==
                       std::optional<std::string_view>{"CLOCKWISE_180"});
                expect(piece.at("ground_level_delta").as_int() ==
                       std::optional<std::int32_t>{1});
                const auto *element = nbt_child(piece, "pool_element");
                expect(element != nullptr && element->as_compound() != nullptr);
                if (element != nullptr && element->as_compound() != nullptr) {
                    const auto &entries = *element->as_compound();
                    expect(entries.at("element_type").as_string() ==
                           std::optional<std::string_view>{
                               "minecraft:single_pool_element"});
                    expect(entries.at("projection").as_string() ==
                           std::optional<std::string_view>{"rigid"});
                }
                const auto *junctions = nbt_child(piece, "junctions");
                expect(junctions != nullptr && junctions->as_list() != nullptr);
                if (junctions != nullptr && junctions->as_list() != nullptr) {
                    expect(junctions->as_list()->items.size() == 1U);
                }
                expect(piece.at("liquid_settings").as_string() ==
                       std::optional<std::string_view>{"ignore_waterlogging"});
            }
        }

        // The references keep one packed position per reference count.
        const auto packed_of = [&](std::string_view id)
            -> std::optional<std::vector<std::int64_t>> {
            const auto found = reference_entries.find(id);
            if (found == reference_entries.end()) {
                return std::nullopt;
            }
            const auto values = found->second.as_long_array();
            if (!values) {
                return std::nullopt;
            }
            return std::vector<std::int64_t>{values->begin(), values->end()};
        };
        const auto wreck_references = packed_of("minecraft:shipwreck");
        expect(wreck_references.has_value());
        if (wreck_references) {
            expect(wreck_references->size() == 1U);
            expect((*wreck_references)[0] == packed_chunk_pos(1, 0));
        }
        const auto village_references = packed_of("minecraft:village_plains");
        expect(village_references.has_value());
        if (village_references) {
            expect(village_references->size() == 2U);
            expect((*village_references)[0] == packed_chunk_pos(-1, 0));
            expect((*village_references)[1] == packed_chunk_pos(-1, 0));
        }

        // A column without structure records keeps the field empty so its
        // semantic hash is unchanged.
        worldgen::proto_chunk plain{states, 0, 0, -64, 384};
        plain.set_status(worldgen::chunk_status::terrain);
        const auto plain_column = from_proto_chunk(plain, {"minecraft:plains"});
        expect(plain_column.has_value());
        if (plain_column) {
            expect(plain_column->structures.empty());
        }

        // The stored records decode back into the generated ones.
        auto restored = restored_starts(*converted);
        expect(restored.size() == 3U);
        std::map<std::string, structure_start> by_id{};
        for (auto &start : restored) {
            by_id.emplace(start.structure, std::move(start));
        }
        expect(by_id.size() == 3U);
        expect(same_start(by_id["minecraft:shipwreck"], shipwreck_start()));
        expect(same_start(by_id["minecraft:mineshaft"], mineshaft_start()));
        expect(same_start(by_id["minecraft:village_plains"], jigsaw_start()));
    };

    "structure records survive the store and an Anvil round trip"_test = [&] {
        auto states = make_registry();
        worldgen::proto_chunk source{states, 0, 0, -64, 384};
        source.set_status(worldgen::chunk_status::terrain);
        fill_structure_fixture(source);
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
        if (!read) {
            return;
        }
        expect(read->structures == converted->structures);
        const auto before = semantic_hash(*converted);
        const auto after = semantic_hash(*read);
        expect(before.has_value() && after.has_value());
        if (before && after) {
            expect(*before == *after);
        }
        expect(store->commit().has_value());

        const auto export_path = scratch.path() / "anvil";
        const auto exported =
            anvil::export_world(root.string(), export_path.string(), true);
        expect(exported.has_value());
        if (!exported) {
            return;
        }
        const auto imported_path = scratch.path() / "rwf";
        const auto imported = anvil::import_world(export_path.string(),
                                                  imported_path.string(), true);
        expect(imported.has_value());
        if (!imported) {
            return;
        }
        auto reopened = world_store::open(imported_path.string());
        expect(reopened.has_value());
        if (!reopened) {
            return;
        }
        const auto back = reopened->read_chunk({.x = 0, .z = 0});
        expect(back.has_value());
        if (!back) {
            return;
        }
        // The Anvil import/export path carries the compound byte for byte, so
        // the exported chunk declares the same starts a vanilla reader accepts.
        expect(back->structures == converted->structures);
        const auto restored = restored_starts(*back);
        expect(restored.size() == 3U);
        std::map<std::string, structure_start> by_id{};
        for (const auto &start : restored) {
            by_id.emplace(start.structure, start);
        }
        expect(by_id.size() == 3U);
        expect(same_start(by_id["minecraft:shipwreck"], shipwreck_start()));
        expect(same_start(by_id["minecraft:mineshaft"], mineshaft_start()));
        expect(same_start(by_id["minecraft:village_plains"], jigsaw_start()));
        std::map<std::pair<std::string, std::pair<std::int32_t, std::int32_t>>,
                 std::int32_t>
            expected_references{};
        expected_references[{"minecraft:shipwreck", {1, 0}}] = 1;
        expected_references[{"minecraft:village_plains", {-1, 0}}] = 2;
        expected_references[{"minecraft:mineshaft", {0, 1}}] = 1;
        std::map<std::pair<std::string, std::pair<std::int32_t, std::int32_t>>,
                 std::int32_t>
            actual_references{};
        {
            worldgen::block_state_registry restored_states{};
            (void)restored_states.intern("minecraft:air", {.air = true});
            worldgen::proto_chunk restored_chunk{restored_states, 0, 0, -64,
                                                 384};
            expect(
                restore_structures(*back, restored_chunk, step_of).has_value());
            for (const auto &reference :
                 restored_chunk.structure_references()) {
                actual_references[{reference.structure,
                                   {reference.chunk_x, reference.chunk_z}}] =
                    reference.count;
            }
        }
        const bool references_match = actual_references == expected_references;
        expect(references_match);
    };

    "a corrupt stored structure payload fails the reverse bridge"_test = [&] {
        auto states = make_registry();
        worldgen::proto_chunk source{states, 0, 0, -64, 384};
        source.set_status(worldgen::chunk_status::terrain);
        fill_structure_fixture(source);
        const auto converted = from_proto_chunk(source, {"minecraft:plains"});
        expect(converted.has_value());
        if (!converted) {
            return;
        }
        const auto restore = [&](const chunk &stored) {
            worldgen::block_state_registry restored_states{};
            (void)restored_states.intern("minecraft:air", {.air = true});
            worldgen::proto_chunk restored{restored_states, 0, 0, -64, 384};
            return restore_structures(stored, restored, step_of);
        };
        // Truncated NBT is not a compound.
        {
            auto corrupt = *converted;
            corrupt.structures = {std::byte{0x0A}};
            const auto restored = restore(corrupt);
            expect(!restored);
            if (!restored) {
                expect(restored.error().code ==
                       refrain::error_code::parse_error);
            }
        }
        // A compound whose starts entry has the wrong shape is rejected
        // instead of being treated as an empty structure list.
        {
            data::nbt::compound bad_root{};
            bad_root["starts"] = data::nbt::make_int(1);
            const auto bytes =
                data::nbt::write(data::nbt::value{std::move(bad_root)}, "",
                                 data::nbt::compression::none);
            expect(bytes.has_value());
            if (!bytes) {
                return;
            }
            auto corrupt = *converted;
            corrupt.structures = *bytes;
            const auto restored = restore(corrupt);
            expect(!restored);
            if (!restored) {
                expect(restored.error().code ==
                       refrain::error_code::parse_error);
            }
        }
        // An unknown piece id is malformed for the generated record model.
        {
            auto document = data::nbt::read(converted->structures);
            expect(document.has_value());
            if (!document || document->as_compound() == nullptr) {
                return;
            }
            auto *starts = (*document->as_compound())["starts"].as_compound();
            expect(starts != nullptr);
            if (starts == nullptr) {
                return;
            }
            auto &wreck = (*starts)["minecraft:shipwreck"];
            auto *tag = wreck.as_compound();
            expect(tag != nullptr);
            if (tag == nullptr) {
                return;
            }
            auto *children = (*tag)["Children"].as_list();
            expect(children != nullptr && !children->items.empty());
            if (children == nullptr || children->items.empty()) {
                return;
            }
            auto *piece = children->items.front().as_compound();
            expect(piece != nullptr);
            if (piece == nullptr) {
                return;
            }
            (*piece)["id"] = data::nbt::make_string("minecraft:unknown_piece");
            const auto bytes =
                data::nbt::write(*document, "", data::nbt::compression::none);
            expect(bytes.has_value());
            if (!bytes) {
                return;
            }
            auto corrupt = *converted;
            corrupt.structures = *bytes;
            const auto restored = restore(corrupt);
            expect(!restored);
            if (!restored) {
                expect(restored.error().code ==
                       refrain::error_code::parse_error);
            }
        }
    };

    "a record the compound cannot represent fails the conversion"_test = [&] {
        auto states = make_registry();
        const auto convert = [&](const worldgen::proto_chunk &chunk) {
            return from_proto_chunk(chunk, {"minecraft:plains"});
        };
        // A piece type the vanilla piece registry does not name is rejected
        // rather than dropped from the stored start.
        {
            worldgen::proto_chunk source{states, 0, 0, -64, 384};
            structure_start start{};
            start.structure = "minecraft:test";
            start.step = "surface_structures";
            auto piece = make_piece("minecraft:unknown",
                                    {.min_x = 0,
                                     .min_y = 64,
                                     .min_z = 0,
                                     .max_x = 3,
                                     .max_y = 70,
                                     .max_z = 3},
                                    -1);
            start.pieces.push_back(std::move(piece));
            source.add_structure_start(std::move(start));
            const auto converted = convert(source);
            expect(!converted);
            if (!converted) {
                expect(converted.error().code ==
                       refrain::error_code::parse_error);
            }
        }
        // Two starts of one structure cannot share a column: the compound is
        // keyed by structure id and the reference keeps one start per id.
        {
            worldgen::proto_chunk source{states, 0, 0, -64, 384};
            source.add_structure_start(shipwreck_start());
            source.add_structure_start(shipwreck_start());
            const auto converted = convert(source);
            expect(!converted);
            if (!converted) {
                expect(converted.error().code ==
                       refrain::error_code::parse_error);
            }
        }
        // An empty structure id has no compound key.
        {
            worldgen::proto_chunk source{states, 0, 0, -64, 384};
            structure_start start{};
            start.step = "surface_structures";
            start.pieces.push_back(make_piece("minecraft:btp",
                                              {.min_x = 0,
                                               .min_y = 0,
                                               .min_z = 0,
                                               .max_x = 0,
                                               .max_y = 0,
                                               .max_z = 0},
                                              -1));
            source.add_structure_start(std::move(start));
            const auto converted = convert(source);
            expect(!converted);
            if (!converted) {
                expect(converted.error().code ==
                       refrain::error_code::parse_error);
            }
        }
    };
};
