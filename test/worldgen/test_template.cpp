// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <boost/ut.hpp>

#include <cstdint>
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
};
