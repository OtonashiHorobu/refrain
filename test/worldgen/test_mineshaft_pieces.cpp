// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/start.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <string_view>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

struct fixture {
    block_state_registry states;
    proto_chunk chunk;
    chunk_region region;
    structure::structure_place_context context;

    fixture()
        : chunk{[this] {
              (void)states.intern("minecraft:air", {.air = true});
              (void)states.intern("minecraft:cave_air", {.air = true});
              (void)states.intern("minecraft:stone",
                                  {.air = false,
                                   .blocks_motion = true,
                                   .blocks_motion_no_leaves = true});
              (void)states.intern("minecraft:oak_planks",
                                  {.air = false,
                                   .blocks_motion = true,
                                   .blocks_motion_no_leaves = true});
              (void)states.intern("minecraft:dark_oak_planks",
                                  {.air = false,
                                   .blocks_motion = true,
                                   .blocks_motion_no_leaves = true});
              return proto_chunk{states, 0, 0, 0, 32};
          }()},
          region{chunk, 0},
          context{.region = &region,
                  .chunk = &chunk,
                  .states = &states,
                  .state = [this](std::string_view key) -> block_state_id {
                      if (const auto found = states.find(key); found) {
                          return *found;
                      }
                      const auto block = state_block_name(key);
                      const bool air = is_air_block_name(block);
                      return states
                          .intern(key, {.air = air,
                                        .blocks_motion = !air,
                                        .blocks_motion_no_leaves = !air})
                          .value_or(no_block_state);
                  },
                  .registry = &states,
                  .seed = 1} {
        const auto stone = *states.find("minecraft:stone");
        for (jint y = 0; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    chunk.set_state(x, y, z, stone);
                }
            }
        }
        chunk.set_status(chunk_status::terrain);
    }

    [[nodiscard]] auto name(jint x, jint y, jint z) const -> std::string_view {
        return states.name(chunk.state_at(x, y, z));
    }
};

auto make_random() -> worldgen_random {
    return worldgen_random{std::make_unique<xoroshiro_random_source>(1)};
}

auto make_piece(std::string type)
    -> ::refrain::worldgen::structure_piece_record {
    ::refrain::worldgen::structure_piece_record record{};
    record.type = std::move(type);
    record.min_y = 10;
    return record;
}

} // namespace

suite<"worldgen/mineshaft-pieces"> mineshaft_piece_tests = [] {
    "a room clears its interior, entrances and upper half sphere"_test = [] {
        fixture world;
        auto record = make_piece("minecraft:msroom");
        record.min_x = 0;
        record.min_z = 0;
        record.max_x = 7;
        record.max_y = 18;
        record.max_z = 7;
        record.entrances.push_back(structure::structure_bbox{.min_x = 8,
                                                             .min_y = 10,
                                                             .min_z = 2,
                                                             .max_x = 8,
                                                             .max_y = 12,
                                                             .max_z = 4});
        auto random = make_random();
        structure::place_structure_piece(world.context, record, random);
        expect(world.name(0, 11, 0) == "minecraft:cave_air");
        expect(world.name(4, 13, 4) == "minecraft:cave_air");
        expect(world.name(8, 11, 3) == "minecraft:cave_air");
        expect(world.name(0, 9, 0) == "minecraft:stone");
        expect(world.name(12, 11, 0) == "minecraft:stone");
    };

    "a two-floored crossing clears both floors and supports pillars"_test = [] {
        fixture world;
        auto record = make_piece("minecraft:mscrossing");
        record.min_x = 0;
        record.min_z = 0;
        record.max_x = 4;
        record.max_y = 16;
        record.max_z = 4;
        record.two_floored = true;
        auto random = make_random();
        structure::place_structure_piece(world.context, record, random);
        expect(world.name(1, 10, 0) == "minecraft:cave_air");
        expect(world.name(1, 14, 0) == "minecraft:cave_air");
        expect(world.name(1, 10, 1) == "minecraft:oak_planks");
        expect(world.name(0, 16, 0) == "minecraft:stone");
    };

    "stairs clear their steps in local coordinates"_test = [] {
        fixture world;
        auto record = make_piece("minecraft:msstairs");
        record.min_x = 0;
        record.min_z = 0;
        record.max_x = 2;
        record.max_y = 17;
        record.max_z = 8;
        record.orientation = 0;
        auto random = make_random();
        structure::place_structure_piece(world.context, record, random);
        expect(world.name(0, 16, 0) == "minecraft:cave_air");
        expect(world.name(0, 11, 8) == "minecraft:cave_air");
        expect(world.name(2, 12, 4) == "minecraft:cave_air");
        expect(world.name(2, 8, 0) == "minecraft:stone");
    };

    "a corridor clears its tube and keeps the surrounding stone"_test = [] {
        fixture world;
        auto record = make_piece("minecraft:mscorridor");
        record.min_x = 0;
        record.min_z = 0;
        record.max_x = 2;
        record.max_y = 20;
        record.max_z = 4;
        record.sections = 1;
        record.orientation = 0;
        const auto air = *world.states.find("minecraft:air");
        world.chunk.set_state(0, 13, 2, air);
        world.chunk.set_state(1, 13, 2, air);
        world.chunk.set_state(2, 13, 2, air);
        auto random = make_random();
        structure::place_structure_piece(world.context, record, random);
        expect(world.name(1, 10, 0) == "minecraft:cave_air");
        expect(world.name(1, 11, 4) == "minecraft:cave_air");
        expect(world.name(1, 11, 5) == "minecraft:stone");
        expect(world.name(1, 9, 0) == "minecraft:stone");
    };

    "a railed corridor places rails on its solid floor"_test = [] {
        fixture world;
        auto record = make_piece("minecraft:mscorridor");
        record.min_x = 0;
        record.min_z = 0;
        record.max_x = 2;
        record.max_y = 20;
        record.max_z = 14;
        record.sections = 3;
        record.rails = true;
        record.orientation = 0;
        auto random = make_random();
        structure::place_structure_piece(world.context, record, random);
        std::size_t rails = 0;
        for (jint z = 0; z <= 14; ++z) {
            if (world.name(1, 10, z) == "minecraft:rail[shape=north_south]") {
                ++rails;
            }
        }
        expect(rails > 0);
    };
};
