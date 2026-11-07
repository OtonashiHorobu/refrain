// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <optional>

namespace {

using refrain::worldgen::block_state_id;
using refrain::worldgen::block_state_registry;
using refrain::worldgen::chunk_status;
using refrain::worldgen::heightmap_kind;
using refrain::worldgen::jint;
using refrain::worldgen::proto_chunk;
using refrain::worldgen::density::sampler_context;
using refrain::worldgen::terrain::aquifer;
using refrain::worldgen::terrain::disabled_aquifer;
using refrain::worldgen::terrain::fill_proto_chunk;
using refrain::worldgen::terrain::fluid_picker;
using refrain::worldgen::terrain::proto_fill_options;

constexpr block_state_id air = 0;
constexpr block_state_id stone = 1;
constexpr block_state_id water = 2;
constexpr block_state_id lava = 3;

class water_aquifer final : public aquifer {
  public:
    [[nodiscard]] auto compute_substance(jint, jint, jint, double)
        -> std::optional<block_state_id> override {
        return water;
    }
    [[nodiscard]] auto should_schedule_fluid_update() const -> bool override {
        return true;
    }
};

struct fixture {
    block_state_registry registry{};
    block_state_id stone_id{};
    block_state_id water_id{};
    block_state_id lava_id{};

    fixture() {
        auto stone_state = registry.intern("minecraft:stone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true});
        auto water_state = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        auto lava_state =
            registry.intern("minecraft:lava", {.air = false, .fluid = true});
        if (stone_state && water_state && lava_state) {
            stone_id = *stone_state;
            water_id = *water_state;
            lava_id = *lava_state;
        }
    }
};

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/proto-fill"> proto_fill_tests = [] {
    "stone fill updates the worldgen heightmaps"_test = [] {
        fixture ids;
        proto_chunk chunk{ids.registry, 2, 3, 0, 32};
        chunk.set_status(chunk_status::biomes);

        auto sampler = density::constant(1.0F);
        const sampler_context context;
        fluid_picker picker{.sea_level = 8,
                            .default_fluid = ids.water_id,
                            .lava = ids.lava_id,
                            .air = ids.registry.air()};
        disabled_aquifer source{picker};
        const proto_fill_options options{
            .air = ids.registry.air(),
            .default_block = ids.stone_id,
            .default_fluid = ids.water_id,
            .lava = ids.lava_id,
        };
        fill_proto_chunk(chunk, options, *sampler, context, source);

        expect(chunk.state_at(0, 0, 0) == ids.stone_id);
        expect(chunk.state_at(15, 31, 15) == ids.stone_id);
        expect(chunk.first_available(heightmap_kind::world_surface_wg, 0, 0) ==
               32);
        expect(chunk.first_available(heightmap_kind::ocean_floor_wg, 0, 0) ==
               32);
        expect(chunk.first_available(heightmap_kind::world_surface, 0, 0) == 0);

        chunk.set_status(chunk_status::terrain);
        expect(chunk.first_available(heightmap_kind::world_surface, 0, 0) ==
               32);
        expect(chunk.first_available(heightmap_kind::ocean_floor, 0, 0) == 32);
        expect(chunk.first_available(heightmap_kind::motion_blocking, 0, 0) ==
               32);
    };

    "disabled aquifer writes its fluid column"_test = [] {
        fixture ids;
        proto_chunk chunk{ids.registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);

        auto sampler = density::constant(-1.0F);
        const sampler_context context;
        fluid_picker picker{.sea_level = 8,
                            .default_fluid = ids.water_id,
                            .lava = ids.lava_id,
                            .air = ids.registry.air()};
        disabled_aquifer source{picker};
        const proto_fill_options options{
            .air = ids.registry.air(),
            .default_block = ids.stone_id,
            .default_fluid = ids.water_id,
            .lava = ids.lava_id,
        };
        fill_proto_chunk(chunk, options, *sampler, context, source);

        expect(chunk.state_at(0, 0, 0) == ids.water_id);
        expect(chunk.state_at(0, 7, 0) == ids.water_id);
        expect(chunk.state_at(0, 8, 0) == ids.registry.air());
        expect(chunk.first_available(heightmap_kind::world_surface_wg, 0, 0) ==
               8);
        expect(chunk.first_available(heightmap_kind::ocean_floor_wg, 0, 0) ==
               0);
        expect(chunk.post_processing()[0].empty());
    };

    "fluid writes requested by the aquifer are marked"_test = [] {
        fixture ids;
        proto_chunk chunk{ids.registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);

        auto sampler = density::constant(1.0F);
        const sampler_context context;
        water_aquifer source;
        const proto_fill_options options{
            .air = ids.registry.air(),
            .default_block = ids.stone_id,
            .default_fluid = ids.water_id,
            .lava = ids.lava_id,
        };
        fill_proto_chunk(chunk, options, *sampler, context, source);

        expect(chunk.state_at(0, 0, 0) == ids.water_id);
        expect(chunk.post_processing().size() == 2);
        expect(chunk.post_processing()[0].size() == 4096);
        expect(chunk.post_processing()[1].size() == 4096);
    };
};
