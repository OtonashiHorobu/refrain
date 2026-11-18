// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The generation service's unit of work. The tile edge decides how long a
// joining client waits for its first chunks and how much fill and decoration
// work a served chunk costs. Two properties are pinned here: a tile stages its
// chunks independently of its edge (the cross-border ring is per tile), and the
// natural-biome content still changes with the edge, which is why the
// production default stays at eight instead of trading throughput for join
// latency. The timing and cost numbers are printed by the measurement case for
// a local report; they are not a pass/fail gate.
//
// The datapack snapshot is local and not committed, so every case skips with
// one diagnostic when the checkout has no copy.

#include <refrain/core/time.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/stage_profile.hpp>

#include "support/scratch_directory.hpp"
#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace world = refrain::world;
namespace worldgen = refrain::worldgen;
namespace sim = refrain::sim;
namespace test_support = refrain::test_support;

constexpr worldgen::jlong test_seed = 12345;
/// A structure set that does not exist turns structure starts off while
/// leaving the feature stage active.
constexpr std::string_view no_structure_set = "oracle:tile_no_structure_set";

[[nodiscard]] auto create_generator(
    worldgen::jint tile_size, std::string_view biome = {},
    worldgen::generation_target target = worldgen::generation_target::full)
    -> refrain::result<std::unique_ptr<worldgen::chunk_generator>> {
    return worldgen::chunk_generator::create(worldgen::chunk_generator_config{
        .data_root = test_support::vanilla_data_root(),
        .templates_root = "",
        .seed = test_seed,
        .kind = worldgen::dimension_kind::overworld,
        .fixed_biome = std::string{biome},
        .target = target,
        .structure_sets = {std::string{no_structure_set}},
        .tile_size = tile_size,
    });
}

/// Block-by-block difference count between two chunks of the same region.
[[nodiscard]] auto block_differences(const worldgen::proto_chunk &left,
                                     const worldgen::proto_chunk &right)
    -> std::size_t {
    if (left.min_y() != right.min_y() || left.height() != right.height()) {
        return 1;
    }
    std::size_t count{0};
    for (worldgen::jint y = left.min_y(); y < left.min_y() + left.height();
         ++y) {
        for (worldgen::jint z = 0; z < 16; ++z) {
            for (worldgen::jint x = 0; x < 16; ++x) {
                if (left.table().name(left.state_at(x, y, z)) !=
                    right.table().name(right.state_at(x, y, z))) {
                    ++count;
                }
            }
        }
    }
    return count;
}

/// The chunks of one generated tile, keyed by position.
[[nodiscard]] auto
by_position(std::vector<std::unique_ptr<worldgen::proto_chunk>> chunks)
    -> std::map<std::pair<worldgen::jint, worldgen::jint>,
                std::unique_ptr<worldgen::proto_chunk>> {
    std::map<std::pair<worldgen::jint, worldgen::jint>,
             std::unique_ptr<worldgen::proto_chunk>>
        out{};
    for (auto &chunk : chunks) {
        if (chunk != nullptr) {
            out[{chunk->chunk_x(), chunk->chunk_z()}] = std::move(chunk);
        }
    }
    return out;
}

/// Generate the tile that contains `pos` and compare that chunk between two
/// tile edges. Returns the block difference count.
[[nodiscard]] auto
compare_across_edges(worldgen::chunk_generator &left,
                     worldgen::chunk_generator &right, worldgen::chunk_pos pos,
                     worldgen::jint left_edge, worldgen::jint right_edge)
    -> std::size_t {
    const auto origin_of = [](worldgen::chunk_pos at, int edge) {
        const auto floor_div = [edge](int value) {
            return value >= 0 ? value / edge : (value - edge + 1) / edge;
        };
        return worldgen::chunk_pos{.x = floor_div(at.x) * edge,
                                   .z = floor_div(at.z) * edge};
    };
    auto left_tile{left.generate_tile(
        origin_of(worldgen::chunk_pos{.x = static_cast<int>(pos.x),
                                      .z = static_cast<int>(pos.z)},
                  static_cast<int>(left_edge)))};
    auto right_tile{right.generate_tile(
        origin_of(worldgen::chunk_pos{.x = static_cast<int>(pos.x),
                                      .z = static_cast<int>(pos.z)},
                  static_cast<int>(right_edge)))};
    expect(left_tile.has_value());
    expect(right_tile.has_value());
    if (!left_tile || !right_tile) {
        return 0;
    }
    const auto left_chunks = by_position(std::move(*left_tile));
    const auto right_chunks = by_position(std::move(*right_tile));
    const auto left_found{left_chunks.find({pos.x, pos.z})};
    const auto right_found{right_chunks.find({pos.x, pos.z})};
    expect(left_found != left_chunks.end());
    expect(right_found != right_chunks.end());
    if (left_found == left_chunks.end() || right_found == right_chunks.end()) {
        return 0;
    }
    return block_differences(*left_found->second, *right_found->second);
}

