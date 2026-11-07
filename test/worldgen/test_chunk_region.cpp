// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/chunk_region.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using refrain::worldgen::block_state_id;
using refrain::worldgen::block_state_table;
using refrain::worldgen::chunk_region;
using refrain::worldgen::heightmap_kind;
using refrain::worldgen::proto_chunk;
using refrain::worldgen::state_traits;

constexpr block_state_id air = 0;
constexpr block_state_id stone = 1;

class test_table final : public block_state_table {
  public:
    test_table() {
        names_ = {"minecraft:air", "minecraft:stone", "minecraft:water"};
        traits_ = {{
            {.air = true},
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true},
            {.air = false, .fluid = true},
        }};
    }

    [[nodiscard]] auto air() const -> block_state_id override { return 0; }
    [[nodiscard]] auto traits(block_state_id id) const
        -> state_traits override {
        return traits_[id];
    }
    [[nodiscard]] auto name(block_state_id id) const
        -> std::string_view override {
        return names_[id];
    }
    [[nodiscard]] auto in_tag(block_state_id, std::string_view) const
        -> bool override {
        return false;
    }

  private:
    std::vector<std::string> names_{};
    std::array<state_traits, 3> traits_{};
};

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/chunk-region"> chunk_region_tests = [] {
    "writes reach the 3x3 neighbourhood within the write radius"_test = [] {
        const test_table table;
        proto_chunk center{table, 0, 0, 0, 32};
        proto_chunk east{table, 1, 0, 0, 32};
        proto_chunk west{table, -1, 0, 0, 32};
        proto_chunk far{table, 2, 0, 0, 32};

        chunk_region region{center, 1};
        region.attach(east);
        region.attach(west);
        region.attach(far);

        expect(region.set_block_state(0, 1, 0, stone));
        expect(region.set_block_state(16, 1, 0, stone));
        expect(region.set_block_state(-1, 1, 0, stone));
        expect(!region.set_block_state(32, 1, 0, stone));
        expect(!region.set_block_state(0, 1, 16, stone));
        expect(region.dropped_writes() == 2U);

        expect(center.state_at(0, 1, 0) == stone);
        expect(east.state_at(0, 1, 0) == stone);
        expect(west.state_at(15, 1, 0) == stone);
        expect(far.state_at(0, 1, 0) == air);

        expect(region.state_at(16, 1, 0) == stone);
        expect(region.state_at(17, 1, 0) == air);
        expect(region.state_at(-1, 1, 0) == stone);
        expect(region.state_at(0, 1, 16) == air);
    };

    "a zero write radius only writes the centre"_test = [] {
        const test_table table;
        proto_chunk center{table, 0, 0, 0, 32};
        proto_chunk east{table, 1, 0, 0, 32};
        chunk_region region{center, 0};
        region.attach(east);

        expect(region.set_block_state(0, 2, 0, stone));
        expect(!region.set_block_state(16, 2, 0, stone));
        expect(region.dropped_writes() == 1U);
        expect(center.state_at(0, 2, 0) == stone);
        expect(east.state_at(0, 2, 0) == air);
    };

    "reads of attached chunks work across negative coordinates"_test = [] {
        const test_table table;
        proto_chunk center{table, 0, 0, 0, 32};
        proto_chunk northwest{table, -1, 1, 0, 32};
        chunk_region region{center, 1};
        region.attach(northwest);

        expect(region.set_block_state(-1, 3, 16, stone));
        expect(region.state_at(-1, 3, 16) == stone);
        expect(region.state_at(-16, 3, 16) == air);
        expect(northwest.state_at(15, 3, 0) == stone);
        expect(!region.has_chunk({.x = -2, .z = 1}));
        expect(region.find(-1, 1) != nullptr);
    };

    "the centre heightmaps are visible through the region"_test = [] {
        const test_table table;
        proto_chunk center{table, 0, 0, 0, 32};
        chunk_region region{center, 1};
        expect(region.set_block_state(2, 5, 3, stone));
        expect(region.first_available(heightmap_kind::world_surface_wg, 2, 3) ==
               6);
        expect(region.first_available(heightmap_kind::world_surface, 2, 3) ==
               0);
    };
};
