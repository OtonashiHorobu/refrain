// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The per-chunk status pipeline as the runtime scheduler drives it: the three
// status steps produce the same chunk as the one-call entry point, the content
// does not depend on the configured tile edge, and the natural-biome output
// matches the reference single-chunk mode. The source datapack is local and
// not committed, so every case skips with one diagnostic when the checkout has
// no copy.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace worldgen = refrain::worldgen;
namespace test_support = refrain::test_support;

constexpr worldgen::jlong test_seed = 12345;
/// A structure set that does not exist turns structure starts off while
/// leaving the feature stage active.
constexpr std::string_view no_structure_set =
    "oracle:pipeline_no_structure_set";
constexpr std::string_view fixed_biome = "minecraft:plains";

auto create_generator(worldgen::jint tile_size, std::string_view biome)
    -> refrain::result<std::unique_ptr<worldgen::chunk_generator>> {
    return worldgen::chunk_generator::create(worldgen::chunk_generator_config{
        .data_root = test_support::vanilla_data_root(),
        .templates_root = "",
        .seed = test_seed,
        .kind = worldgen::dimension_kind::overworld,
        .fixed_biome = std::string{biome},
        .target = worldgen::generation_target::full,
        .structure_sets = {std::string{no_structure_set}},
        .tile_size = tile_size,
        .proto_cache_capacity = 128,
    });
}

/// The reference single-chunk mode: decorates the target and its eight
/// neighbours through one region in force-load order.
auto create_reference(worldgen::jint tile_size, std::string_view biome)
    -> refrain::result<std::unique_ptr<worldgen::chunk_generator>> {
    return worldgen::chunk_generator::create(worldgen::chunk_generator_config{
        .data_root = test_support::vanilla_data_root(),
        .templates_root = "",
        .seed = test_seed,
        .kind = worldgen::dimension_kind::overworld,
        .fixed_biome = std::string{biome},
        .target = worldgen::generation_target::full,
        .structure_sets = {std::string{no_structure_set}},
        .decorate_neighbours = true,
        .neighbours_in_generation_order = true,
        .neighbour_box =
            worldgen::chunk_rectangle{
                .min_x = -1, .min_z = -1, .max_x = 1, .max_z = 1},
        .tile_size = tile_size,
        .proto_cache_capacity = 128,
    });
}

auto mix(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    constexpr std::uint64_t multiplier = 1099511628211ULL;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= multiplier;
    }
    return hash;
}

auto mix_bytes(std::uint64_t hash, std::string_view text) -> std::uint64_t {
    constexpr std::uint64_t multiplier = 1099511628211ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= multiplier;
    }
    return hash;
}

/// Blocks, biomes and the final heightmaps of one chunk as one value. Two
/// chunks with the same digest are interchangeable for every consumer the
/// pipeline serves.
auto chunk_digest(const worldgen::proto_chunk &chunk,
                  const std::vector<std::string> &biome_names)
    -> std::uint64_t {
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    hash = mix(hash, static_cast<std::uint64_t>(chunk.chunk_x()));
    hash = mix(hash, static_cast<std::uint64_t>(chunk.chunk_z()));
    for (worldgen::jint y = chunk.min_y(); y < chunk.min_y() + chunk.height();
         ++y) {
        for (worldgen::jint z = 0; z < 16; ++z) {
            for (worldgen::jint x = 0; x < 16; ++x) {
                hash = mix_bytes(hash,
                                 chunk.table().name(chunk.state_at(x, y, z)));
            }
        }
    }
    for (worldgen::jint y = chunk.min_y(); y < chunk.min_y() + chunk.height();
         y += 4) {
        for (worldgen::jint z = 0; z < 16; z += 4) {
            for (worldgen::jint x = 0; x < 16; x += 4) {
                const auto biome = chunk.biome_at(x, y, z);
                hash = mix_bytes(hash, biome < biome_names.size()
                                           ? biome_names[biome]
                                           : std::string_view{"unknown"});
            }
        }
    }
    for (const auto kind :
         {worldgen::heightmap_kind::world_surface,
          worldgen::heightmap_kind::ocean_floor,
          worldgen::heightmap_kind::motion_blocking,
          worldgen::heightmap_kind::motion_blocking_no_leaves}) {
        for (worldgen::jint z = 0; z < 16; ++z) {
            for (worldgen::jint x = 0; x < 16; ++x) {
                hash = mix(hash,
                           static_cast<std::uint64_t>(
                               chunk.heightmap_at(kind).first_available(x, z)));
            }
        }
    }
    return hash;
}

} // namespace

