// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>

namespace {

using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::structure;

auto ints(std::initializer_list<jint> values) -> data::nbt::value {
    auto list = data::nbt::make_list(data::nbt::tag_type::int_);
    for (const auto value : values) {
        (void)list.push(data::nbt::value{value});
    }
    return list;
}

auto doubles(std::initializer_list<double> values) -> data::nbt::value {
    auto list = data::nbt::make_list(data::nbt::tag_type::double_);
    for (const auto value : values) {
        (void)list.push(data::nbt::value{value});
    }
    return list;
}

auto palette_entry(std::string name, data::nbt::compound properties = {})
    -> data::nbt::compound {
    data::nbt::compound entry;
    entry["Name"] = data::nbt::value{std::move(name)};
    if (!properties.empty()) {
        entry["Properties"] = data::nbt::value{std::move(properties)};
    }
    return entry;
}

auto block_entry(std::initializer_list<jint> pos, jint state,
                 data::nbt::compound nbt = {}) -> data::nbt::compound {
    data::nbt::compound entry;
    entry["pos"] = ints(pos);
    entry["state"] = data::nbt::value{state};
    if (!nbt.empty()) {
        entry["nbt"] = data::nbt::value{std::move(nbt)};
    }
    return entry;
}

} // namespace

using namespace boost::ut;

