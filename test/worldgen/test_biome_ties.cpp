// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The climate nearest-point search resolves equal-distance parameter points in
// traversal order: the warm start is the previous selection of the same
// generator, so a tie can select a different leaf than a search started from
// the root. These cases pin two chunks where the stored biome and the
// root-start biome differ, pin the per-chunk warm-start reset at a chunk
// boundary, and check that a batch produces the same biome-inclusive digest on
// one and eight workers.

#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/dimension_generator.hpp>

#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

auto hash_bytes(std::uint64_t hash, std::string_view text) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= prime;
    }
    return hash;
}

auto hash_integer(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= prime;
    }
    return hash;
}

/// Hashes the block states and the 4x4x4 biome cells of one chunk. Biome ids
/// are hashed by name because each generator instance owns its own registry.
auto chunk_digest(const proto_chunk &chunk,
                  const std::vector<std::string> &biome_names)
    -> std::uint64_t {
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.chunk_x()));
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.chunk_z()));
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.status()));
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                hash = hash_bytes(hash,
                                  chunk.table().name(chunk.state_at(x, y, z)));
            }
        }
    }
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); y += 4) {
        for (jint z = 0; z < 16; z += 4) {
            for (jint x = 0; x < 16; x += 4) {
                const auto id = chunk.biome_at(x, y, z);
                if (static_cast<std::size_t>(id) < biome_names.size()) {
                    hash = hash_bytes(hash, biome_names[id]);
                } else {
                    hash = hash_integer(hash, static_cast<std::uint64_t>(id));
                }
            }
        }
    }
    return hash;
}

auto batch_digests(const std::vector<std::unique_ptr<proto_chunk>> &chunks,
                   const std::vector<std::string> &biome_names)
    -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> digests;
    digests.reserve(chunks.size());
    for (const auto &chunk : chunks) {
        digests.push_back(chunk_digest(*chunk, biome_names));
    }
    return digests;
}

struct tie_case {
    dimension_kind kind{dimension_kind::overworld};
    jlong seed{0};
    chunk_pos chunk{};
    jint quart_x{0};
    jint quart_y{0};
    jint quart_z{0};
    std::string_view stored{};
    std::string_view root{};
};

void check_tie(const tie_case &value) {
    const auto snapshot = refrain::test_support::vanilla_data();
    if (!snapshot.available) {
        return;
    }
    auto generated = chunk_generator::create(chunk_generator_config{
        .data_root = snapshot.path,
        .templates_root = {},
        .seed = value.seed,
        .kind = value.kind,
        .fixed_biome = {},
        .target = generation_target::terrain,
    });
    expect(generated.has_value()) << generated.error().message;
    if (!generated) {
        return;
    }
    auto &generator = **generated;
    const auto chunk = generator.generate(value.chunk);
    expect(chunk.has_value()) << chunk.error().message;
    if (!chunk) {
        return;
    }

    // The searched target was selected during the biome stage of the chunk.
    // Read the cell the chunk stored instead of querying it again, so the
    // expectation is about the recorded selection.
    const jint block_x = (value.quart_x - value.chunk.x * 4) * 4;
    const jint block_y = value.quart_y * 4;
    const jint block_z = (value.quart_z - value.chunk.z * 4) * 4;
    const auto stored = generator.biome_names()[static_cast<std::size_t>(
        (*chunk)->biome_at(block_x, block_y, block_z))];
    expect(stored == value.stored) << std::string{"stored biome is "} + stored +
                                          ", expected " +
                                          std::string{value.stored};

    // The same target from a fresh root: no warm start, so the root order
    // decides the tie instead of the previous selection.
    auto dimension = dimension_generator::create(dimension_generator_config{
        .data_root = snapshot.path,
        .kind = value.kind,
        .seed = value.seed,
    });
    expect(dimension.has_value()) << dimension.error().message;
    if (!dimension) {
        return;
    }
    auto &source = **dimension;
    source.reset_biome_search();
    const auto root = source.biome_name(
        source.biome_at_quart(value.quart_x, value.quart_y, value.quart_z));
    expect(root == value.root) << std::string{"root-start biome is "} +
                                      std::string{root} + ", expected " +
                                      std::string{value.root};
    expect(stored != root) << "the two equidistant leaves must name different "
                              "biomes";
}

auto batch_digest(dimension_kind kind, jlong seed, std::size_t workers,
                  std::span<const chunk_pos> positions)
    -> std::optional<std::vector<std::uint64_t>> {
    const auto snapshot = refrain::test_support::vanilla_data();
    if (!snapshot.available) {
        return std::nullopt;
    }
    auto created = chunk_generator::create(chunk_generator_config{
        .data_root = snapshot.path,
        .templates_root = {},
        .seed = seed,
        .kind = kind,
        .fixed_biome = {},
        .target = generation_target::terrain,
    });
    expect(created.has_value()) << created.error().message;
    if (!created) {
        return std::nullopt;
    }
    const auto chunks = (*created)->generate_batch(positions, workers);
    expect(chunks.has_value()) << chunks.error().message;
    if (!chunks) {
        return std::nullopt;
    }
    return batch_digests(*chunks, (*created)->biome_names());
}

