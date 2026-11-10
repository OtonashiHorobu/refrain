// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace {

using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::structure;

/// Returns the same 24-bit value for every draw, so a test can place a
/// predicate result exactly on a chance boundary. `next_float` multiplies the
/// value by 2^-24.
class scripted_float_source final : public random_source {
  public:
    explicit scripted_float_source(jint bits) : bits_{bits} {}

    auto next(int) -> jint override { return bits_; }
    auto next_gaussian() -> double override { return 0.0; }
    void set_seed(jlong) override {}
    auto fork() -> std::unique_ptr<random_source> override {
        return std::make_unique<scripted_float_source>(bits_);
    }
    auto fork_positional()
        -> std::unique_ptr<positional_random_factory> override {
        return nullptr;
    }

  private:
    jint bits_{0};
};

auto scripted_random(jint bits) -> worldgen_random {
    return worldgen_random{std::make_unique<scripted_float_source>(bits)};
}

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

    "a canonical template palette keeps its stored order"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry registry;
        const auto stone = registry.intern("minecraft:stone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true});
        const auto stairs =
            registry.intern("minecraft:oak_stairs[facing=east,half=bottom,"
                            "shape=straight,waterlogged=false]",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        const auto chest = registry.intern(
            "minecraft:chest[facing=north,type=single,waterlogged=false]",
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true});
        expect(stone && stairs && chest);
        if (!stone || !stairs || !chest) {
            return;
        }
        data::nbt::compound root;
        root["size"] = ints({1, 2, 1});
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
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 1, 0}, 0)});
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 1)});
        (void)blocks.push(
            data::nbt::value{block_entry({0, 0, 1}, 2, chest_nbt)});
        root["blocks"] = std::move(blocks);
        const auto loaded =
            load_structure_template(registry, data::nbt::value{root});
        expect(loaded.has_value());
        if (!loaded || loaded->palettes.empty() ||
            loaded->palettes.front().size() != 3U) {
            return;
        }
        const auto &order = loaded->palettes.front();
        // Already grouped full/other/entity; a trait-based re-sort would move
        // the stairs before the stone at y 1.
        expect(order[0].state == *stone && order[0].y == 1);
        expect(order[1].state == *stairs && order[1].y == 0);
        expect(order[2].nbt != nullptr && order[2].y == 0 && order[2].z == 1);
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
        expect((*list)->processors.size() == 3U);
    };

    "a linear position predicate interpolates between distance bounds"_test =
        [] {
            const block_pos reference{};
            position_rule_test test{};
            test.type = position_rule_type::linear_pos;
            test.min_chance = 0.25F;
            test.max_chance = 0.75F;
            test.min_dist = 0;
            test.max_dist = 10;
            const auto passes = [&](jint bits, const block_pos &world) {
                auto random = scripted_random(bits);
                return test.test(world, reference, random);
            };
            // 0.25, 0.5 and 0.75 are exact 24-bit float draws.
            expect(passes(1 << 22, block_pos{.x = 0, .y = 0, .z = 0}));
            expect(!passes((1 << 22) + 1, block_pos{.x = 0, .y = 0, .z = 0}));
            expect(passes(1 << 23, block_pos{.x = 5, .y = 0, .z = 0}));
            expect(!passes((1 << 23) + 1, block_pos{.x = 5, .y = 0, .z = 0}));
            expect(passes(3 << 22, block_pos{.x = 0, .y = 0, .z = 10}));
            expect(!passes((3 << 22) + 1, block_pos{.x = 0, .y = 0, .z = 10}));
            // Distances past either bound clamp to the end chance.
            expect(passes(3 << 22, block_pos{.x = 100, .y = 0, .z = 0}));
            expect(!passes((3 << 22) + 1, block_pos{.x = 100, .y = 0, .z = 0}));
            // The distance is the sum over all three axes.
            expect(passes(1 << 23, block_pos{.x = 2, .y = 2, .z = 1}));
            expect(!passes((1 << 23) + 1, block_pos{.x = 2, .y = 2, .z = 1}));
        };

    "an axis aligned position predicate measures only its axis"_test = [] {
        const block_pos reference{};
        position_rule_test test{};
        test.type = position_rule_type::axis_aligned_linear_pos;
        test.min_chance = 0.0F;
        test.max_chance = 1.0F;
        test.min_dist = 0;
        test.max_dist = 10;
        const auto passes = [&](jint bits, const block_pos &world) {
            auto random = scripted_random(bits);
            return test.test(world, reference, random);
        };
        test.axis = position_axis::x;
        expect(passes((1 << 24) - 1, block_pos{.x = 100, .y = 0, .z = 0}));
        expect(passes((1 << 24) - 1, block_pos{.x = -100, .y = 0, .z = 0}));
        // The maximum draw still passes a chance of one.
        expect(passes((1 << 24) - 1, block_pos{.x = 10, .y = 0, .z = 0}));
        // A distance of zero uses the minimum chance; only a zero draw passes.
        expect(passes(0, block_pos{.x = 0, .y = 0, .z = 0}));
        expect(!passes(1, block_pos{.x = 0, .y = 0, .z = 0}));
        test.axis = position_axis::y;
        expect(!passes(1, block_pos{.x = 100, .y = 0, .z = 100}));
        test.axis = position_axis::z;
        expect(passes((1 << 24) - 1, block_pos{.x = 0, .y = 0, .z = 10}));
        // A falling chance clamps the same way.
        test.min_chance = 1.0F;
        test.max_chance = 0.0F;
        test.axis = position_axis::y;
        expect(passes(0, block_pos{.x = 0, .y = 0, .z = 0}));
        expect(!passes(1, block_pos{.x = 0, .y = 10, .z = 0}));
    };

    "a rule position predicate sees the world position and reference"_test =
        [] {
            block_state_registry states;
            const auto stone = states.intern(
                "minecraft:stone", {.air = false, .blocks_motion = true});
            const auto dirt = states.intern(
                "minecraft:dirt", {.air = false, .blocks_motion = true});
            expect(stone && dirt);
            if (!stone || !dirt) {
                return;
            }
            const block_pos world{.x = 8, .y = 5, .z = 8};
            proto_chunk chunk{states, 0, 0, 0, 32};
            chunk.set_state(world.x, world.y, world.z, *stone);
            chunk_region region{chunk, 0};
            template_rule rule{};
            rule.input.type = rule_test_type::random_block_match;
            rule.input.blocks = {"minecraft:stone"};
            rule.input.probability = 1.0F;
            rule.location.type = rule_test_type::always_true;
            rule.position.type = position_rule_type::linear_pos;
            rule.position.min_chance = 0.25F;
            rule.position.max_chance = 0.75F;
            rule.position.min_dist = 0;
            rule.position.max_dist = 10;
            rule.output = *dirt;
            const rule_processor processor{{rule}};
            // The input test draws first; the location test draws nothing and
            // the position test draws next. A reference at the block itself
            // is distance zero, so the chance is the minimum.
            legacy_random_source expected{
                mth_get_seed(world.x, world.y, world.z)};
            (void)expected.next_float();
            const bool matches = expected.next_float() <= 0.25F;
            const template_block_info info{
                .x = 999, .y = 999, .z = 999, .state = *stone};
            const auto result =
                processor.process(states, region, info, world, world, nullptr);
            expect(result.has_value());
            if (result) {
                expect(result->state == (matches ? *dirt : *stone));
            }
            // Moving the reference ten blocks away selects the maximum chance
            // even though the template-local position did not change.
            legacy_random_source far_expected{
                mth_get_seed(world.x, world.y, world.z)};
            (void)far_expected.next_float();
            const bool far_matches = far_expected.next_float() <= 0.75F;
            const block_pos far_reference{
                .x = world.x + 10, .y = world.y, .z = world.z};
            const auto far_result = processor.process(
                states, region, info, world, far_reference, nullptr);
            expect(far_result.has_value());
            if (far_result) {
                expect(far_result->state == (far_matches ? *dirt : *stone));
            }
        };

    "a decoded position predicate controls the rule output"_test = [] {
        block_state_registry states;
        const auto stone = states.intern("minecraft:stone",
                                         {.air = false, .blocks_motion = true});
        const auto dirt = states.intern("minecraft:dirt",
                                        {.air = false, .blocks_motion = true});
        expect(stone && dirt);
        if (!stone || !dirt) {
            return;
        }
        const auto document = data::json::value::parse(R"({
            "processors": [{
                "processor_type": "minecraft:rule",
                "rules": [{
                    "input_predicate": {"predicate_type": "minecraft:always_true"},
                    "location_predicate": {"predicate_type": "minecraft:always_true"},
                    "position_predicate": {
                        "predicate_type": "minecraft:axis_aligned_linear_pos",
                        "axis": "x",
                        "min_chance": 0.25,
                        "max_chance": 0.75,
                        "min_dist": 0,
                        "max_dist": 10
                    },
                    "output_state": "minecraft:dirt"
                }]
            }]
        })");
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto list = decode_processor_list(states, *document);
        expect(list.has_value());
        if (!list) {
            return;
        }
        structure_template templ{};
        templ.size_x = 1;
        templ.size_y = 1;
        templ.size_z = 1;
        templ.palettes.push_back(
            {template_block_info{.x = 0, .y = 0, .z = 0, .state = *stone}});
        template_place_settings settings{};
        settings.processors = (*list)->processors;
        const auto place_at = [&](const block_pos &origin,
                                  const block_pos &reference) {
            proto_chunk chunk{states, 0, 0, 0, 32};
            chunk_region region{chunk, 0};
            settings.reference = reference;
            (void)place_template(region, states, templ, origin, settings);
            return chunk.state_at(origin.x & 15, origin.y, origin.z & 15);
        };
        const block_pos near_origin{.x = 2, .y = 10, .z = 2};
        legacy_random_source near_expected{
            mth_get_seed(near_origin.x, near_origin.y, near_origin.z)};
        const bool near_matches = near_expected.next_float() <= 0.25F;
        expect(place_at(near_origin, near_origin) ==
               (near_matches ? *dirt : *stone));
        const block_pos far_origin{.x = 6, .y = 10, .z = 6};
        const block_pos far_reference{
            .x = far_origin.x + 10, .y = far_origin.y, .z = far_origin.z};
        legacy_random_source far_expected{
            mth_get_seed(far_origin.x, far_origin.y, far_origin.z)};
        const bool far_matches = far_expected.next_float() <= 0.75F;
        expect(place_at(far_origin, far_reference) ==
               (far_matches ? *dirt : *stone));
    };

    "invalid position predicates are rejected"_test = [] {
        block_state_registry states;
        const auto equal_range = data::json::value::parse(R"({
            "processors": [{
                "processor_type": "minecraft:rule",
                "rules": [{
                    "input_predicate": {"predicate_type": "minecraft:always_true"},
                    "location_predicate": {"predicate_type": "minecraft:always_true"},
                    "position_predicate": {
                        "predicate_type": "minecraft:linear_pos",
                        "min_dist": 5, "max_dist": 5
                    },
                    "output_state": "minecraft:dirt"
                }]
            }]
        })");
        const auto unknown_type = data::json::value::parse(R"({
            "processors": [{
                "processor_type": "minecraft:rule",
                "rules": [{
                    "input_predicate": {"predicate_type": "minecraft:always_true"},
                    "location_predicate": {"predicate_type": "minecraft:always_true"},
                    "position_predicate": {
                        "predicate_type": "minecraft:does_not_exist"
                    },
                    "output_state": "minecraft:dirt"
                }]
            }]
        })");
        const auto unknown_axis = data::json::value::parse(R"({
            "processors": [{
                "processor_type": "minecraft:rule",
                "rules": [{
                    "input_predicate": {"predicate_type": "minecraft:always_true"},
                    "location_predicate": {"predicate_type": "minecraft:always_true"},
                    "position_predicate": {
                        "predicate_type": "minecraft:axis_aligned_linear_pos",
                        "axis": "w",
                        "min_dist": 0, "max_dist": 1
                    },
                    "output_state": "minecraft:dirt"
                }]
            }]
        })");
        const auto missing_type = data::json::value::parse(R"({
            "processors": [{
                "processor_type": "minecraft:rule",
                "rules": [{
                    "input_predicate": {"predicate_type": "minecraft:always_true"},
                    "location_predicate": {"predicate_type": "minecraft:always_true"},
                    "position_predicate": {"min_dist": 0, "max_dist": 1},
                    "output_state": "minecraft:dirt"
                }]
            }]
        })");
        expect(equal_range.has_value() && unknown_type.has_value() &&
               unknown_axis.has_value() && missing_type.has_value());
        if (!equal_range || !unknown_type || !unknown_axis || !missing_type) {
            return;
        }
        expect(!decode_processor_list(states, *equal_range).has_value());
        expect(!decode_processor_list(states, *unknown_type).has_value());
        expect(!decode_processor_list(states, *unknown_axis).has_value());
        expect(!decode_processor_list(states, *missing_type).has_value());
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

    "wall sides ignore hanging lanterns"_test = [] {
        using namespace refrain;
        using namespace refrain::worldgen;
        block_state_registry registry;
        const auto wall = registry.intern(
            "minecraft:blackstone_wall[east=none,north=none,south=none,"
            "up=true,waterlogged=false,west=none]",
            {.air = false, .blocks_motion = true});
        const auto bricks =
            registry.intern("minecraft:polished_blackstone_bricks",
                            {.air = false, .blocks_motion = true});
        const auto lantern =
            registry.intern("minecraft:lantern[hanging=true,waterlogged=false]",
                            {.air = false, .blocks_motion = true});
        expect(wall && bricks && lantern);
        if (!wall || !bricks || !lantern) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 16};
        chunk_region region{chunk, 0};
        const auto property = [&](std::string_view key) {
            const auto name =
                std::string{registry.name(chunk.state_at(5, 5, 5))};
            const auto at = name.find(key);
            if (at == std::string::npos) {
                return std::string{};
            }
            const auto start = at + key.size();
            return name.substr(start, name.find_first_of(",]", start) - start);
        };
        const auto reset_wall = [&] {
            chunk.set_state(5, 5, 5, *wall);
            chunk.set_state(4, 5, 5, registry.air());
            chunk.set_state(5, 6, 5, registry.air());
        };

        // A hanging lantern west of a wall does not offer a sturdy face, so
        // the wall must not connect to it even though the block has motion
        // blocking height.
        reset_wall();
        chunk.set_state(4, 5, 5, *lantern);
        chunk.set_state(5, 6, 5, *bricks);
        (void)structure::refresh_block_shape(registry, region,
                                             block_pos{.x = 5, .y = 5, .z = 5});
        expect(property("west=") == "none");
        expect(property("up=") == "true");

        // A lantern above the wall does not cover the side, so a connected
        // wall stays low instead of turning tall.
        reset_wall();
        chunk.set_state(4, 5, 5, *bricks);
        chunk.set_state(5, 6, 5, *lantern);
        expect(structure::refresh_block_shape(
            registry, region, block_pos{.x = 5, .y = 5, .z = 5}));
        expect(property("west=") == "low");

        // A full block above covers the side and makes it tall.
        chunk.set_state(5, 6, 5, *bricks);
        expect(structure::refresh_block_shape(
            registry, region, block_pos{.x = 5, .y = 5, .z = 5}));
        expect(property("west=") == "tall");
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

    "an ocean ruin piece places and rots its template"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false, .blocks_motion = true});
        auto stairs =
            registry.intern("minecraft:oak_stairs[facing=north,half=bottom,"
                            "shape=straight,waterlogged=false]",
                            {.air = false, .blocks_motion = true});
        auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        expect(stone && stairs && water);
        if (!stone || !stairs || !water) {
            return;
        }
        const auto directory =
            std::filesystem::temp_directory_path() / "refrain-ocean-ruin-test";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory / "test" / "structure");
        data::nbt::compound root;
        root["size"] = ints({2, 1, 1});
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
        data::nbt::compound stair_properties;
        stair_properties["facing"] = data::nbt::value{std::string{"north"}};
        stair_properties["half"] = data::nbt::value{std::string{"bottom"}};
        stair_properties["shape"] = data::nbt::value{std::string{"straight"}};
        stair_properties["waterlogged"] =
            data::nbt::value{std::string{"false"}};
        (void)palette.push(data::nbt::value{palette_entry(
            "minecraft:oak_stairs", std::move(stair_properties))});
        root["palette"] = std::move(palette);
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        (void)blocks.push(data::nbt::value{block_entry({1, 0, 0}, 1)});
        root["blocks"] = std::move(blocks);
        const auto written = data::nbt::write_file(
            (directory / "test" / "structure" / "ruin.nbt").string(),
            data::nbt::value{root}, "", data::nbt::compression::gzip);
        expect(written.has_value());
        structure_template_manager manager{directory.string(), registry};
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_state(1, 10, 0, *water);
        chunk_region region{chunk, 0};
        const auto resolve = [&registry](std::string_view key) {
            return registry.find(key).value_or(no_block_state);
        };
        const structure_place_context context{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .state = resolve,
            .templates = &manager,
            .registry = &registry,
        };
        structure_piece_record piece{
            .type = "minecraft:ocean_ruin",
            .template_id = "test:ruin",
            .template_x = 0,
            .template_y = 10,
            .template_z = 0,
            .height_position = 10,
            .integrity = 1.0F,
        };
        piece.orientation = 2;
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(1)};
        place_structure_piece(context, piece, random);
        expect(chunk.state_at(0, 10, 0) == *stone);
        const auto placed =
            std::string{registry.name(chunk.state_at(1, 10, 0))};
        expect(placed.starts_with("minecraft:oak_stairs"));
        expect(placed.find("waterlogged=true") != std::string::npos);

        piece.template_x = 4;
        piece.height_position = 10;
        piece.integrity = 0.0F;
        auto rot_random =
            worldgen_random{std::make_unique<legacy_random_source>(1)};
        place_structure_piece(context, piece, rot_random);
        expect(chunk.state_at(4, 10, 0) != *stone);
        std::filesystem::remove_all(directory);
    };

    "the capped rule processor replaces a limited set of blocks"_test = [] {
        block_state_registry registry;
        const auto sand = registry.intern(
            "minecraft:sand", {.air = false, .blocks_motion = true});
        const auto suspicious = registry.intern(
            "minecraft:suspicious_sand", {.air = false, .blocks_motion = true});
        expect(sand && suspicious);
        if (!sand || !suspicious) {
            return;
        }
        structure_template templ{};
        templ.size_x = 4;
        templ.size_y = 1;
        templ.size_z = 1;
        templ.palettes.emplace_back();
        for (jint x = 0; x < 4; ++x) {
            templ.palettes.front().push_back(
                template_block_info{.x = x, .y = 0, .z = 0, .state = *sand});
        }
        capped_rule_processor capped{
            "minecraft:sand", "minecraft:suspicious_sand",
            "minecraft:archaeology/ocean_ruin_warm", 2};
        template_place_settings settings{};
        settings.processors = {&capped};
        settings.level_seed = 12345;
        settings.bounds = template_bounds{.min_x = 0,
                                          .min_y = 0,
                                          .min_z = 0,
                                          .max_x = 15,
                                          .max_y = 31,
                                          .max_z = 31};
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        (void)place_template(region, registry, templ,
                             block_pos{.x = 0, .y = 10, .z = 0}, settings);
        jint suspicious_count = 0;
        jint sand_count = 0;
        for (jint x = 0; x < 4; ++x) {
            const auto state = chunk.state_at(x, 10, 0);
            if (state == *suspicious) {
                ++suspicious_count;
                const auto *entity =
                    chunk.block_entity_at(block_pos{.x = x, .y = 10, .z = 0});
                expect(entity != nullptr);
            } else if (state == *sand) {
                ++sand_count;
            }
        }
        expect(suspicious_count == 2);
        expect(sand_count == 2);
    };

    "template palettes and rot use positional seeds by default"_test = [] {
        block_state_registry registry;
        const auto stone = registry.intern(
            "minecraft:stone", {.air = false, .blocks_motion = true});
        const auto dirt = registry.intern(
            "minecraft:dirt", {.air = false, .blocks_motion = true});
        expect(stone && dirt);
        if (!stone || !dirt) {
            return;
        }
        structure_template templ{};
        templ.size_x = 1;
        templ.size_y = 1;
        templ.size_z = 1;
        templ.palettes.push_back(
            {template_block_info{.x = 0, .y = 0, .z = 0, .state = *stone}});
        templ.palettes.push_back(
            {template_block_info{.x = 0, .y = 0, .z = 0, .state = *dirt}});
        legacy_random_source probe{mth_get_seed(0, 10, 0)};
        const auto expected_palette = probe.next_int(2);
        template_place_settings settings{};
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        (void)place_template(region, registry, templ,
                             block_pos{.x = 0, .y = 10, .z = 0}, settings);
        const auto chosen = chunk.state_at(0, 10, 0);
        expect(chosen == (expected_palette == 0 ? *stone : *dirt));

        structure_template rot_template{};
        rot_template.size_x = 1;
        rot_template.size_y = 1;
        rot_template.size_z = 1;
        rot_template.palettes.push_back(
            {template_block_info{.x = 0, .y = 0, .z = 0, .state = *stone}});
        legacy_random_source rot_probe{mth_get_seed(0, 20, 0)};
        const bool kept = rot_probe.next_float() <= 0.5F;
        block_rot_processor rot{0.5F, {}, false};
        auto rot_settings = template_place_settings{};
        rot_settings.processors = {&rot};
        proto_chunk rot_chunk{registry, 0, 0, 0, 32};
        chunk_region rot_region{rot_chunk, 0};
        (void)place_template(rot_region, registry, rot_template,
                             block_pos{.x = 0, .y = 20, .z = 0}, rot_settings);
        expect((rot_chunk.state_at(0, 20, 0) == *stone) == kept);
    };

    "block rot matches rottable tags"_test = [] {
        block_state_registry registry;
        const auto stone = registry.intern(
            "minecraft:stone", {.air = false, .blocks_motion = true});
        const auto dirt = registry.intern(
            "minecraft:dirt", {.air = false, .blocks_motion = true});
        expect(stone && dirt);
        if (!stone || !dirt) {
            return;
        }
        const std::array<block_state_id, 1> tagged{*stone};
        registry.add_tag("test:rottable", tagged);
        block_rot_processor rot{0.0F, {"#test:rottable"}, true};
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        const template_block_info stone_info{.state = *stone};
        const template_block_info dirt_info{.state = *dirt};
        expect(!rot.process(registry, region, stone_info,
                            block_pos{.x = 0, .y = 1, .z = 0}, block_pos{},
                            nullptr)
                    .has_value());
        expect(rot.process(registry, region, dirt_info,
                           block_pos{.x = 0, .y = 1, .z = 0}, block_pos{},
                           nullptr)
                   .has_value());
    };

    "ruined portal processors replace, protect and restore blocks"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto gold = states.intern("minecraft:gold_block",
                                        {.air = false, .blocks_motion = true});
        const auto stone = states.intern("minecraft:stone",
                                         {.air = false, .blocks_motion = true});
        const auto polished =
            states.intern("minecraft:polished_blackstone",
                          {.air = false, .blocks_motion = true});
        const auto netherrack = states.intern(
            "minecraft:netherrack", {.air = false, .blocks_motion = true});
        const auto lava =
            states.intern("minecraft:lava", {.air = false, .fluid = true});
        expect(gold && stone && polished && netherrack && lava);
        if (!gold || !stone || !polished || !netherrack || !lava) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk.set_state(8, 5, 8, *gold);
        chunk_region region{chunk, 0};
        const block_pos world{.x = 8, .y = 5, .z = 8};
        template_block_info info{.x = 0, .y = 5, .z = 0, .state = *gold};

        const structure::rule_processor rule{{{
            .input =
                structure::rule_test{.type =
                                         structure::rule_test_type::block_match,
                                     .blocks = {"minecraft:gold_block"}},
            .output = *netherrack,
        }}};
        const auto replaced =
            rule.process(states, region, info, world, block_pos{}, nullptr);
        expect(replaced.has_value());
        if (replaced) {
            expect(replaced->state == *netherrack);
        }

        const std::array<block_state_id, 1> tag_states{*gold};
        states.add_tag("test:protected", tag_states);
        const structure::protected_block_processor protected_blocks{
            "test:protected"};
        expect(!protected_blocks
                    .process(states, region, info, world, block_pos{}, nullptr)
                    .has_value());

        chunk.set_state(8, 5, 8, *lava);
        const structure::lava_submerged_processor submerged{*lava};
        template_block_info non_full{.x = 0, .y = 5, .z = 0, .state = 0};
        const auto restored = submerged.process(states, region, non_full, world,
                                                block_pos{}, nullptr);
        expect(restored.has_value());
        if (restored) {
            expect(restored->state == *lava);
        }

        chunk.set_state(8, 5, 8, *stone);
        const structure::blackstone_replace_processor blackstone;
        template_block_info stone_info{.x = 0, .y = 5, .z = 0, .state = *stone};
        const auto black = blackstone.process(states, region, stone_info, world,
                                              block_pos{}, nullptr);
        expect(black.has_value());
        if (black) {
            expect(black->state == *polished);
        }
    };

    "the age processor only produces cracked or mossy variants"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        const auto stone_bricks = states.intern(
            "minecraft:stone_bricks", {.air = false, .blocks_motion = true});
        const auto cracked =
            states.intern("minecraft:cracked_stone_bricks",
                          {.air = false, .blocks_motion = true});
        const auto mossy = states.intern("minecraft:mossy_stone_bricks",
                                         {.air = false, .blocks_motion = true});
        for (const auto *stairs :
             {"minecraft:stone_brick_stairs[facing=north,half=bottom]",
              "minecraft:mossy_stone_brick_stairs[facing=north,half=bottom]"}) {
            (void)states.intern(stairs, {.air = false, .blocks_motion = true});
        }
        expect(stone_bricks && cracked && mossy);
        if (!stone_bricks || !cracked || !mossy) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        const structure::block_age_processor dry{0.0F};
        const structure::block_age_processor wet{1.0F};
        template_block_info info{
            .x = 0, .y = 5, .z = 0, .state = *stone_bricks};
        std::size_t changed = 0;
        for (jint x = 0; x < 256; ++x) {
            const block_pos world{.x = x, .y = 5, .z = 0};
            const auto dry_result =
                dry.process(states, region, info, world, block_pos{}, nullptr);
            const auto wet_result =
                wet.process(states, region, info, world, block_pos{}, nullptr);
            expect(dry_result.has_value() && wet_result.has_value());
            if (!dry_result || !wet_result) {
                return;
            }
            const auto dry_name = states.name(dry_result->state);
            const auto wet_name = states.name(wet_result->state);
            expect(dry_name == "minecraft:stone_bricks" ||
                   dry_name == "minecraft:cracked_stone_bricks" ||
                   dry_name.starts_with("minecraft:stone_brick_stairs"));
            expect(wet_name == "minecraft:stone_bricks" ||
                   wet_name == "minecraft:mossy_stone_bricks" ||
                   wet_name.starts_with("minecraft:mossy_stone_brick_stairs"));
            changed += dry_name != "minecraft:stone_bricks" ? 1U : 0U;
        }
        expect(changed > 0);
    };

    "the age processor uses the world position for its random"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        const auto stone_bricks = states.intern(
            "minecraft:stone_bricks", {.air = false, .blocks_motion = true});
        const auto cracked =
            states.intern("minecraft:cracked_stone_bricks",
                          {.air = false, .blocks_motion = true});
        const auto mossy = states.intern("minecraft:mossy_stone_bricks",
                                         {.air = false, .blocks_motion = true});
        (void)states.intern(
            "minecraft:stone_brick_stairs[facing=north,half=bottom]",
            {.air = false, .blocks_motion = true});
        const auto expected_stairs =
            states.intern("minecraft:stone_brick_stairs[facing=east,half=top]",
                          {.air = false, .blocks_motion = true});
        expect(stone_bricks && cracked && mossy && expected_stairs);
        if (!stone_bricks || !cracked || !mossy) {
            return;
        }
        proto_chunk chunk{states, -16, -36, -64, 384};
        chunk_region region{chunk, 0};
        const structure::block_age_processor age{0.2F};
        const template_block_info info{
            .x = 1, .y = 1, .z = 3, .state = *stone_bricks};
        const auto result = age.process(
            states, region, info, block_pos{.x = -253, .y = 63, .z = -569},
            block_pos{}, nullptr);
        expect(result.has_value());
        if (result) {
            expect(result->state == *expected_stairs);
        }
    };

    "loot table containers consume one shared random seed"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        const auto air = states.intern("minecraft:air", {.air = true});
        const auto chest =
            states.intern("minecraft:chest", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        expect(air && chest);
        if (!air || !chest) {
            return;
        }
        structure::structure_template templ{};
        templ.size_x = 1;
        templ.size_y = 1;
        templ.size_z = 1;
        structure::template_block_info block{};
        block.state = *chest;
        data::nbt::compound chest_nbt{};
        chest_nbt["LootTable"] =
            data::nbt::value{std::string{"minecraft:chests/test"}};
        block.nbt =
            std::make_shared<const data::nbt::compound>(std::move(chest_nbt));
        templ.palettes.push_back({block});
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        worldgen_random loot{std::make_unique<legacy_random_source>(7)};
        worldgen_random expected{std::make_unique<legacy_random_source>(7)};
        const jlong seed = expected.next_long();
        structure::template_place_settings settings{};
        settings.loot_random = &loot;
        (void)structure::place_template(
            region, states, templ, block_pos{.x = 4, .y = 5, .z = 6}, settings);
        const auto *entity =
            chunk.block_entity_at(block_pos{.x = 4, .y = 5, .z = 6});
        expect(entity != nullptr);
        if (entity == nullptr) {
            return;
        }
        const auto found = entity->find("LootTableSeed");
        expect(found != entity->end());
        if (found != entity->end()) {
            expect(found->second.as_long().value_or(0) == seed);
        }
    };

    "a placed fence clears a side over a non-sturdy neighbour"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto carpet = states.intern(
            "minecraft:red_carpet", {.air = false, .blocks_motion = false});
        const auto fence =
            states.intern("minecraft:birch_fence[east=false,north=false,south="
                          "false,waterlogged=false,west=true]",
                          {.air = false, .blocks_motion = true});
        expect(carpet && fence);
        if (!carpet || !fence) {
            return;
        }
        structure::structure_template templ{};
        templ.size_x = 2;
        templ.size_y = 1;
        templ.size_z = 1;
        structure::template_block_info fence_info{};
        fence_info.x = 1;
        fence_info.state = *fence;
        structure::template_block_info carpet_info{};
        carpet_info.x = 0;
        carpet_info.state = *carpet;
        templ.palettes.push_back({carpet_info, fence_info});
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        (void)structure::place_template(region, states, templ,
                                        block_pos{.x = 4, .y = 5, .z = 6},
                                        structure::template_place_settings{});
        const auto placed = states.name(chunk.state_at(6, 5, 6));
        expect(placed.find("west=true") == std::string_view::npos);
    };

    "a capped processor list applies a rule delegate to a limited set"_test =
        [] {
            using namespace refrain::worldgen;
            block_state_registry states;
            const auto air = states.intern("minecraft:air", {.air = true});
            const auto sand = states.intern(
                "minecraft:sand", {.air = false, .blocks_motion = true});
            const auto suspicious =
                states.intern("minecraft:suspicious_sand",
                              {.air = false, .blocks_motion = true});
            expect(air && sand && suspicious);
            if (!air || !sand || !suspicious) {
                return;
            }
            const std::array<block_state_id, 1> replaceable{*sand};
            states.add_tag("minecraft:trail_ruins_replaceable", replaceable);
            const auto document = data::json::value::parse(R"({
                "processors": [
                    {
                        "processor_type": "minecraft:capped",
                        "limit": 2,
                        "delegate": {
                            "processor_type": "minecraft:rule",
                            "rules": [
                                {
                                    "input_predicate": {
                                        "predicate_type": "minecraft:tag_match",
                                        "tag": "minecraft:trail_ruins_replaceable"
                                    },
                                    "location_predicate": {
                                        "predicate_type": "minecraft:always_true"
                                    },
                                    "output_state": "minecraft:suspicious_sand",
                                    "block_entity_modifier": {
                                        "type": "minecraft:append_loot",
                                        "loot_table": "minecraft:archaeology/trail_ruins_common"
                                    }
                                }
                            ]
                        }
                    }
                ]
            })");
            expect(document.has_value());
            if (!document) {
                return;
            }
            const auto list = decode_processor_list(states, *document);
            expect(list.has_value());
            if (!list || (*list)->processors.size() != 1U) {
                return;
            }
            structure::structure_template templ{};
            templ.size_x = 6;
            templ.size_y = 1;
            templ.size_z = 1;
            templ.palettes.emplace_back();
            for (jint x = 0; x < 6; ++x) {
                templ.palettes.front().push_back(structure::template_block_info{
                    .x = x, .y = 0, .z = 0, .state = *sand});
            }
            const block_pos origin{.x = 8, .y = 20, .z = 8};
            const jlong level_seed = 12345;
            // The capped stream draws the limit (a constant consumes
            // nothing), then shuffles the index list.
            legacy_random_source base{level_seed};
            legacy_random_source capped{
                mth_get_seed(origin.x, origin.y, origin.z) ^ base.next_long()};
            std::array<std::size_t, 6> indices{0, 1, 2, 3, 4, 5};
            for (std::size_t remaining = indices.size(); remaining > 1;
                 --remaining) {
                const auto swap_to = static_cast<std::size_t>(
                    capped.next_int(static_cast<jint>(remaining)));
                std::swap(indices[remaining - 1], indices[swap_to]);
            }
            std::array<jlong, 2> expected_seeds{};
            for (std::size_t i = 0; i < expected_seeds.size(); ++i) {
                const auto world_x = origin.x + static_cast<jint>(indices[i]);
                legacy_random_source block{
                    mth_get_seed(world_x, origin.y, origin.z)};
                expected_seeds[i] = block.next_long();
            }
            structure::template_place_settings settings{};
            settings.processors = (*list)->processors;
            settings.level_seed = level_seed;
            worldgen_random placement{
                std::make_unique<legacy_random_source>(999)};
            settings.loot_random = &placement;
            proto_chunk chunk{states, 0, 0, 0, 32};
            chunk_region region{chunk, 0};
            (void)structure::place_template(region, states, templ, origin,
                                            settings);
            jint suspicious_count = 0;
            for (jint x = 0; x < 6; ++x) {
                const auto state =
                    chunk.state_at(origin.x + x, origin.y, origin.z);
                if (state == *sand) {
                    continue;
                }
                expect(state == *suspicious);
                ++suspicious_count;
                const auto *entity = chunk.block_entity_at(
                    block_pos{.x = origin.x + x, .y = origin.y, .z = origin.z});
                expect(entity != nullptr);
                if (entity == nullptr) {
                    continue;
                }
                const auto table = entity->find("LootTable");
                expect(table != entity->end());
                if (table != entity->end()) {
                    expect(table->second.as_string() ==
                           "minecraft:archaeology/trail_ruins_common");
                }
            }
            expect(suspicious_count == 2);
            for (std::size_t i = 0; i < expected_seeds.size(); ++i) {
                const auto *entity = chunk.block_entity_at(
                    block_pos{.x = origin.x + static_cast<jint>(indices[i]),
                              .y = origin.y,
                              .z = origin.z});
                expect(entity != nullptr);
                if (entity == nullptr) {
                    continue;
                }
                const auto seed = entity->find("LootTableSeed");
                expect(seed != entity->end());
                if (seed != entity->end()) {
                    expect(seed->second.as_long().value_or(0) ==
                           expected_seeds[i]);
                }
            }
        };

    "a capped uniform limit samples once before the shuffle"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        const auto air = states.intern("minecraft:air", {.air = true});
        const auto stone = states.intern("minecraft:stone",
                                         {.air = false, .blocks_motion = true});
        const auto dirt = states.intern("minecraft:dirt",
                                        {.air = false, .blocks_motion = true});
        expect(air && stone && dirt);
        if (!air || !stone || !dirt) {
            return;
        }
        const auto document = data::json::value::parse(R"({
            "processors": [
                {
                    "processor_type": "minecraft:capped",
                    "limit": {
                        "type": "minecraft:uniform",
                        "min_inclusive": 2,
                        "max_inclusive": 5
                    },
                    "delegate": {
                        "processor_type": "minecraft:rule",
                        "rules": [
                            {
                                "input_predicate": {
                                    "predicate_type": "minecraft:always_true"
                                },
                                "location_predicate": {
                                    "predicate_type": "minecraft:always_true"
                                },
                                "output_state": "minecraft:dirt"
                            }
                        ]
                    }
                }
            ]
        })");
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto list = decode_processor_list(states, *document);
        expect(list.has_value());
        if (!list) {
            return;
        }
        structure::structure_template templ{};
        templ.size_x = 8;
        templ.size_y = 1;
        templ.size_z = 1;
        templ.palettes.emplace_back();
        for (jint x = 0; x < 8; ++x) {
            templ.palettes.front().push_back(structure::template_block_info{
                .x = x, .y = 0, .z = 0, .state = *stone});
        }
        const block_pos origin{.x = 0, .y = 10, .z = 0};
        legacy_random_source base{77};
        legacy_random_source capped{mth_get_seed(origin.x, origin.y, origin.z) ^
                                    base.next_long()};
        const auto expected = 2 + capped.next_int(5 - 2 + 1);
        structure::template_place_settings settings{};
        settings.processors = list->get()->processors;
        settings.level_seed = 77;
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        (void)structure::place_template(region, states, templ, origin,
                                        settings);
        jint replaced = 0;
        for (jint x = 0; x < 8; ++x) {
            if (chunk.state_at(x, 10, 0) == *dirt) {
                ++replaced;
            }
        }
        expect(replaced == expected);
    };

    "rule block entity modifiers rewrite the processed tag"_test = [] {
        using namespace refrain::worldgen;
        struct case_definition {
            std::string modifier;
            data::nbt::compound existing;
            bool expect_empty;
            bool expect_passthrough;
            bool expect_loot;
        };
        const std::array cases{
            case_definition{
                .modifier = R"({"type": "minecraft:passthrough"})",
                .existing = {{"keep", data::nbt::value{std::string{"yes"}}}},
                .expect_empty = false,
                .expect_passthrough = true,
                .expect_loot = false,
            },
            case_definition{
                .modifier = R"({"type": "minecraft:clear"})",
                .existing = {{"keep", data::nbt::value{std::string{"yes"}}}},
                .expect_empty = true,
                .expect_passthrough = false,
                .expect_loot = false,
            },
            case_definition{
                .modifier = R"({
                    "type": "minecraft:append_static",
                    "data": {"level": 2, "nested": {"b": "two"}, "flag": true}
                })",
                .existing = {{"level",
                              data::nbt::value{static_cast<std::int8_t>(1)}},
                             {"nested", data::nbt::value{data::nbt::compound{
                                            {"a", data::nbt::value{std::string{
                                                      "one"}}}}}}},
                .expect_empty = false,
                .expect_passthrough = false,
                .expect_loot = false,
            },
            case_definition{
                .modifier = R"({
                    "type": "minecraft:append_loot",
                    "loot_table": "minecraft:chests/trial_chambers_supply"
                })",
                .existing = {{"keep", data::nbt::value{std::string{"yes"}}}},
                .expect_empty = false,
                .expect_passthrough = false,
                .expect_loot = true,
            },
        };
        for (const auto &definition : cases) {
            block_state_registry states;
            const auto air = states.intern("minecraft:air", {.air = true});
            const auto chest = states.intern(
                "minecraft:chest", {.air = false, .blocks_motion = true});
            expect(air && chest);
            if (!air || !chest) {
                continue;
            }
            const std::string json =
                std::string{
                    "{\"processors\": [{\"processor_type\": "
                    "\"minecraft:rule\", \"rules\": [{\"input_predicate\": "
                    "{\"predicate_type\": \"minecraft:always_true\"}, "
                    "\"location_predicate\": {\"predicate_type\": "
                    "\"minecraft:always_true\"}, \"output_state\": "
                    "\"minecraft:chest\", \"block_entity_modifier\": "} +
                definition.modifier + "}]}]}";
            const auto document = data::json::value::parse(json);
            expect(document.has_value());
            if (!document) {
                continue;
            }
            const auto list = decode_processor_list(states, *document);
            expect(list.has_value());
            if (!list) {
                continue;
            }
            structure::structure_template templ{};
            templ.size_x = 1;
            templ.size_y = 1;
            templ.size_z = 1;
            structure::template_block_info info{};
            info.state = *chest;
            info.nbt = std::make_shared<const data::nbt::compound>(
                definition.existing);
            templ.palettes.push_back({info});
            const block_pos origin{.x = 4, .y = 6, .z = 8};
            structure::template_place_settings settings{};
            settings.processors = (*list)->processors;
            settings.level_seed = 5;
            worldgen_random placement{
                std::make_unique<legacy_random_source>(1234)};
            const jlong placement_seed = placement.next_long();
            settings.loot_random = &placement;
            proto_chunk chunk{states, 0, 0, 0, 32};
            chunk_region region{chunk, 0};
            (void)structure::place_template(region, states, templ, origin,
                                            settings);
            const auto *entity = chunk.block_entity_at(origin);
            expect(entity != nullptr);
            if (entity == nullptr) {
                continue;
            }
            if (definition.expect_empty) {
                expect(entity->find("keep") == entity->end());
                expect(entity->find("LootTable") == entity->end());
                continue;
            }
            if (definition.expect_passthrough) {
                const auto keep = entity->find("keep");
                expect(keep != entity->end());
                if (keep != entity->end()) {
                    expect(keep->second.as_string() == "yes");
                }
                expect(entity->find("LootTable") == entity->end());
            }
            if (definition.expect_loot) {
                const auto table = entity->find("LootTable");
                expect(table != entity->end());
                if (table != entity->end()) {
                    expect(table->second.as_string() ==
                           "minecraft:chests/trial_chambers_supply");
                }
                legacy_random_source rule_random{
                    mth_get_seed(origin.x, origin.y, origin.z)};
                const jlong rule_seed = rule_random.next_long();
                const auto seed = entity->find("LootTableSeed");
                expect(seed != entity->end());
                if (seed != entity->end()) {
                    expect(seed->second.as_long().value_or(0) == rule_seed);
                    expect(rule_seed != placement_seed);
                }
            }
            if (!definition.expect_loot &&
                definition.modifier.find("append_static") !=
                    std::string::npos) {
                const auto nested = entity->find("nested");
                expect(nested != entity->end());
                if (nested != entity->end() &&
                    nested->second.as_compound() != nullptr) {
                    const auto *entries = nested->second.as_compound();
                    const auto a = entries->find("a");
                    const auto b = entries->find("b");
                    expect(a != entries->end() && b != entries->end());
                    if (a != entries->end() && b != entries->end()) {
                        expect(a->second.as_string() == "one");
                        expect(b->second.as_string() == "two");
                    }
                }
                const auto level = entity->find("level");
                expect(level != entity->end());
                if (level != entity->end()) {
                    expect(level->second.as_byte().value_or(0) == 2);
                }
                const auto flag = entity->find("flag");
                expect(flag != entity->end());
                if (flag != entity->end()) {
                    expect(flag->second.as_byte().value_or(0) == 1);
                }
            }
        }
    };

    "unknown processor and provider types are rejected"_test = [] {
        block_state_registry registry;
        const auto unknown_processor = data::json::value::parse(R"({
            "processors": [{"processor_type": "minecraft:does_not_exist"}]
        })");
        const auto unknown_provider = data::json::value::parse(R"({
            "processors": [{
                "processor_type": "minecraft:capped",
                "limit": {"type": "minecraft:does_not_exist"},
                "delegate": {"processor_type": "minecraft:nop"}
            }]
        })");
        const auto zero_limit = data::json::value::parse(R"({
            "processors": [{
                "processor_type": "minecraft:capped",
                "limit": {"type": "minecraft:uniform",
                          "min_inclusive": 0,
                          "max_inclusive": 2},
                "delegate": {"processor_type": "minecraft:nop"}
            }]
        })");
        expect(unknown_processor.has_value() && unknown_provider.has_value() &&
               zero_limit.has_value());
        if (!unknown_processor || !unknown_provider || !zero_limit) {
            return;
        }
        expect(
            !decode_processor_list(registry, *unknown_processor).has_value());
        expect(!decode_processor_list(registry, *unknown_provider).has_value());
        expect(!decode_processor_list(registry, *zero_limit).has_value());
    };
};
