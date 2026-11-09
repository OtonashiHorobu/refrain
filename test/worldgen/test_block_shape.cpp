// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Synthetic coverage of the collision-face model features use in place of
// block shapes: a shape family answers per face and the motion tags alone do
// not make a thin block sturdy.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/block_shape.hpp>

#include <boost/ut.hpp>

#include <array>
#include <string_view>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::feature;

auto make_state(block_state_registry &registry, std::string_view key)
    -> block_state_id {
    const auto interned =
        registry.intern(key, state_traits{.air = false,
                                          .blocks_motion = true,
                                          .blocks_motion_no_leaves = true});
    return interned.has_value() ? *interned : no_block_state;
}

auto sturdy(const block_state_registry &registry, block_state_id state,
            block_face face) -> bool {
    return is_sturdy_face(registry, state, face);
}

} // namespace

suite<"worldgen/block-shape"> block_shape_tests = [] {
    "thin shapes offer no sturdy face while full cubes do"_test = [] {
        block_state_registry registry;
        const auto stone = make_state(registry, "minecraft:stone");
        const auto dripstone =
            make_state(registry, "minecraft:pointed_dripstone[thickness=tip,"
                                 "tip_direction=down,waterlogged=false]");
        const auto cave_vines =
            make_state(registry, "minecraft:cave_vines[age=25,berries=false]");
        const auto vein = make_state(
            registry, "minecraft:sculk_vein[down=true,east=false,north=false,"
                      "south=false,up=false,waterlogged=false,west=false]");
        const auto sensor = make_state(
            registry,
            "minecraft:sculk_sensor[power=0,sculk_sensor_phase=inactive,"
            "waterlogged=false]");
        expect(stone != no_block_state && dripstone != no_block_state &&
               cave_vines != no_block_state && vein != no_block_state &&
               sensor != no_block_state);
        if (stone == no_block_state || dripstone == no_block_state ||
            cave_vines == no_block_state || vein == no_block_state ||
            sensor == no_block_state) {
            return;
        }
        const std::array<block_state_id, 2> speleothems{dripstone};
        registry.add_tag("minecraft:speleothems", speleothems);
        const std::array<block_state_id, 1> vines{cave_vines};
        registry.add_tag("minecraft:cave_vines", vines);
        const std::array<block_state_id, 1> inhibitors{sensor};
        registry.add_tag("minecraft:sculk_growth_inhibitors", inhibitors);

        for (const auto face :
             {block_face::down, block_face::up, block_face::north,
              block_face::south, block_face::west, block_face::east}) {
            expect(sturdy(registry, stone, face));
            expect(!sturdy(registry, dripstone, face));
            expect(!sturdy(registry, cave_vines, face));
            expect(!sturdy(registry, vein, face));
        }
        // An eight pixel tall sensor only reaches the bottom boundary.
        expect(sturdy(registry, sensor, block_face::down));
        expect(!sturdy(registry, sensor, block_face::up));
        expect(!sturdy(registry, sensor, block_face::north));
    };

    "slabs, stairs, trapdoors and doors answer by their state shape"_test = [] {
        block_state_registry registry;
        const auto slab_bottom = make_state(
            registry, "minecraft:oak_slab[type=bottom,waterlogged=false]");
        const auto slab_top = make_state(
            registry, "minecraft:oak_slab[type=top,waterlogged=false]");
        const auto slab_double = make_state(
            registry, "minecraft:oak_slab[type=double,waterlogged=false]");
        const auto stair = make_state(
            registry, "minecraft:oak_stairs[facing=north,half=bottom,"
                      "shape=straight,waterlogged=false]");
        const auto stair_inner = make_state(
            registry, "minecraft:oak_stairs[facing=north,half=bottom,"
                      "shape=inner_right,waterlogged=false]");
        const auto stair_outer = make_state(
            registry, "minecraft:oak_stairs[facing=north,half=bottom,"
                      "shape=outer_left,waterlogged=false]");
        const auto trapdoor_closed = make_state(
            registry,
            "minecraft:oak_trapdoor[facing=north,half=bottom,open=false,"
            "powered=false,waterlogged=false]");
        const auto trapdoor_open = make_state(
            registry,
            "minecraft:oak_trapdoor[facing=north,half=bottom,open=true,"
            "powered=false,waterlogged=false]");
        const auto door = make_state(
            registry, "minecraft:oak_door[facing=north,half=lower,hinge=left,"
                      "open=false,powered=false]");
        const std::array<block_state_id, 1> slabs{slab_bottom};
        registry.add_tag("minecraft:slabs", slabs);
        const std::array<block_state_id, 1> stairs{stair};
        registry.add_tag("minecraft:stairs", stairs);
        const std::array<block_state_id, 1> trapdoors{trapdoor_closed};
        registry.add_tag("minecraft:trapdoors", trapdoors);
        const std::array<block_state_id, 1> doors{door};
        registry.add_tag("minecraft:doors", doors);

        expect(sturdy(registry, slab_bottom, block_face::down));
        expect(!sturdy(registry, slab_bottom, block_face::up));
        expect(sturdy(registry, slab_top, block_face::up));
        expect(!sturdy(registry, slab_top, block_face::down));
        for (const auto face :
             {block_face::down, block_face::up, block_face::north,
              block_face::south, block_face::west, block_face::east}) {
            expect(sturdy(registry, slab_double, face));
        }

        expect(sturdy(registry, stair, block_face::north));
        expect(sturdy(registry, stair, block_face::down));
        expect(!sturdy(registry, stair, block_face::up));
        expect(!sturdy(registry, stair, block_face::south));
        // A straight stair exposes one tall side; an inner corner exposes
        // two and an outer corner only its base face.
        expect(sturdy(registry, stair_inner, block_face::north));
        expect(sturdy(registry, stair_inner, block_face::east));
        expect(sturdy(registry, stair_outer, block_face::down));
        expect(!sturdy(registry, stair_outer, block_face::north));

        expect(sturdy(registry, trapdoor_closed, block_face::down));
        expect(!sturdy(registry, trapdoor_closed, block_face::up));
        expect(sturdy(registry, trapdoor_open, block_face::south));
        expect(!sturdy(registry, trapdoor_open, block_face::north));
        expect(sturdy(registry, door, block_face::south));
        expect(!sturdy(registry, door, block_face::north));
    };

    "walls, fences and panes stay thin at every face"_test = [] {
        block_state_registry registry;
        const auto wall = make_state(
            registry,
            "minecraft:cobblestone_wall[east=none,north=none,south=none,"
            "up=true,waterlogged=false,west=none]");
        const auto fence = make_state(
            registry, "minecraft:oak_fence[east=false,north=false,south=false,"
                      "waterlogged=false,west=false]");
        const auto pane = make_state(
            registry, "minecraft:glass_pane[east=false,north=false,south=false,"
                      "waterlogged=false,west=false]");
        const std::array<block_state_id, 1> walls{wall};
        registry.add_tag("minecraft:walls", walls);
        const std::array<block_state_id, 1> fences{fence};
        registry.add_tag("minecraft:fences", fences);
        for (const auto face :
             {block_face::down, block_face::up, block_face::north,
              block_face::south, block_face::west, block_face::east}) {
            expect(!sturdy(registry, wall, face));
            expect(!sturdy(registry, fence, face));
            expect(!sturdy(registry, pane, face));
        }
    };
};