[[nodiscard]] auto measure_enabled() -> bool {
    const char *value = std::getenv("REFRAIN_TILE_MEASURE");
    return value != nullptr && *value != '\0' && *value != '0';
}

/// Generates the same rectangle through the tile path at one and eight
/// requested workers and returns the block differences, position by position.
[[nodiscard]] auto compare_worker_counts(worldgen::chunk_generator &one,
                                         worldgen::chunk_generator &many,
                                         worldgen::jint edge,
                                         worldgen::jint width,
                                         worldgen::jint depth) -> std::size_t {
    const auto origin = worldgen::chunk_pos{.x = 0, .z = 0};
    auto first{one.generate_area_tiled(origin, width, depth, 1)};
    auto second{many.generate_area_tiled(origin, width, depth, 8)};
    expect(first.has_value());
    expect(second.has_value());
    if (!first || !second) {
        return 0;
    }
    expect(first->size() == second->size());
    expect(first->size() ==
           static_cast<std::size_t>(width) * static_cast<std::size_t>(depth));
    static_cast<void>(edge);
    std::size_t differences{0};
    for (std::size_t index = 0; index < first->size(); ++index) {
        if ((*first)[index] == nullptr || (*second)[index] == nullptr) {
            ++differences;
            continue;
        }
        differences += block_differences(*(*first)[index], *(*second)[index]);
    }
    return differences;
}

} // namespace

suite<"server/chunk-generation"> chunk_generation_tests = [] {
    "server/chunk tile edge keeps terrain and fixed-biome content identical"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // A tile brings its own one-chunk ring, and every stage that
            // serves a chunk runs on the chunk's own position. The served
            // content therefore must not depend on the edge that staged it:
            // the terrain-only target and a fixed biome are the two cases
            // where decoration order cannot reach the result.
            for (const auto &pair : {std::pair{8, 4}, std::pair{8, 2}}) {
                for (const auto target : {worldgen::generation_target::terrain,
                                          worldgen::generation_target::full}) {
                    auto left{create_generator(pair.first, {}, target)};
                    auto right{create_generator(pair.second, {}, target)};
                    expect(left.has_value());
                    expect(right.has_value());
                    if (!left || !right) {
                        continue;
                    }
                    if (target == worldgen::generation_target::full) {
                        // Only a fixed biome makes the full target
                        // edge-independent; see the natural-biome case.
                        auto fixed_left{
                            create_generator(pair.first, "minecraft:plains")};
                        auto fixed_right{
                            create_generator(pair.second, "minecraft:plains")};
                        expect(fixed_left.has_value());
                        expect(fixed_right.has_value());
                        if (!fixed_left || !fixed_right) {
                            continue;
                        }
                        left = std::move(fixed_left);
                        right = std::move(fixed_right);
                    }
                    for (const auto chunk :
                         {worldgen::chunk_pos{.x = 3, .z = 3},
                          worldgen::chunk_pos{.x = 1, .z = 2},
                          worldgen::chunk_pos{.x = 0, .z = 0}}) {
                        const auto differences{compare_across_edges(
                            **left, **right, chunk, pair.first, pair.second)};
                        expect(differences == 0U)
                            << "tile-edge " << pair.first << "/" << pair.second
                            << " changed chunk (" << chunk.x << ", " << chunk.z
                            << ") by " << differences << " block(s)";
                    }
                }
            }
        };

    "server/chunk tile path is identical at one and eight workers"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        // The production path generates whole tiles. A tile never shares state
        // with another tile, so the worker that happens to run one cannot
        // change the chunks it serves. This is the worker-count independence
        // the experimental per-chunk path does not have yet.
        auto serial{create_generator(2)};
        auto parallel{create_generator(2)};
        expect(serial.has_value());
        expect(parallel.has_value());
        if (!serial || !parallel) {
            return;
        }
        const auto differences{
            compare_worker_counts(**serial, **parallel, 2, 4, 4)};
        expect(differences == 0U) << "the tile path changed " << differences
                                  << " block(s) between one and eight workers";
    };

    "server/chunk tile edge changes natural-biome decoration at a tile corner"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // With the dimension's own biomes the decorated rectangle differs
            // between edges: a ring chunk's feature placement can read the
            // terrain of the ring outside the tile, and an undecorated chunk
            // there is read as terrain only. The corner chunk of the smaller
            // tile is where that difference reaches a served chunk, so its
            // content is not edge neutral. That is why the production default
            // edge stays eight; making a smaller edge safe needs the generator
            // to decorate a ring that follows the served square, not the tile.
            auto large{create_generator(8)};
            auto half{create_generator(4)};
            expect(large.has_value());
            expect(half.has_value());
            if (!large || !half) {
                return;
            }
            const auto differences{compare_across_edges(
                **large, **half, worldgen::chunk_pos{.x = 3, .z = 3}, 8, 4)};
            std::fprintf(stderr,
                         "tile-edge natural biome corner: %zu block(s) "
                         "differ between edge 8 and 4\n",
                         differences);
            expect(differences > 0U)
                << "the natural-biome corner content became edge neutral; "
                   "re-measure the production tile edge before changing it";
        };
};

