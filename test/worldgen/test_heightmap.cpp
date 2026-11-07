// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/heightmap.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using refrain::worldgen::heightmap;
using refrain::worldgen::heightmap_kind;
using refrain::worldgen::heightmap_kind_count;
using refrain::worldgen::heightmap_source;
using refrain::worldgen::jint;
using refrain::worldgen::matches;
using refrain::worldgen::state_traits;

constexpr std::uint16_t air = 0;
constexpr std::uint16_t stone = 1;
constexpr std::uint16_t water = 2;
constexpr std::uint16_t leaves = 3;
constexpr std::uint16_t fence = 4;
constexpr jint test_min_y = 0;
constexpr jint test_height = 16;

constexpr std::array<state_traits, 5> traits_table{{
    {.air = true,
     .fluid = false,
     .blocks_motion = false,
     .blocks_motion_no_leaves = false},
    {.air = false,
     .fluid = false,
     .blocks_motion = true,
     .blocks_motion_no_leaves = true},
    {.air = false,
     .fluid = true,
     .blocks_motion = false,
     .blocks_motion_no_leaves = false},
    {.air = false,
     .fluid = false,
     .blocks_motion = true,
     .blocks_motion_no_leaves = false},
    {.air = false,
     .fluid = false,
     .blocks_motion = true,
     .blocks_motion_no_leaves = true},
}};

struct fake_chunk {
    std::vector<std::uint16_t> states =
        std::vector<std::uint16_t>(16U * 16U * 16U, air);

    auto at(jint x, jint y, jint z) -> std::uint16_t & {
        return states[static_cast<std::size_t>(y) * 256U +
                      static_cast<std::size_t>(z) * 16U +
                      static_cast<std::size_t>(x)];
    }
    [[nodiscard]] auto at(jint x, jint y, jint z) const -> std::uint16_t {
        return states[static_cast<std::size_t>(y) * 256U +
                      static_cast<std::size_t>(z) * 16U +
                      static_cast<std::size_t>(x)];
    }

    [[nodiscard]] auto source() const -> heightmap_source {
        return heightmap_source{
            .state_at = [this](jint x, jint y, jint z) { return at(x, y, z); },
            .traits_of =
                [](std::uint16_t state) { return traits_table[state]; },
        };
    }
};

auto reference_first_available(const fake_chunk &chunk, heightmap_kind kind,
                               jint x, jint z) -> jint {
    for (jint y = test_min_y + test_height - 1; y >= test_min_y; --y) {
        if (matches(kind, traits_table[chunk.at(x, y, z)])) {
            return y + 1;
        }
    }
    return test_min_y;
}

