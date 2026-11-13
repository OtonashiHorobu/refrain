// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The production tile path against the single-chunk reference mode. A tile
// fills its square plus one terrain ring and decorates its square plus one
// ring in force-load order, so every served chunk holds the feature writes its
// eight neighbours made across its border. The reference mode stages the same
// region one chunk at a time, so the two must agree block for block: the
// production tile on a fixed-biome chunk and on every chunk of a three-by-three
// tile, and with per-chunk biome plans when no biome is fixed.
//
// The datapack snapshot is local and not committed, so every case skips with
// one diagnostic when the checkout has no copy.

#include <refrain/worldgen/chunk_generator.hpp>

#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

constexpr jlong test_seed = 12345;
constexpr std::string_view fixed_biome = "minecraft:plains";
/// A structure set that does not exist disables structure starts while
/// leaving the feature stage active, so the comparison covers decoration.
constexpr std::string_view no_structure_set = "oracle:tile_no_structure_set";

auto source_data_root() -> std::string {
    return refrain::test_support::vanilla_data_root();
}

/// Structure sets for a case: `nullopt` keeps starts off with a set that does
/// not exist, an empty list enables every shipped set.
using structure_sets = std::optional<std::vector<std::string>>;

auto selected_structure_sets(const structure_sets &sets)
    -> std::vector<std::string> {
    if (sets.has_value()) {
        return *sets;
    }
    return {std::string{no_structure_set}};
}

/// A production tile generator. The tile edge comes from the configuration,
/// exactly as the runtime service sets it.
auto create_tiled(jint tile_size, std::string_view biome,
                  const structure_sets &sets = std::nullopt)
    -> result<std::unique_ptr<chunk_generator>> {
    return chunk_generator::create(chunk_generator_config{
        .data_root = source_data_root(),
        .templates_root = "",
        .seed = test_seed,
        .kind = dimension_kind::overworld,
        .fixed_biome = std::string{biome},
        .target = generation_target::full,
        .structure_sets = selected_structure_sets(sets),
        .tile_size = tile_size,
    });
}

/// The single-chunk reference mode for one target. With a `box` it decorates
/// exactly the chunks inside that rectangle in force-load order and attaches
/// one more terrain ring; the box is how a reference run mirrors the region a
/// production tile stages. Without a box it decorates the target's eight
/// neighbours, the mode a saved reference world is compared with.
auto create_reference(chunk_rectangle box, std::string_view biome,
                      const structure_sets &sets = std::nullopt)
    -> result<std::unique_ptr<chunk_generator>> {
    return chunk_generator::create(chunk_generator_config{
        .data_root = source_data_root(),
        .templates_root = "",
        .seed = test_seed,
        .kind = dimension_kind::overworld,
        .fixed_biome = std::string{biome},
        .target = generation_target::full,
        .structure_sets = selected_structure_sets(sets),
        .decorate_neighbours = true,
        .neighbours_in_generation_order = true,
        .neighbour_box = box,
    });
}

auto block_difference_count(const proto_chunk &left, const proto_chunk &right)
    -> std::size_t {
    if (left.min_y() != right.min_y() || left.height() != right.height()) {
        return 1;
    }
    std::size_t count = 0;
    for (jint y = left.min_y(); y < left.min_y() + left.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                if (left.table().name(left.state_at(x, y, z)) !=
                    right.table().name(right.state_at(x, y, z))) {
                    ++count;
                }
            }
        }
    }
    return count;
}

/// The rectangle a tile of `tile_size` chunks serves when its minimum corner
/// is `origin`.
auto tile_served(chunk_pos origin, jint tile_size) -> chunk_rectangle {
    return chunk_rectangle{.min_x = origin.x,
                           .min_z = origin.z,
                           .max_x = origin.x + tile_size - 1,
                           .max_z = origin.z + tile_size - 1};
}