suite<"server/chunk-generation"> chunk_generation_diagnostics = [] {
    "server/chunk tile edge content diagnostic"_test = [] {
        if (!measure_enabled() || !test_support::vanilla_data().available) {
            return;
        }
        // Prints which stage changes with the edge, for a local report.
        const auto compare_diagnostic{[](worldgen::chunk_generator &left,
                                         worldgen::chunk_generator &right,
                                         worldgen::chunk_pos pos, int left_edge,
                                         int right_edge) {
            const auto differences{compare_across_edges(
                left, right, pos, static_cast<worldgen::jint>(left_edge),
                static_cast<worldgen::jint>(right_edge))};
            std::fprintf(stderr,
                         "tile-edge diag chunk (%d,%d) %d vs %d: %zu "
                         "block(s) differ\n",
                         static_cast<int>(pos.x), static_cast<int>(pos.z),
                         left_edge, right_edge, differences);
        }};
        for (const auto target : {worldgen::generation_target::terrain,
                                  worldgen::generation_target::full}) {
            for (const auto &pair : {std::pair{8, 4}, std::pair{8, 2}}) {
                auto left{create_generator(pair.first, {}, target)};
                auto right{create_generator(pair.second, {}, target)};
                expect(left.has_value());
                expect(right.has_value());
                if (!left || !right) {
                    continue;
                }
                std::fprintf(stderr, "tile-edge diag stage=%d edge %d/%d\n",
                             static_cast<int>(target), pair.first, pair.second);
                for (const auto chunk : {worldgen::chunk_pos{.x = 3, .z = 3},
                                         worldgen::chunk_pos{.x = 1, .z = 2},
                                         worldgen::chunk_pos{.x = 0, .z = 0}}) {
                    compare_diagnostic(**left, **right, chunk, pair.first,
                                       pair.second);
                }
            }
        }
        for (const auto &pair : {std::pair{8, 4}, std::pair{8, 2}}) {
            auto left{create_generator(pair.first, "minecraft:plains")};
            auto right{create_generator(pair.second, "minecraft:plains")};
            expect(left.has_value());
            expect(right.has_value());
            if (!left || !right) {
                continue;
            }
            std::fprintf(stderr, "tile-edge diag fixed biome edge %d/%d\n",
                         pair.first, pair.second);
            compare_diagnostic(**left, **right,
                               worldgen::chunk_pos{.x = 3, .z = 3}, pair.first,
                               pair.second);
        }
        for (const auto size : {8, 4, 2}) {
            auto generator{create_generator(size)};
            expect(generator.has_value());
            if (!generator) {
                continue;
            }
            auto tile{(*generator)
                          ->generate_tile(worldgen::chunk_pos{.x = 0, .z = 0})};
            expect(tile.has_value());
            if (!tile) {
                continue;
            }
            std::size_t starts{0};
            for (const auto &chunk : *tile) {
                starts += chunk->structure_starts().size();
            }
            std::fprintf(stderr, "tile-edge diag edge %d starts=%zu\n", size,
                         starts);
        }
    };
};