suite<"server/chunk-generation"> chunk_generation_tests = [] {
    "server/chunk status steps build the same chunk as the pipeline"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        auto stepped{create_generator(8, fixed_biome)};
        auto whole{create_generator(8, fixed_biome)};
        expect(stepped.has_value()) << (stepped ? "" : stepped.error().message);
        expect(whole.has_value()) << (whole ? "" : whole.error().message);
        if (!stepped || !whole) {
            return;
        }
        const worldgen::chunk_pos probe{.x = 0, .z = 0};
        expect(static_cast<bool>((*stepped)->advance_terrain(probe)));
        expect(static_cast<bool>((*stepped)->advance_decorated(probe)));
        auto served{(*stepped)->take_served(probe)};
        auto expected{(*whole)->generate_chunk(probe)};
        expect(served.has_value()) << (served ? "" : served.error().message);
        expect(expected.has_value())
            << (expected ? "" : expected.error().message);
        if (!served || !expected) {
            return;
        }
        const auto names{(*stepped)->biome_names()};
        expect(chunk_digest(**served, names) == chunk_digest(**expected, names))
            << "the status steps and the one-call path produced different "
               "chunks";
        expect((*served)->status() == (*expected)->status());
    };

    "server/chunk decorated step fills what the terrain step left out"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // A worker may receive the decorated job after a restart without
            // the terrain job having run; the step is self-sufficient.
            auto stepped{create_generator(8, fixed_biome)};
            auto whole{create_generator(8, fixed_biome)};
            expect(stepped.has_value());
            expect(whole.has_value());
            if (!stepped || !whole) {
                return;
            }
            const worldgen::chunk_pos probe{.x = 1, .z = 0};
            expect(static_cast<bool>((*stepped)->advance_decorated(probe)));
            auto served{(*stepped)->take_served(probe)};
            auto expected{(*whole)->generate_chunk(probe)};
            expect(served.has_value());
            expect(expected.has_value());
            if (!served || !expected) {
                return;
            }
            const auto names{(*stepped)->biome_names()};
            expect(chunk_digest(**served, names) ==
                   chunk_digest(**expected, names));
        };

    "server/chunk pipeline content ignores the configured tile edge"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        // The scheduler never generates a tile: the three status jobs are
        // per-chunk. The configured edge must therefore not reach the served
        // content, with the dimension's own biomes included.
        const std::vector<worldgen::chunk_pos> probes{
            {.x = 0, .z = 0}, {.x = 3, .z = 3}, {.x = 1, .z = 2}};
        std::vector<std::uint64_t> digests(probes.size(), 0);
        for (const worldgen::jint tile_size : {2, 4, 8}) {
            auto generator{create_generator(tile_size, {})};
            expect(generator.has_value())
                << (generator ? "" : generator.error().message);
            if (!generator) {
                return;
            }
            for (std::size_t index = 0; index < probes.size(); ++index) {
                auto served{(*generator)->generate_chunk(probes[index])};
                expect(served.has_value())
                    << (served ? "" : served.error().message);
                if (!served) {
                    return;
                }
                const auto digest{
                    chunk_digest(**served, (*generator)->biome_names())};
                if (tile_size == 2) {
                    digests[index] = digest;
                } else {
                    expect(digest == digests[index])
                        << "tile edge " << tile_size << " changed chunk ("
                        << probes[index].x << ", " << probes[index].z << ")";
                }
            }
        }
    };

    "server/chunk pipeline matches the natural-biome reference"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        auto pipeline{create_generator(8, {})};
        auto reference{create_reference(8, {})};
        expect(pipeline.has_value())
            << (pipeline ? "" : pipeline.error().message);
        expect(reference.has_value())
            << (reference ? "" : reference.error().message);
        if (!pipeline || !reference) {
            return;
        }
        const worldgen::chunk_pos probe{.x = 0, .z = 0};
        auto served{(*pipeline)->generate_chunk(probe)};
        auto expected{(*reference)->generate(probe)};
        expect(served.has_value()) << (served ? "" : served.error().message);
        expect(expected.has_value())
            << (expected ? "" : expected.error().message);
        if (!served || !expected) {
            return;
        }
        const auto names{(*pipeline)->biome_names()};
        expect(chunk_digest(**served, names) ==
               chunk_digest(**expected, names));
    };

    "server/chunk repeated status requests hit the cache"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        auto generator{create_generator(8, fixed_biome)};
        expect(generator.has_value());
        if (!generator) {
            return;
        }
        const worldgen::chunk_pos probe{.x = 0, .z = 0};
        const auto first_before{(*generator)->cache_stats()};
        expect(static_cast<bool>((*generator)->advance_terrain(probe)));
        expect(static_cast<bool>((*generator)->advance_decorated(probe)));
        auto first{(*generator)->take_served(probe)};
        const auto first_stats{(*generator)->cache_stats()};
        expect(first.has_value());
        if (!first || first_stats.entries <= first_before.entries) {
            return;
        }
        expect(static_cast<bool>((*generator)->advance_terrain(probe)));
        expect(static_cast<bool>((*generator)->advance_decorated(probe)));
        auto second{(*generator)->take_served(probe)};
        const auto second_stats{(*generator)->cache_stats()};
        expect(second.has_value());
        if (!second) {
            return;
        }
        const auto names{(*generator)->biome_names()};
        expect(chunk_digest(**second, names) == chunk_digest(**first, names));
        expect(second_stats.entries == first_stats.entries)
            << "a repeated request added cache entries";
        expect(second_stats.hits > first_stats.hits)
            << "a repeated request did not hit the cache";
    };

    "server/chunk cache unload rebuilds the same chunk"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        auto generator{create_generator(8, fixed_biome)};
        expect(generator.has_value());
        if (!generator) {
            return;
        }
        const worldgen::chunk_pos probe{.x = 0, .z = 0};
        auto first{(*generator)->generate_chunk(probe)};
        expect(first.has_value());
        if (!first) {
            return;
        }
        const auto names{(*generator)->biome_names()};
        const auto digest{chunk_digest(**first, names)};
        (*generator)->clear_proto_cache();
        expect((*generator)->cache_stats().entries == 0U);
        auto second{(*generator)->generate_chunk(probe)};
        expect(second.has_value());
        if (!second) {
            return;
        }
        expect(chunk_digest(**second, names) == digest);
    };
};