suite<"worldgen/template"> template_tests = [] {
    "a template groups and sorts blocks and keeps block entity tags"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto stairs =
            registry.intern("minecraft:oak_stairs[facing=east,half=bottom,"
                            "shape=straight,waterlogged=false]",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        auto chest = registry.intern(
            "minecraft:chest[facing=north,type=single,waterlogged=false]",
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true});
        expect(stone && stairs && chest);
        if (!stone || !stairs || !chest) {
            return;
        }

        data::nbt::compound root;
        root["size"] = ints({3, 2, 2});

        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
        data::nbt::compound stair_properties;
        stair_properties["facing"] = data::nbt::value{std::string{"east"}};
        stair_properties["half"] = data::nbt::value{std::string{"bottom"}};
        stair_properties["shape"] = data::nbt::value{std::string{"straight"}};
        stair_properties["waterlogged"] =
            data::nbt::value{std::string{"false"}};
        (void)palette.push(data::nbt::value{palette_entry(
            "minecraft:oak_stairs", std::move(stair_properties))});
        data::nbt::compound chest_properties;
        chest_properties["facing"] = data::nbt::value{std::string{"north"}};
        chest_properties["type"] = data::nbt::value{std::string{"single"}};
        chest_properties["waterlogged"] =
            data::nbt::value{std::string{"false"}};
        (void)palette.push(data::nbt::value{
            palette_entry("minecraft:chest", std::move(chest_properties))});
        root["palette"] = std::move(palette);

        data::nbt::compound chest_nbt;
        chest_nbt["id"] = data::nbt::value{std::string{"minecraft:chest"}};
        chest_nbt["LootTable"] =
            data::nbt::value{std::string{"minecraft:chests/simple_dungeon"}};
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({1, 1, 0}, 0)});
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        (void)blocks.push(
            data::nbt::value{block_entry({2, 0, 1}, 2, chest_nbt)});
        (void)blocks.push(data::nbt::value{block_entry({1, 0, 1}, 1)});
        (void)blocks.push(data::nbt::value{block_entry({0, 1, 1}, 1)});
        root["blocks"] = std::move(blocks);

        auto entities = data::nbt::make_list(data::nbt::tag_type::compound);
        data::nbt::compound entity_nbt;
        entity_nbt["id"] = data::nbt::value{std::string{"minecraft:villager"}};
        data::nbt::compound entity;
        entity["pos"] = doubles({1.5, 0.0, 0.5});
        entity["blockPos"] = ints({1, 0, 0});
        entity["nbt"] = data::nbt::value{entity_nbt};
        (void)entities.push(data::nbt::value{std::move(entity)});
        data::nbt::compound bare_entity;
        bare_entity["pos"] = doubles({0.0, 0.0, 0.0});
        bare_entity["blockPos"] = ints({0, 0, 0});
        (void)entities.push(data::nbt::value{std::move(bare_entity)});
        root["entities"] = std::move(entities);

        const auto loaded =
            load_structure_template(registry, data::nbt::value{root});
        expect(loaded.has_value());
        if (!loaded) {
            return;
        }
        expect(loaded->size_x == 3);
        expect(loaded->size_y == 2);
        expect(loaded->size_z == 2);
        expect(loaded->palettes.size() == 1U);
        if (loaded->palettes.size() != 1U) {
            return;
        }
        const auto &order = loaded->palettes.front();
        expect(order.size() == 5U);
        if (order.size() != 5U) {
            return;
        }
        // Full blocks sorted by y, x, z, then the block entity.
        expect(order[0].x == 0 && order[0].y == 0 && order[0].z == 0);
        expect(order[1].x == 1 && order[1].y == 0 && order[1].z == 1);
        expect(order[2].x == 0 && order[2].y == 1 && order[2].z == 1);
        expect(order[3].x == 1 && order[3].y == 1 && order[3].z == 0);
        expect(order[4].x == 2 && order[4].y == 0 && order[4].z == 1);
        expect(order[4].nbt != nullptr);
        if (order[4].nbt != nullptr) {
            const auto id = order[4].nbt->find("id");
            expect(id != order[4].nbt->end());
            if (id != order[4].nbt->end()) {
                expect(id->second.as_string() == "minecraft:chest");
            }
        }
        expect(order[0].state == *stone);
        expect(order[1].state == *stairs);
        expect(order[2].state == *stairs);
        expect(order[3].state == *stone);
        expect(loaded->entities.size() == 1U);
        if (!loaded->entities.empty()) {
            expect(loaded->entities[0].x == 1.5);
            expect(loaded->entities[0].block_x == 1);
        }
    };

    "mirror and rotation transform positions around the pivot"_test = [] {
        const template_transform none{};
        const auto identity =
            transform_position(block_pos{.x = 1, .y = 2, .z = 3}, none);
        expect(identity.x == 1 && identity.y == 2 && identity.z == 3);

        template_transform left_right{};
        left_right.mirror = template_mirror::left_right;
        const auto mirrored =
            transform_position(block_pos{.x = 1, .y = 2, .z = 3}, left_right);
        expect(mirrored.x == 1 && mirrored.z == -3);

        template_transform front_back{};
        front_back.mirror = template_mirror::front_back;
        const auto flipped =
            transform_position(block_pos{.x = 1, .y = 2, .z = 3}, front_back);
        expect(flipped.x == -1 && flipped.z == 3);

        template_transform clockwise{};
        clockwise.rotation = template_rotation::clockwise_90;
        const auto rotated =
            transform_position(block_pos{.x = 1, .y = 2, .z = 3}, clockwise);
        expect(rotated.x == -3 && rotated.y == 2 && rotated.z == 1);

        template_transform counter{};
        counter.rotation = template_rotation::counterclockwise_90;
        const auto counter_rotated =
            transform_position(block_pos{.x = 1, .y = 2, .z = 3}, counter);
        expect(counter_rotated.x == 3 && counter_rotated.z == -1);

        template_transform half_turn{};
        half_turn.rotation = template_rotation::clockwise_180;
        const auto reversed =
            transform_position(block_pos{.x = 1, .y = 2, .z = 3}, half_turn);
        expect(reversed.x == -1 && reversed.z == -3);

        template_transform pivoted{};
        pivoted.rotation = template_rotation::clockwise_90;
        pivoted.pivot = block_pos{.x = 5, .y = 0, .z = 7};
        const auto around_pivot =
            transform_position(block_pos{.x = 1, .y = 2, .z = 3}, pivoted);
        expect(around_pivot.x == 9 && around_pivot.y == 2 &&
               around_pivot.z == 3);
    };

    "mirror then rotation transforms the common state properties"_test = [] {
        block_state_registry registry;
        auto furnace =
            registry.intern("minecraft:furnace[facing=north,lit=false]",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        auto pillar = registry.intern("minecraft:oak_log[axis=x]",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
        auto sign =
            registry.intern("minecraft:oak_sign[rotation=0,waterlogged=false]",
                            {.air = false,
                             .blocks_motion = false,
                             .blocks_motion_no_leaves = false});
        auto vine = registry.intern("minecraft:vine[east=true,north=false,"
                                    "south=false,up=false,west=false]",
                                    {.air = false,
                                     .blocks_motion = false,
                                     .blocks_motion_no_leaves = false});
        expect(furnace && pillar && sign && vine);
        if (!furnace || !pillar || !sign || !vine) {
            return;
        }

        template_transform clockwise{};
        clockwise.rotation = template_rotation::clockwise_90;
        const auto turned = transform_state(registry, *furnace, clockwise);
        expect(registry.name(turned) ==
               "minecraft:furnace[facing=east,lit=false]");

        template_transform left_right{};
        left_right.mirror = template_mirror::left_right;
        const auto mirrored = transform_state(registry, *furnace, left_right);
        expect(registry.name(mirrored) ==
               "minecraft:furnace[facing=south,lit=false]");

        template_transform half_turn{};
        half_turn.rotation = template_rotation::clockwise_180;
        const auto axis_turned = transform_state(registry, *pillar, half_turn);
        expect(registry.name(axis_turned) == "minecraft:oak_log[axis=x]");
        const auto axis_quarter = transform_state(registry, *pillar, clockwise);
        expect(registry.name(axis_quarter) == "minecraft:oak_log[axis=z]");

        const auto sign_turned = transform_state(registry, *sign, clockwise);
        expect(registry.name(sign_turned) ==
               "minecraft:oak_sign[rotation=4,waterlogged=false]");
        const auto sign_mirrored = transform_state(registry, *sign, left_right);
        expect(registry.name(sign_mirrored) ==
               "minecraft:oak_sign[rotation=8,waterlogged=false]");

        const auto vine_turned = transform_state(registry, *vine, clockwise);
        expect(registry.name(vine_turned) ==
               "minecraft:vine[south=true,east=false,west=false,up=false,north="
               "false]");
    };

    "placement writes transformed blocks and block entities"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto chest = registry.intern(
            "minecraft:chest[facing=north,type=single,waterlogged=false]",
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true});
        expect(stone && chest);
        if (!stone || !chest) {
            return;
        }

        data::nbt::compound root;
        root["size"] = ints({2, 1, 1});
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
        data::nbt::compound chest_properties;
        chest_properties["facing"] = data::nbt::value{std::string{"north"}};
        chest_properties["type"] = data::nbt::value{std::string{"single"}};
        chest_properties["waterlogged"] =
            data::nbt::value{std::string{"false"}};
        (void)palette.push(data::nbt::value{
            palette_entry("minecraft:chest", std::move(chest_properties))});
        root["palette"] = std::move(palette);

        data::nbt::compound chest_nbt;
        chest_nbt["id"] = data::nbt::value{std::string{"minecraft:chest"}};
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        (void)blocks.push(
            data::nbt::value{block_entry({1, 0, 0}, 1, chest_nbt)});
        root["blocks"] = std::move(blocks);

        const auto loaded =
            load_structure_template(registry, data::nbt::value{root});
        expect(loaded.has_value());
        if (!loaded) {
            return;
        }

        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        template_transform identity{};
        expect(place_template(region, registry, *loaded,
                              block_pos{.x = 2, .y = 3, .z = 2},
                              identity) == 2U);
        expect(chunk.state_at(2, 3, 2) == *stone);
        expect(chunk.state_at(3, 3, 2) == *chest);
        const auto *entity = chunk.block_entity_at({.x = 3, .y = 3, .z = 2});
        expect(entity != nullptr);
        if (entity != nullptr) {
            const auto x = entity->find("x");
            expect(x != entity->end());
            if (x != entity->end()) {
                const auto value = x->second.as_int();
                expect(value.has_value() && *value == 3);
            }
        }

        proto_chunk turned_chunk{registry, 0, 0, 0, 32};
        chunk_region turned_region{turned_chunk, 0};
        template_transform turned{};
        turned.mirror = template_mirror::left_right;
        turned.rotation = template_rotation::clockwise_90;
        expect(place_template(turned_region, registry, *loaded,
                              block_pos{.x = 5, .y = 3, .z = 5}, turned) == 2U);
        expect(turned_chunk.state_at(5, 3, 5) == *stone);
        const auto turned_chest = transform_state(registry, *chest, turned);
        expect(turned_chunk.state_at(5, 3, 6) == turned_chest);
        expect(registry.name(turned_chest) ==
               "minecraft:chest[facing=west,type=single,waterlogged=false]");
    };

    "placement clips to bounds and drops ignored blocks"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto marker = registry.intern("minecraft:structure_block[mode=load]",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
        auto chest = registry.intern(
            "minecraft:chest[facing=north,type=single,waterlogged=false]",
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true});
        expect(stone && marker && chest);
        if (!stone || !marker || !chest) {
            return;
        }

        data::nbt::compound root;
        root["size"] = ints({3, 1, 1});
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
        data::nbt::compound marker_properties;
        marker_properties["mode"] = data::nbt::value{std::string{"load"}};
        (void)palette.push(data::nbt::value{palette_entry(
            "minecraft:structure_block", std::move(marker_properties))});
        data::nbt::compound chest_properties;
        chest_properties["facing"] = data::nbt::value{std::string{"north"}};
        (void)palette.push(data::nbt::value{
            palette_entry("minecraft:chest", std::move(chest_properties))});
        root["palette"] = std::move(palette);
        data::nbt::compound chest_nbt;
        chest_nbt["id"] = data::nbt::value{std::string{"minecraft:chest"}};
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        (void)blocks.push(data::nbt::value{block_entry({1, 0, 0}, 1)});
        (void)blocks.push(
            data::nbt::value{block_entry({2, 0, 0}, 2, chest_nbt)});
        root["blocks"] = std::move(blocks);

        const auto loaded =
            load_structure_template(registry, data::nbt::value{root});
        expect(loaded.has_value());
        if (!loaded) {
            return;
        }

        block_ignore_processor ignore{
            std::vector<std::string>{"minecraft:structure_block"}};
        template_place_settings clipped{};
        clipped.bounds = template_bounds{.min_x = 0,
                                         .min_y = 0,
                                         .min_z = 0,
                                         .max_x = 1,
                                         .max_y = 0,
                                         .max_z = 0};
        clipped.processors = {&ignore};
        proto_chunk bounded_chunk{registry, 0, 0, 0, 16};
        chunk_region bounded_region{bounded_chunk, 0};
        expect(place_template(bounded_region, registry, *loaded,
                              block_pos{.x = 0, .y = 0, .z = 0},
                              clipped) == 1U);
        expect(bounded_chunk.state_at(0, 0, 0) == *stone);
        expect(bounded_chunk.state_at(1, 0, 0) == registry.air());
        expect(bounded_chunk.state_at(2, 0, 0) == registry.air());

        template_place_settings processed{};
        processed.processors = {&ignore};
        proto_chunk chunk{registry, 0, 0, 0, 16};
        chunk_region region{chunk, 0};
        expect(place_template(region, registry, *loaded,
                              block_pos{.x = 0, .y = 0, .z = 0},
                              processed) == 2U);
        expect(chunk.state_at(0, 0, 0) == *stone);
        expect(chunk.state_at(1, 0, 0) == registry.air());
        expect(chunk.state_at(2, 0, 0) == *chest);
    };

    "an igloo start places the top last and the basement below"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-igloo-starts";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory / "minecraft" /
                                            "structure" / "igloo");
        const auto write_template = [&](std::string_view name, jint size_x,
                                        jint size_y, jint size_z) {
            data::nbt::compound root;
            root["size"] = ints({size_x, size_y, size_z});
            auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
            (void)palette.push(
                data::nbt::value{palette_entry("minecraft:stone")});
            root["palette"] = std::move(palette);
            auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
            (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
            root["blocks"] = std::move(blocks);
            return data::nbt::write_file(
                (directory / "minecraft" / "structure" / "igloo" /
                 (std::string{name} + ".nbt"))
                    .string(),
                data::nbt::value{root}, "", data::nbt::compression::gzip);
        };
        expect(write_template("top", 7, 5, 8).has_value());
        expect(write_template("middle", 3, 3, 3).has_value());
        expect(write_template("bottom", 7, 6, 9).has_value());

        structure_template_manager manager{directory.string(), registry};
        bool saw_plain = false;
        bool saw_basement = false;
        for (jint chunk_x = 0; chunk_x < 8; ++chunk_x) {
            const auto start =
                igloo_start(chunk_pos{.x = chunk_x, .z = 0}, 12345, manager);
            expect(start.structure == "minecraft:igloo");
            expect(!start.pieces.empty());
            if (start.pieces.empty()) {
                continue;
            }
            const auto &top = start.pieces.back();
            expect(top.template_id == "minecraft:igloo/top");
            expect(top.template_origin.x == chunk_x * 16);
            expect(top.template_origin.y == 90);
            expect(top.template_origin.z == 0);
            expect(top.template_rotation >= 0 && top.template_rotation <= 3);
            if (start.pieces.size() == 1) {
                saw_plain = true;
                continue;
            }
            saw_basement = true;
            const auto depth = static_cast<jint>(start.pieces.size()) - 1;
            expect(depth >= 4 && depth <= 11);
            const auto &bottom = start.pieces.front();
            expect(bottom.template_id == "minecraft:igloo/bottom");
            expect(bottom.template_origin.x == chunk_x * 16);
            expect(bottom.template_origin.y == 87 - depth * 3);
            expect(bottom.template_origin.z == -2);
            for (jint i = 0; i < depth - 1; ++i) {
                const auto &middle =
                    start.pieces[static_cast<std::size_t>(i + 1)];
                expect(middle.template_id == "minecraft:igloo/middle");
                expect(middle.template_origin.x == chunk_x * 16 + 2);
                expect(middle.template_origin.y == 87 - i * 3);
                expect(middle.template_origin.z == 4);
            }
            expect(start.bounds.min_y <= bottom.template_origin.y);
            expect(start.bounds.max_y >= 94);
        }
        expect(saw_plain);
        expect(saw_basement);
        std::filesystem::remove_all(directory);
    };

    "the lowercase palette keys of the current format resolve"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        data::nbt::compound root;
        root["size"] = ints({1, 1, 1});
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        data::nbt::compound entry;
        entry["id"] = data::nbt::value{std::string{"minecraft:stone"}};
        (void)palette.push(data::nbt::value{entry});
        root["palette"] = std::move(palette);
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        root["blocks"] = std::move(blocks);
        const auto loaded =
            load_structure_template(registry, data::nbt::value{root});
        expect(loaded.has_value());
        if (!loaded || loaded->palettes.empty() ||
            loaded->palettes.front().empty()) {
            return;
        }
        expect(loaded->palettes.front().front().state == *stone);
    };

    "a known block with an unknown property set keeps its properties"_test =
        [] {
            block_state_registry registry;
            auto stairs =
                registry.intern("minecraft:oak_stairs[facing=north,half=bottom,"
                                "shape=straight,waterlogged=false]",
                                {.air = false,
                                 .blocks_motion = true,
                                 .blocks_motion_no_leaves = true});
            expect(stairs.has_value());
            if (!stairs) {
                return;
            }
            data::nbt::compound root;
            root["size"] = ints({1, 1, 1});
            auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
            data::nbt::compound entry;
            entry["id"] = data::nbt::value{std::string{"minecraft:oak_stairs"}};
            data::nbt::compound properties;
            properties["facing"] = data::nbt::value{std::string{"south"}};
            properties["half"] = data::nbt::value{std::string{"bottom"}};
            properties["shape"] = data::nbt::value{std::string{"straight"}};
            properties["waterlogged"] = data::nbt::value{std::string{"false"}};
            entry["properties"] = data::nbt::value{properties};
            (void)palette.push(data::nbt::value{entry});
            root["palette"] = std::move(palette);
            auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
            (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
            root["blocks"] = std::move(blocks);
            const auto loaded =
                load_structure_template(registry, data::nbt::value{root});
            expect(loaded.has_value());
            if (!loaded || loaded->palettes.empty() ||
                loaded->palettes.front().empty()) {
                return;
            }
            const auto name =
                registry.name(loaded->palettes.front().front().state);
            expect(name.find("facing=south") != std::string_view::npos);
            expect(name != registry.name(*stairs));
        };

    "block_rot and protected_blocks filter template entries"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        data::nbt::compound root;
        root["size"] = ints({4, 1, 1});
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
        root["palette"] = std::move(palette);
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        for (jint x = 0; x < 4; ++x) {
            (void)blocks.push(data::nbt::value{block_entry({x, 0, 0}, 0)});
        }
        root["blocks"] = std::move(blocks);
        const auto loaded =
            load_structure_template(registry, data::nbt::value{root});
        expect(loaded.has_value());
        if (!loaded) {
            return;
        }
        block_rot_processor drop_all{0.0F, {}, false};
        block_rot_processor keep_all{1.0F, {}, false};
        proto_chunk chunk{registry, 0, 0, 0, 16};
        chunk_region region{chunk, 0};
        template_place_settings settings{};
        settings.processors = {&drop_all};
        expect(place_template(region, registry, *loaded,
                              block_pos{.x = 0, .y = 0, .z = 0},
                              settings) == 0U);
        settings.processors = {&keep_all};
        expect(place_template(region, registry, *loaded,
                              block_pos{.x = 0, .y = 0, .z = 0},
                              settings) == 4U);

        registry.add_tag("minecraft:features_cannot_replace",
                         std::span<const block_state_id>{&*stone, 1});
        for (jint x = 0; x < 4; ++x) {
            chunk.set_state(x, 0, 0, registry.air());
        }
        chunk.set_state(1, 0, 0, *stone);
        protected_blocks_processor protected_blocks{
            "minecraft:features_cannot_replace"};
        settings.processors = {&protected_blocks};
        expect(place_template(region, registry, *loaded,
                              block_pos{.x = 0, .y = 0, .z = 0},
                              settings) == 3U);
        expect(chunk.state_at(1, 0, 0) == *stone);
    };

    "a processor list decodes block_rot and protected_blocks"_test = [] {
        block_state_registry registry;
        const auto document = data::json::value::parse(R"({
            "processors": [
                {"processor_type": "minecraft:block_rot", "integrity": 0.9},
                {"processor_type": "minecraft:protected_blocks",
                 "value": "#minecraft:features_cannot_replace"},
                {"processor_type": "minecraft:nop"}
            ]
        })");
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto list = decode_processor_list(registry, *document);
        expect(list.has_value());
        if (!list) {
            return;
        }
        expect((*list)->processors.size() == 2U);
    };

    "unknown palette names and indices resolve to air"_test = [] {
        block_state_registry registry;
        data::nbt::compound root;
        root["size"] = ints({1, 1, 1});
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(
            data::nbt::value{palette_entry("minecraft:not_a_block")});
        root["palette"] = std::move(palette);
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 7)});
        root["blocks"] = std::move(blocks);

        const auto loaded =
            load_structure_template(registry, data::nbt::value{root});
        expect(loaded.has_value());
        if (!loaded || loaded->palettes.empty()) {
            return;
        }
        expect(loaded->palettes.front().size() == 2U);
        for (const auto &block : loaded->palettes.front()) {
            expect(block.state == registry.air());
        }
    };

    "a jar template loads with its size and block entities"_test = [] {
        const char *path = std::getenv("REFRAIN_VANILLA_TEMPLATE");
        if (path == nullptr) {
            return;
        }
        const auto bytes = core::io::read_file(path);
        expect(bytes.has_value()) << bytes.error().message;
        if (!bytes) {
            return;
        }
        const auto document = data::nbt::read(
            std::span<const std::byte>{bytes->data(), bytes->size()},
            data::nbt::compression::gzip);
        expect(document.has_value()) << document.error().message;
        if (!document) {
            return;
        }
        block_state_registry registry;
        const auto loaded = load_structure_template(registry, *document);
        expect(loaded.has_value());
        if (!loaded) {
            return;
        }
        expect(loaded->size_x == 7);
        expect(loaded->size_y == 5);
        expect(loaded->size_z == 8);
        expect(loaded->palettes.size() == 1U);
        if (loaded->palettes.empty()) {
            return;
        }
        expect(loaded->palettes.front().size() == 152U);
        std::size_t block_entities = 0;
        for (const auto &block : loaded->palettes.front()) {
            block_entities += block.nbt != nullptr ? 1U : 0U;
        }
        expect(block_entities == 1U);
        expect(loaded->entities.empty());
    };

    "the manager loads gzip templates and returns an empty one for a missing id"_test =
        [] {
            block_state_registry registry;
            auto stone = registry.intern("minecraft:stone",
                                         {.air = false,
                                          .blocks_motion = true,
                                          .blocks_motion_no_leaves = true});
            expect(stone.has_value());
            if (!stone) {
                return;
            }
            const auto directory =
                std::filesystem::path{core::io::temp_directory()} /
                "refrain-template-manager";
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory / "test" /
                                                "structure");

            data::nbt::compound root;
            root["size"] = ints({1, 1, 1});
            auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
            (void)palette.push(
                data::nbt::value{palette_entry("minecraft:stone")});
            root["palette"] = std::move(palette);
            auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
            (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
            root["blocks"] = std::move(blocks);
            const auto written = data::nbt::write_file(
                (directory / "test" / "structure" / "demo.nbt").string(),
                data::nbt::value{root}, "", data::nbt::compression::gzip);
            expect(written.has_value());

            structure_template_manager manager{directory.string(), registry};
            const auto &loaded = manager.get_or_create("test:demo");
            expect(loaded.size_x == 1 && loaded.size_y == 1 &&
                   loaded.size_z == 1);
            expect(loaded.palettes.size() == 1U);
            if (!loaded.palettes.empty()) {
                expect(loaded.palettes.front().size() == 1U);
            }
            const auto &missing = manager.get_or_create("test:missing");
            expect(missing.size_x == 0);
            expect(missing.palettes.empty());
            expect(&manager.get_or_create("test:demo") == &loaded);
            std::filesystem::remove_all(directory);
        };

    "template shape updates cross the write radius"_test = [] {
        using namespace refrain;
        using namespace refrain::worldgen;
        block_state_registry registry;
        const auto stair_west =
            registry.intern("minecraft:jungle_stairs[facing=west,half=top,"
                            "shape=straight,waterlogged=false]",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        const auto stair_north =
            registry.intern("minecraft:jungle_stairs[facing=north,half=top,"
                            "shape=straight,waterlogged=false]",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        const auto fence =
            registry.intern("minecraft:jungle_fence[east=false,north=false,"
                            "south=false,waterlogged=false,west=false]",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        expect(stair_west && stair_north && fence);
        if (!stair_west || !stair_north || !fence) {
            return;
        }
        structure::structure_template templ{};
        templ.size_x = 17;
        templ.size_y = 1;
        templ.size_z = 1;
        templ.palettes.push_back({
            structure::template_block_info{
                .x = 15, .y = 0, .z = 0, .state = *stair_west},
            structure::template_block_info{
                .x = 16, .y = 0, .z = 0, .state = *stair_north},
        });
        proto_chunk left{registry, 0, 0, 0, 32};
        proto_chunk right{registry, 1, 0, 0, 32};
        chunk_region region{left, 1};
        region.attach(right);
        auto left_settings = structure::template_place_settings{};
        left_settings.bounds = structure::template_bounds{.min_x = 0,
                                                          .min_y = 0,
                                                          .min_z = 0,
                                                          .max_x = 15,
                                                          .max_y = 31,
                                                          .max_z = 31};
        auto right_settings = structure::template_place_settings{};
        right_settings.bounds = structure::template_bounds{.min_x = 16,
                                                           .min_y = 0,
                                                           .min_z = 0,
                                                           .max_x = 31,
                                                           .max_y = 31,
                                                           .max_z = 31};
        (void)structure::place_template(region, registry, templ,
                                        block_pos{.x = 0, .y = 0, .z = 0},
                                        left_settings);
        (void)structure::place_template(region, registry, templ,
                                        block_pos{.x = 0, .y = 0, .z = 0},
                                        right_settings);
        const auto shape =
            std::string{registry.name(region.state_at(15, 0, 0))};
        expect(shape.find("shape=inner_right") != std::string::npos);

        templ.palettes.clear();
        templ.palettes.push_back({
            structure::template_block_info{
                .x = 15, .y = 0, .z = 0, .state = *fence},
            structure::template_block_info{
                .x = 16, .y = 0, .z = 0, .state = *fence},
        });
        proto_chunk left_fence{registry, 0, 0, 0, 32};
        proto_chunk right_fence{registry, 1, 0, 0, 32};
        chunk_region fence_region{left_fence, 1};
        fence_region.attach(right_fence);
        (void)structure::place_template(fence_region, registry, templ,
                                        block_pos{.x = 0, .y = 0, .z = 0},
                                        left_settings);
        (void)structure::place_template(fence_region, registry, templ,
                                        block_pos{.x = 0, .y = 0, .z = 0},
                                        right_settings);
        const auto east =
            std::string{registry.name(fence_region.state_at(15, 0, 0))};
        expect(east.find("east=true") != std::string::npos);
    };
};