suite<"server/chunk-generation"> chunk_generation_measurement = [] {
    "server/chunk tile edge measurement"_test = [] {
        if (!measure_enabled() || !test_support::vanilla_data().available) {
            return;
        }
        // Wall-clock numbers from this host, printed for the report. They are
        // a sanity check on the tile-edge trade-off, not a benchmark: another
        // lane compiling in parallel moves them further than most changes do.
        constexpr worldgen::jint sizes[]{8, 4, 2};
        constexpr worldgen::chunk_pos origins[]{
            {.x = 0, .z = 0}, {.x = 8, .z = 0}, {.x = 16, .z = 0}};
        for (const auto size : sizes) {
            auto generator{create_generator(size)};
            expect(generator.has_value());
            if (!generator) {
                continue;
            }
            worldgen::stage_profile profile{};
            (*generator)->set_stage_profile(&profile);
            double total_ms{0.0};
            std::size_t served{0};
            for (const auto &origin : origins) {
                const auto start{refrain::core::now_ns()};
                auto chunks{(*generator)->generate_tile(origin)};
                const auto elapsed{
                    static_cast<double>(refrain::core::now_ns() - start) /
                    1'000'000.0};
                expect(chunks.has_value());
                if (!chunks) {
                    continue;
                }
                total_ms += elapsed;
                served += chunks->size();
                std::fprintf(stderr,
                             "tile-edge %dx%d tile at (%d,%d): %.1fms for %zu "
                             "columns\n",
                             static_cast<int>(size), static_cast<int>(size),
                             static_cast<int>(origin.x),
                             static_cast<int>(origin.z), elapsed,
                             chunks->size());
            }
            const auto per_served{
                served == 0 ? 0.0 : total_ms / static_cast<double>(served)};
            std::fprintf(
                stderr,
                "tile-edge %dx%d: %.2fms per served chunk, %.1fms "
                "per tile, %zu columns total\n",
                static_cast<int>(size), static_cast<int>(size), per_served,
                served == 0 ? 0.0
                            : total_ms / (static_cast<double>(served) /
                                          (static_cast<double>(size) * size)),
                served);
            for (const auto &stage : profile.stages()) {
                const auto per_chunk{
                    served == 0
                        ? 0.0
                        : static_cast<double>(stage.nanoseconds) / 1'000'000.0 /
                              static_cast<double>(served)};
                std::fprintf(stderr, "tile-edge stage %s: %.3fms/chunk\n",
                             stage.name.c_str(), per_chunk);
            }
        }
        // Adoption cost: one column's decode, entity spawn and initial light.
        // The adoption budget is chosen from this.
        test_support::scratch_directory scratch;
        const auto path{(scratch.path() / "world").string()};
        expect(
            static_cast<bool>(world::create_world(path, test_seed, "probe")));
        auto runtime{sim::world::open(path)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto generator{create_generator(8)};
        expect(generator.has_value());
        if (!generator) {
            return;
        }
        auto chunks{
            (*generator)->generate_tile(worldgen::chunk_pos{.x = 0, .z = 0})};
        expect(chunks.has_value());
        if (!chunks) {
            return;
        }
        std::vector<double> costs{};
        costs.reserve(chunks->size());
        for (auto &proto : *chunks) {
            auto column{
                world::from_proto_chunk(*proto, (*generator)->biome_names())};
            expect(column.has_value());
            if (!column) {
                continue;
            }
            const auto pos{sim::chunk_pos{proto->chunk_x(), proto->chunk_z()}};
            const auto start{refrain::core::now_ns()};
            auto inserted{runtime->insert_chunk(pos, std::move(*column))};
            const auto elapsed{
                static_cast<double>(refrain::core::now_ns() - start) /
                1'000'000.0};
            expect(inserted.has_value());
            if (inserted) {
                costs.push_back(elapsed);
            }
        }
        std::sort(costs.begin(), costs.end());
        if (!costs.empty()) {
            const auto p99{costs[static_cast<std::size_t>(
                static_cast<double>(costs.size() - 1) * 0.99)]};
            double total{0.0};
            for (const auto cost : costs) {
                total += cost;
            }
            std::fprintf(stderr,
                         "tile-edge adoption: %zu columns, mean %.3fms, p99 "
                         "%.3fms, total %.1fms\n",
                         costs.size(),
                         total / static_cast<double>(costs.size()), p99, total);
        }
    };
};

suite<"server/chunk-generation"> chunk_generation_gating_tests = [] {
    "server/chunk generation defaults to the production tile path"_test = [] {
        const refrain::server::chunk_generation::config settings{};
        expect(!settings.experimental_chunk_pipeline)
            << "the experimental per-chunk pipeline must stay off by default";
        expect(settings.tile_size == 8);
    };

    "server/chunk generation selects the tile path unless the flag is set"_test =
        [] {
            // The runtime composition never sets the flag; starting a default
            // configuration must therefore run the production tile path.
            auto started{refrain::server::chunk_generation::start(
                refrain::server::chunk_generation::config{
                    .data_root = "/refrain/gating-probe",
                    .worker_count = 1,
                })};
            expect(started.has_value());
            if (!started) {
                return;
            }
            expect(!(*started)->uses_chunk_pipeline());
            expect((*started)->tile_size() == 8);
            (*started)->stop();
        };

    "server/chunk generation runs the scheduler only when opted in"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        auto started{refrain::server::chunk_generation::start(
            refrain::server::chunk_generation::config{
                .data_root = test_support::vanilla_data_root(),
                .worker_count = 1,
                .experimental_chunk_pipeline = true,
            })};
        expect(started.has_value()) << (started ? "" : started.error().message);
        if (!started) {
            return;
        }
        expect((*started)->uses_chunk_pipeline());
        expect((*started)->tile_size() == 1)
            << "a scheduler task covers one column, not a tile";
        (*started)->stop();
    };
};