/// Sixteen positions, more than the eight workers, so workers generate more
/// than one chunk and a missing per-chunk reset can carry a warm start between
/// them. The first two positions are a boundary whose next chunk selects a
/// different first biome when the previous chunk's warm start survives, so a
/// missing reset changes the one-worker batch while the eight-worker batch
/// still starts that chunk fresh.
void check_worker_independence(dimension_kind kind, jlong seed,
                               std::span<const chunk_pos> positions) {
    const auto snapshot = refrain::test_support::vanilla_data();
    if (!snapshot.available) {
        return;
    }
    const auto serial = batch_digest(kind, seed, 1, positions);
    const auto parallel = batch_digest(kind, seed, 8, positions);
    expect(serial.has_value());
    expect(parallel.has_value());
    if (!serial || !parallel) {
        return;
    }
    expect(serial->size() == parallel->size());
    const auto shared = std::min(serial->size(), parallel->size());
    for (std::size_t index = 0; index < shared; ++index) {
        expect((*serial)[index] == (*parallel)[index])
            << "chunk " + std::to_string(index) +
                   " differs between one and eight workers";
    }
}

} // namespace

suite<"worldgen/biome-ties"> biome_tie_tests = [] {
    "the overworld beach and stony shore tie keeps the warmed selection"_test =
        [] {
            check_tie(tie_case{.kind = dimension_kind::overworld,
                               .seed = 1,
                               .chunk = chunk_pos{.x = 5, .z = 10},
                               .quart_x = 21,
                               .quart_y = 2,
                               .quart_z = 40,
                               .stored = "minecraft:beach",
                               .root = "minecraft:stony_shore"});
        };

    "the nether crimson forest and nether wastes tie keeps the warmed "
    "selection"_test = [] {
        check_tie(tie_case{.kind = dimension_kind::nether,
                           .seed = 12345,
                           .chunk = chunk_pos{.x = 0, .z = 7},
                           .quart_x = 0,
                           .quart_y = 0,
                           .quart_z = 30,
                           .stored = "minecraft:crimson_forest",
                           .root = "minecraft:nether_wastes"});
    };

    "a chunk starts from a fresh biome search after the previous chunk"_test =
        [] {
            const auto snapshot = refrain::test_support::vanilla_data();
            if (!snapshot.available) {
                return;
            }
            auto created = chunk_generator::create(chunk_generator_config{
                .data_root = snapshot.path,
                .templates_root = {},
                .seed = 12345,
                .kind = dimension_kind::nether,
                .fixed_biome = {},
                .target = generation_target::terrain,
            });
            expect(created.has_value()) << created.error().message;
            if (!created) {
                return;
            }
            const auto previous =
                (*created)->generate(chunk_pos{.x = 77589, .z = 0});
            expect(previous.has_value()) << previous.error().message;
            if (!previous) {
                return;
            }
            const auto next =
                (*created)->generate(chunk_pos{.x = 77590, .z = 0});
            expect(next.has_value()) << next.error().message;
            if (!next) {
                return;
            }
            // The first quart cell would select soul_sand_valley if the
            // previous chunk's last selection stayed the warm start.
            const auto first =
                (*created)->biome_names()[static_cast<std::size_t>(
                    (*next)->biome_at(0, 0, 0))];
            expect(first == "minecraft:nether_wastes")
                << "the first quart cell inherited the previous chunk's warm "
                   "start";
        };

    "the biome-inclusive chunk digest is identical at one and eight workers"_test =
        [] {
            constexpr std::array<chunk_pos, 16> overworld_positions{{
                {.x = 0, .z = 0},
                {.x = 0, .z = 1},
                {.x = 0, .z = 2},
                {.x = 0, .z = 3},
                {.x = 1, .z = 0},
                {.x = 1, .z = 1},
                {.x = 1, .z = 2},
                {.x = 1, .z = 3},
                {.x = 2, .z = 0},
                {.x = 2, .z = 1},
                {.x = 2, .z = 2},
                {.x = 2, .z = 3},
                {.x = 3, .z = 0},
                {.x = 3, .z = 1},
                {.x = 3, .z = 2},
                {.x = 3, .z = 3},
            }};
            constexpr std::array<chunk_pos, 16> nether_positions{{
                {.x = 77589, .z = 0},
                {.x = 77590, .z = 0},
                {.x = 77588, .z = 0},
                {.x = 77591, .z = 0},
                {.x = 77588, .z = 1},
                {.x = 77589, .z = 1},
                {.x = 77590, .z = 1},
                {.x = 77591, .z = 1},
                {.x = 77588, .z = 2},
                {.x = 77589, .z = 2},
                {.x = 77590, .z = 2},
                {.x = 77591, .z = 2},
                {.x = 77588, .z = 3},
                {.x = 77589, .z = 3},
                {.x = 77590, .z = 3},
                {.x = 77591, .z = 3},
            }};
            check_worker_independence(dimension_kind::overworld, 1,
                                      overworld_positions);
            check_worker_independence(dimension_kind::nether, 12345,
                                      nether_positions);
        };
};
