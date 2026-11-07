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