/// The rectangle a tile decorates: itself and one ring.
auto tile_decorated(chunk_pos origin, jint tile_size) -> chunk_rectangle {
    return chunk_rectangle{.min_x = origin.x - 1,
                           .min_z = origin.z - 1,
                           .max_x = origin.x + tile_size,
                           .max_z = origin.z + tile_size};
}

/// Compares every served chunk of one tile with a reference run of the same
/// decorated rectangle.
auto compare_tile_with_reference(chunk_pos origin, jint tile_size,
                                 std::string_view biome,
                                 const structure_sets &sets = std::nullopt)
    -> std::size_t {
    auto tiled = create_tiled(tile_size, biome, sets);
    expect(tiled.has_value()) << (tiled ? "" : tiled.error().message);
    if (!tiled) {
        return 0;
    }
    auto reference =
        create_reference(tile_decorated(origin, tile_size), biome, sets);
    expect(reference.has_value())
        << (reference ? "" : reference.error().message);
    if (!reference) {
        return 0;
    }
    auto chunks = (*tiled)->generate_tile(origin);
    expect(chunks.has_value()) << (chunks ? "" : chunks.error().message);
    if (!chunks) {
        return 0;
    }
    const auto served = tile_served(origin, tile_size);
    expect(chunks->size() ==
           static_cast<std::size_t>(served.max_x - served.min_x + 1) *
               static_cast<std::size_t>(served.max_z - served.min_z + 1));
    std::size_t differences = 0;
    for (const auto &chunk : *chunks) {
        const chunk_pos position{.x = chunk->chunk_x(), .z = chunk->chunk_z()};
        expect(position.x >= served.min_x && position.x <= served.max_x);
        expect(position.z >= served.min_z && position.z <= served.max_z);
        auto expected = (*reference)->generate(position);
        expect(expected.has_value())
            << (expected ? "" : expected.error().message);
        if (!expected) {
            return 0;
        }
        differences += block_difference_count(*chunk, **expected);
    }
    return differences;
}

} // namespace