auto make(heightmap_kind kind) -> heightmap {
    return heightmap{kind, test_min_y, test_height};
}

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/heightmap"> heightmap_tests = [] {
    "empty columns store min_y"_test = [] {
        auto map = make(heightmap_kind::world_surface);
        expect(map.first_available(3, 7) == test_min_y);
        expect(map.highest_taken(3, 7) == test_min_y - 1);
        expect(map.empty());
    };

    "updates raise the top and removing the top rescans"_test = [] {
        fake_chunk chunk;
        const auto source = chunk.source();
        auto map = make(heightmap_kind::world_surface);

        chunk.at(2, 5, 9) = stone;
        map.update(2, 5, 9, stone, source);
        expect(map.first_available(2, 9) == 6);
        expect(map.highest_taken(2, 9) == 5);

        chunk.at(2, 10, 9) = stone;
        map.update(2, 10, 9, stone, source);
        expect(map.first_available(2, 9) == 11);

        chunk.at(2, 10, 9) = air;
        map.update(2, 10, 9, air, source);
        expect(map.first_available(2, 9) == 6);

        chunk.at(2, 5, 9) = air;
        map.update(2, 5, 9, air, source);
        expect(map.first_available(2, 9) == test_min_y);
        expect(map.empty());
    };

    "writes below the current top are ignored"_test = [] {
        fake_chunk chunk;
        const auto source = chunk.source();
        auto map = make(heightmap_kind::world_surface);

        chunk.at(1, 7, 1) = stone;
        map.update(1, 7, 1, stone, source);
        chunk.at(1, 4, 1) = stone;
        map.update(1, 4, 1, stone, source);
        expect(map.first_available(1, 1) == 8);
    };

    "ocean floor ignores fluids but motion blocking counts them"_test = [] {
        fake_chunk chunk;
        const auto source = chunk.source();
        auto ocean = make(heightmap_kind::ocean_floor);
        auto motion = make(heightmap_kind::motion_blocking);

        chunk.at(4, 8, 4) = water;
        ocean.update(4, 8, 4, water, source);
        motion.update(4, 8, 4, water, source);
        expect(ocean.first_available(4, 4) == test_min_y);
        expect(motion.first_available(4, 4) == 9);

        chunk.at(4, 6, 4) = stone;
        ocean.update(4, 6, 4, stone, source);
        expect(ocean.first_available(4, 4) == 7);
    };

    "no-leaves ignores leaves and counts fences"_test = [] {
        fake_chunk chunk;
        const auto source = chunk.source();
        auto leaves_map = make(heightmap_kind::motion_blocking_no_leaves);
        auto motion = make(heightmap_kind::motion_blocking);

        chunk.at(6, 8, 2) = leaves;
        leaves_map.update(6, 8, 2, leaves, source);
        motion.update(6, 8, 2, leaves, source);
        expect(leaves_map.first_available(6, 2) == test_min_y);
        expect(motion.first_available(6, 2) == 9);

        chunk.at(6, 7, 2) = fence;
        leaves_map.update(6, 7, 2, fence, source);
        expect(leaves_map.first_available(6, 2) == 8);
    };

    "prime recomputes every column"_test = [] {
        fake_chunk chunk;
        const auto source = chunk.source();
        auto map = make(heightmap_kind::motion_blocking);

        for (jint x = 0; x < 16; ++x) {
            chunk.at(x, x, 15 - x) = stone;
        }
        chunk.at(0, 3, 0) = water;
        map.prime(source);
        for (jint x = 0; x < 16; ++x) {
            expect(map.first_available(x, 15 - x) == x + 1);
        }
        expect(map.first_available(1, 1) == 0);
        expect(map.first_available(0, 0) == 4);

        chunk.at(0, 12, 0) = leaves;
        map.prime(source);
        expect(map.first_available(0, 0) == 13);
    };

    "incremental updates match a full scan"_test = [] {
        fake_chunk chunk;
        const auto source = chunk.source();
        std::array<heightmap, heightmap_kind_count> maps{
            make(heightmap_kind::world_surface_wg),
            make(heightmap_kind::world_surface),
            make(heightmap_kind::ocean_floor_wg),
            make(heightmap_kind::ocean_floor),
            make(heightmap_kind::motion_blocking),
            make(heightmap_kind::motion_blocking_no_leaves),
        };

        std::uint32_t state = 0x12345678U;
        constexpr std::array<std::uint16_t, 5> palette{air, stone, water,
                                                       leaves, fence};
        for (int step = 0; step < 600; ++step) {
            state = state * 1664525U + 1013904223U;
            const jint x = static_cast<jint>((state >> 8) & 15U);
            const jint z = static_cast<jint>((state >> 12) & 15U);
            const jint y = static_cast<jint>((state >> 16) % test_height);
            const auto value = palette[(state >> 20) % palette.size()];

            chunk.at(x, y, z) = value;
            for (std::size_t kind = 0; kind < maps.size(); ++kind) {
                maps[kind].update(x, y, z, value, source);
            }

            for (std::size_t kind = 0; kind < maps.size(); ++kind) {
                for (jint column_x = 0; column_x < 16; ++column_x) {
                    for (jint column_z = 0; column_z < 16; ++column_z) {
                        const auto expected = reference_first_available(
                            chunk, static_cast<heightmap_kind>(kind), column_x,
                            column_z);
                        if (maps[kind].first_available(column_x, column_z) !=
                            expected) {
                            expect(false)
                                << "step " << step << " kind " << kind
                                << " column " << column_x << "," << column_z;
                            return;
                        }
                    }
                }
            }
        }
        expect(true);
    };
};
