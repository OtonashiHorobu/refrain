// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using refrain::data::nbt::compound;
using refrain::data::nbt::value;
using refrain::worldgen::block_pos;
using refrain::worldgen::block_state_id;
using refrain::worldgen::block_state_table;
using refrain::worldgen::chunk_status;
using refrain::worldgen::heightmap_kind;
using refrain::worldgen::jint;
using refrain::worldgen::proto_chunk;
using refrain::worldgen::state_traits;

constexpr block_state_id air = 0;
constexpr block_state_id stone = 1;
constexpr block_state_id water = 2;
constexpr block_state_id leaves = 3;
constexpr block_state_id fence = 4;

class test_table final : public block_state_table {
  public:
    test_table() {
        names_ = {"minecraft:air", "minecraft:stone", "minecraft:water",
                  "minecraft:leaves", "minecraft:fence"};
        traits_ = {{
            {.air = true},
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true},
            {.air = false, .fluid = true},
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = false},
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true},
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
    std::array<state_traits, 5> traits_{};
};

auto first(const proto_chunk &chunk, heightmap_kind kind) -> jint {
    return chunk.first_available(kind, 1, 2);
}

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/proto-chunk"> proto_chunk_tests = [] {
    "writes update only the heightmaps of the current status"_test = [] {
        const test_table table;
        proto_chunk chunk{table, 3, -2, 0, 32};
        chunk.set_status(chunk_status::biomes);

        expect(chunk.set_state(1, 5, 2, stone));
        expect(first(chunk, heightmap_kind::world_surface_wg) == 6);
        expect(first(chunk, heightmap_kind::ocean_floor_wg) == 6);
        expect(first(chunk, heightmap_kind::world_surface) == 0);

        expect(chunk.set_state(1, 8, 2, water));
        expect(first(chunk, heightmap_kind::world_surface_wg) == 9);
        expect(first(chunk, heightmap_kind::ocean_floor_wg) == 6);

        chunk.set_status(chunk_status::terrain);
        expect(first(chunk, heightmap_kind::world_surface) == 9);
        expect(first(chunk, heightmap_kind::ocean_floor) == 6);
        expect(first(chunk, heightmap_kind::motion_blocking) == 9);
        expect(first(chunk, heightmap_kind::motion_blocking_no_leaves) == 9);

        expect(chunk.set_state(1, 10, 2, leaves));
        expect(first(chunk, heightmap_kind::motion_blocking) == 11);
        expect(first(chunk, heightmap_kind::motion_blocking_no_leaves) == 9);
        expect(first(chunk, heightmap_kind::world_surface_wg) == 9);

        expect(chunk.set_state(1, 10, 2, air));
        expect(first(chunk, heightmap_kind::motion_blocking) == 9);

        expect(chunk.set_state(1, 12, 2, fence));
        expect(first(chunk, heightmap_kind::motion_blocking) == 13);
        expect(first(chunk, heightmap_kind::motion_blocking_no_leaves) == 13);
    };

    "biomes address 4x4x4 cells"_test = [] {
        const test_table table;
        proto_chunk chunk{table, 0, 0, -64, 384};
        chunk.set_biome(3, 20, 7, 7);
        expect(chunk.biome_at(3, 20, 7) == 7);
        expect(chunk.biome_at(2, 21, 6) == 7);
        expect(chunk.biome_at(0, 23, 4) == 7);
        expect(chunk.biome_at(4, 20, 7) == 0);
        expect(chunk.biome_at(3, 19, 7) == 0);

        chunk.set_biome(15, -64, 15, 9);
        expect(chunk.biome_at(15, -64, 15) == 9);
        chunk.set_biome(16, 0, 0, 4);
        expect(chunk.biome_at(15, 0, 0) == 0);
    };

    "block entities and post processing keep positions"_test = [] {
        const test_table table;
        proto_chunk chunk{table, 0, 0, -64, 384};
        compound entity;
        entity["id"] = value{std::string{"minecraft:chest"}};
        chunk.set_block_entity({.x = 2, .y = 3, .z = 4}, entity);

        const auto *stored = chunk.block_entity_at({.x = 2, .y = 3, .z = 4});
        expect(stored != nullptr);
        if (stored != nullptr) {
            const auto id = stored->find("id");
            expect(id != stored->end());
            if (id != stored->end()) {
                expect(id->second.as_string() == "minecraft:chest");
            }
        }
        expect(chunk.block_entity_at({.x = 0, .y = 3, .z = 4}) == nullptr);

        chunk.mark_post_processing(3, 5, 7);
        chunk.mark_post_processing(15, 20, 15);
        expect(chunk.post_processing().size() == 24);
        expect(chunk.post_processing()[4].size() == 1);
        expect(chunk.post_processing()[5].size() == 1);
        expect(chunk.post_processing()[4][0] == 1875);
        expect(chunk.post_processing()[5][0] == 3919);

        chunk.remove_block_entity({.x = 2, .y = 3, .z = 4});
        expect(chunk.block_entity_at({.x = 2, .y = 3, .z = 4}) == nullptr);
    };

    "out of bounds writes do not change the chunk"_test = [] {
        const test_table table;
        proto_chunk chunk{table, 0, 0, 0, 32};
        expect(!chunk.set_state(1, -1, 1, stone));
        expect(!chunk.set_state(16, 1, 1, stone));
        expect(chunk.state_at(1, -1, 1) == air);
        expect(chunk.state_at(16, 1, 1) == air);
        expect(chunk.state_at(1, 32, 1) == air);
    };

    "structure starts and references accumulate per chunk"_test = [] {
        const test_table table;
        proto_chunk chunk{table, 0, 0, 0, 32};
        chunk.add_structure_start(
            structure_start{.structure = "minecraft:village_plains",
                            .chunk_x = 3,
                            .chunk_z = 4,
                            .min_x = 40,
                            .min_y = 60,
                            .min_z = 50,
                            .max_x = 70,
                            .max_y = 80,
                            .max_z = 90});
        chunk.add_structure_reference("minecraft:village_plains", 3, 4);
        chunk.add_structure_reference("minecraft:village_plains", 3, 4, 2);
        chunk.add_structure_reference("minecraft:mineshaft", 5, 6);
        expect(chunk.structure_starts().size() == 1U);
        expect(chunk.structure_references().size() == 2U);
        expect(chunk.structure_references()[0].count == 3);
    };

    "status ordering and names"_test = [] {
        expect(status_name(chunk_status::terrain) == "terrain");
        expect(status_name(chunk_status::full) == "full");
        expect(status_at_least(chunk_status::features, chunk_status::terrain));
        expect(!status_at_least(chunk_status::biomes, chunk_status::terrain));
    };
};