suite<"worldgen/tile-decoration"> tile_decoration_tests = [] {
    "the production tile reproduces the reference mode on the probed chunk"_test =
        [] {
            if (!refrain::test_support::vanilla_data().available) {
                return;
            }
            // The probe chunk (5,5) at seed 12345 with the fixed plains biome
            // differed from the reference mode in 327 blocks, 167 of them on
            // border columns, while production decorated the target chunk
            // only. A tile that contains it must now reach zero.
            constexpr chunk_pos origin{.x = 3, .z = 3};
            constexpr jint tile_size = 3;
            auto tiled = create_tiled(tile_size, fixed_biome);
            auto reference = create_reference({}, fixed_biome);
            expect(tiled.has_value()) << (tiled ? "" : tiled.error().message);
            expect(reference.has_value())
                << (reference ? "" : reference.error().message);
            if (!tiled || !reference) {
                return;
            }
            auto chunks = (*tiled)->generate_tile(origin);
            expect(chunks.has_value())
                << (chunks ? "" : chunks.error().message);
            if (!chunks) {
                return;
            }
            constexpr chunk_pos probe{.x = 5, .z = 5};
            const proto_chunk *served = nullptr;
            for (const auto &chunk : *chunks) {
                if (chunk->chunk_x() == probe.x &&
                    chunk->chunk_z() == probe.z) {
                    served = chunk.get();
                }
            }
            expect(served != nullptr);
            if (served == nullptr) {
                return;
            }
            auto expected = (*reference)->generate(probe);
            expect(expected.has_value())
                << (expected ? "" : expected.error().message);
            if (!expected) {
                return;
            }
            const auto differences =
                block_difference_count(*served, **expected);
            std::fprintf(stderr, "tile-decoration probe: %zu block(s) differ\n",
                         differences);
            expect(differences == 0U)
                << "the probe chunk differs from the reference mode: "
                << differences;
        };

    "a three-by-three tile matches the reference mode chunk for chunk"_test =
        [] {
            if (!refrain::test_support::vanilla_data().available) {
                return;
            }
            const auto differences = compare_tile_with_reference(
                chunk_pos{.x = 3, .z = 3}, 3, fixed_biome);
            std::fprintf(stderr,
                         "tile-decoration 3x3 fixed biome: %zu block(s) "
                         "differ\n",
                         differences);
            expect(differences == 0U)
                << "a served tile chunk differs from the reference mode: "
                << differences;
        };

    "a tile computes each decorated chunk's own biome plan"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        // Without a fixed biome every decorated chunk builds its candidate
        // set from its own three-by-three palettes. The reference mode stages
        // the same rectangle, so an equal result means the plans agree too.
        const auto differences =
            compare_tile_with_reference(chunk_pos{.x = 3, .z = 3}, 3, {});
        std::fprintf(stderr,
                     "tile-decoration 3x3 natural biomes: %zu block(s) "
                     "differ\n",
                     differences);
        expect(differences == 0U)
            << "a served tile chunk differs from the reference mode with "
               "per-chunk plans: "
            << differences;
    };

    "a tile places structures like the reference mode"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        // An empty set list enables every shipped structure set, so the case
        // exercises the starts, references and piece placement of the tile
        // staging rather than only its feature decoration.
        const std::vector<std::string> every_set{};
        auto tiled = create_tiled(3, fixed_biome, every_set);
        expect(tiled.has_value()) << (tiled ? "" : tiled.error().message);
        if (!tiled) {
            return;
        }
        auto chunks = (*tiled)->generate_tile(chunk_pos{.x = 3, .z = 3});
        expect(chunks.has_value()) << (chunks ? "" : chunks.error().message);
        if (!chunks) {
            return;
        }
        std::size_t starts = 0;
        std::size_t references = 0;
        for (const auto &chunk : *chunks) {
            starts += chunk->structure_starts().size();
            references += chunk->structure_references().size();
        }
        std::fprintf(stderr,
                     "tile-decoration structures: starts=%zu references=%zu\n",
                     starts, references);
        // The tile may own no start while still placing pieces of starts that
        // reach it from a neighbouring chunk, which is what references record.
        expect(starts + references > 0U)
            << "the compared tile carries no structure start or reference";
        const auto differences = compare_tile_with_reference(
            chunk_pos{.x = 3, .z = 3}, 3, fixed_biome, every_set);
        std::fprintf(stderr, "tile-decoration 3x3 structures: %zu differ\n",
                     differences);
        expect(differences == 0U) << "structure blocks differ: " << differences;
    };

    "a tile batch serves exactly the requested rectangle"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        auto tiled = create_tiled(3, fixed_biome);
        expect(tiled.has_value()) << (tiled ? "" : tiled.error().message);
        if (!tiled) {
            return;
        }
        // A two-by-three rectangle from the corner of a three-chunk tile grid
        // spans two tiles; only the requested chunks come back, in x-major
        // order.
        constexpr chunk_pos origin{.x = 2, .z = 1};
        auto chunks = (*tiled)->generate_area_tiled(origin, 2, 3, 1);
        expect(chunks.has_value()) << (chunks ? "" : chunks.error().message);
        if (!chunks) {
            return;
        }
        expect(chunks->size() == 6U);
        if (chunks->size() != 6U) {
            return;
        }
        for (jint x = 0; x < 2; ++x) {
            for (jint z = 0; z < 3; ++z) {
                const auto index = static_cast<std::size_t>(x) * 3U +
                                   static_cast<std::size_t>(z);
                expect((*chunks)[index] != nullptr);
                if ((*chunks)[index] == nullptr) {
                    return;
                }
                expect((*chunks)[index]->chunk_x() == origin.x + x);
                expect((*chunks)[index]->chunk_z() == origin.z + z);
                expect((*chunks)[index]->status() == chunk_status::full);
            }
        }
    };
};
