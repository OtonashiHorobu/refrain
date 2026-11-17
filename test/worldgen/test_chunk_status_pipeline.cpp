// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The per-chunk status pipeline: one chunk is materialised through a reusable
// proto cache, its neighbours decorate around it in ascending chunk order, and
// the served copy must contain the same blocks as the reference single-chunk
// mode that stages the same neighbourhood one chunk at a time. The cache keeps
// the cross-border writes a later neighbour reads, and the served digest does
// not depend on how often the chunk is requested or on the configured tile
// edge, which the pipeline does not use.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_cache.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace refrain;
using namespace refrain::worldgen;

constexpr jlong test_seed = 12345;
constexpr std::string_view fixed_biome = "minecraft:plains";
/// A structure set that does not exist keeps the structure starts off while
/// leaving the feature stage active.
constexpr std::string_view no_structure_set =
    "oracle:pipeline_no_structure_set";

auto source_data_root() -> std::string {
    return refrain::test_support::vanilla_data_root();
}

/// `optional` absent keeps starts off with a set that does not exist; an empty
/// list enables every shipped set.
using structure_sets = std::optional<std::vector<std::string>>;

auto selected_sets(const structure_sets &sets) -> std::vector<std::string> {
    if (sets.has_value()) {
        return *sets;
    }
    return {std::string{no_structure_set}};
}

auto create_pipeline(jint tile_size, std::string_view biome,
                     const structure_sets &sets = std::nullopt,
                     generation_target target = generation_target::full)
    -> result<std::unique_ptr<chunk_generator>> {
    return chunk_generator::create(chunk_generator_config{
        .data_root = source_data_root(),
        .templates_root = "",
        .seed = test_seed,
        .kind = dimension_kind::overworld,
        .fixed_biome = std::string{biome},
        .target = target,
        .structure_sets = selected_sets(sets),
        .tile_size = tile_size,
    });
}

/// The reference single-chunk mode: decorate the target and its eight
/// neighbours in force-load order through one region. With a `box` it decorates
/// exactly the chunks of that rectangle instead.
auto create_reference(chunk_rectangle box, std::string_view biome,
                      const structure_sets &sets = std::nullopt,
                      generation_target target = generation_target::full)
    -> result<std::unique_ptr<chunk_generator>> {
    return chunk_generator::create(chunk_generator_config{
        .data_root = source_data_root(),
        .templates_root = "",
        .seed = test_seed,
        .kind = dimension_kind::overworld,
        .fixed_biome = std::string{biome},
        .target = target,
        .structure_sets = selected_sets(sets),
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

auto heightmap_difference_count(const proto_chunk &left,
                                const proto_chunk &right) -> std::size_t {
    std::size_t count = 0;
    for (const auto kind :
         {heightmap_kind::world_surface, heightmap_kind::ocean_floor,
          heightmap_kind::motion_blocking,
          heightmap_kind::motion_blocking_no_leaves}) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                if (left.first_available(kind, x, z) !=
                    right.first_available(kind, x, z)) {
                    ++count;
                }
            }
        }
    }
    return count;
}

auto structure_record_difference(const proto_chunk &left,
                                 const proto_chunk &right) -> bool {
    const auto &left_starts = left.structure_starts();
    const auto &right_starts = right.structure_starts();
    if (left_starts.size() != right_starts.size()) {
        return true;
    }
    for (std::size_t index = 0; index < left_starts.size(); ++index) {
        const auto &a = left_starts[index];
        const auto &b = right_starts[index];
        if (a.structure != b.structure || a.step != b.step ||
            a.chunk_x != b.chunk_x || a.chunk_z != b.chunk_z ||
            a.min_x != b.min_x || a.min_y != b.min_y || a.min_z != b.min_z ||
            a.max_x != b.max_x || a.max_y != b.max_y || a.max_z != b.max_z ||
            a.pieces.size() != b.pieces.size()) {
            return true;
        }
    }
    const auto &left_references = left.structure_references();
    const auto &right_references = right.structure_references();
    if (left_references.size() != right_references.size()) {
        return true;
    }
    for (std::size_t index = 0; index < left_references.size(); ++index) {
        const auto &a = left_references[index];
        const auto &b = right_references[index];
        if (a.structure != b.structure || a.chunk_x != b.chunk_x ||
            a.chunk_z != b.chunk_z || a.count != b.count) {
            return true;
        }
    }
    return false;
}

/// Hashes block states, biome cells and the four final heightmaps of one chunk.
auto chunk_digest(const proto_chunk &chunk,
                  const std::vector<std::string> &biome_names)
    -> std::uint64_t {
    const auto mix = [](std::uint64_t hash, std::uint64_t value) {
        constexpr std::uint64_t multiplier = 1099511628211ULL;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xFFU;
            hash *= multiplier;
        }
        return hash;
    };
    const auto mix_bytes = [](std::uint64_t hash, std::string_view text) {
        constexpr std::uint64_t multiplier = 1099511628211ULL;
        for (const char character : text) {
            hash ^= static_cast<unsigned char>(character);
            hash *= multiplier;
        }
        return hash;
    };
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    hash = mix(hash, static_cast<std::uint64_t>(chunk.chunk_x()));
    hash = mix(hash, static_cast<std::uint64_t>(chunk.chunk_z()));
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                hash = mix_bytes(hash,
                                 chunk.table().name(chunk.state_at(x, y, z)));
            }
        }
    }
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); y += 4) {
        for (jint z = 0; z < 16; z += 4) {
            for (jint x = 0; x < 16; x += 4) {
                const auto id = chunk.biome_at(x, y, z);
                if (static_cast<std::size_t>(id) < biome_names.size()) {
                    hash = mix_bytes(hash, biome_names[id]);
                } else {
                    hash = mix(hash, id);
                }
            }
        }
    }
    for (const auto kind :
         {heightmap_kind::world_surface, heightmap_kind::ocean_floor,
          heightmap_kind::motion_blocking,
          heightmap_kind::motion_blocking_no_leaves}) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                hash = mix(hash, static_cast<std::uint64_t>(
                                     chunk.first_available(kind, x, z)));
            }
        }
    }
    return hash;
}

// --- unit-test table for the cache and the region -------------------------

constexpr block_state_id air_block = 0;
constexpr block_state_id stone_block = 1;

class test_table final : public block_state_table {
  public:
    test_table() {
        names_ = {"minecraft:air", "minecraft:stone"};
        traits_ = {{
            {.air = true},
            {.air = false,
             .blocks_motion = true,
             .blocks_motion_no_leaves = true},
        }};
    }

    [[nodiscard]] auto air() const -> block_state_id override {
        return air_block;
    }
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
    std::array<state_traits, 2> traits_{};
};

auto make_chunk(const test_table &table, jint x, jint z)
    -> std::unique_ptr<proto_chunk> {
    return std::make_unique<proto_chunk>(table, x, z, -64, 384);
}

} // namespace

using namespace boost::ut;

/// Compares one pipeline chunk with a reference chunk and reports the first
/// difference class. The status is part of the comparison.
void expect_same_chunk(const proto_chunk &pipeline,
                       const proto_chunk &reference, std::string_view label) {
    expect(pipeline.status() == reference.status())
        << std::string{label} + ": status differs";
    const auto blocks = block_difference_count(pipeline, reference);
    if (blocks != 0U) {
        std::fprintf(stderr, "%s: %zu block(s) differ\n",
                     std::string{label}.c_str(), blocks);
    }
    expect(blocks == 0U) << std::string{label} + ": blocks differ";
    const auto heights = heightmap_difference_count(pipeline, reference);
    expect(heights == 0U) << std::string{label} + ": heightmaps differ";
    expect(!structure_record_difference(pipeline, reference))
        << std::string{label} + ": structure records differ";
}

suite<"worldgen/chunk-status-pipeline"> chunk_status_pipeline_tests = [] {
    "a pipeline chunk reproduces the reference single-chunk content"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        auto pipeline = create_pipeline(8, fixed_biome);
        auto reference = create_reference({}, fixed_biome);
        expect(pipeline.has_value())
            << (pipeline ? "" : pipeline.error().message);
        expect(reference.has_value())
            << (reference ? "" : reference.error().message);
        if (!pipeline || !reference) {
            return;
        }
        // The probe chunk is the border-decoration regression chunk: the
        // target-only production mode differs from the reference mode there.
        constexpr chunk_pos probe{.x = 5, .z = 5};
        auto served = (*pipeline)->generate_chunk(probe);
        expect(served.has_value()) << (served ? "" : served.error().message);
        if (!served) {
            return;
        }
        expect((*served)->status() == chunk_status::full);
        auto expected = (*reference)->generate(probe);
        expect(expected.has_value())
            << (expected ? "" : expected.error().message);
        if (!expected) {
            return;
        }
        expect_same_chunk(**served, **expected, "fixed biome");
    };

    "the pipeline matches the reference with the dimension biome source"_test =
        [] {
            if (!refrain::test_support::vanilla_data().available) {
                return;
            }
            auto pipeline = create_pipeline(8, {});
            auto reference = create_reference({}, {});
            expect(pipeline.has_value())
                << (pipeline ? "" : pipeline.error().message);
            expect(reference.has_value())
                << (reference ? "" : reference.error().message);
            if (!pipeline || !reference) {
                return;
            }
            constexpr chunk_pos probe{.x = 4, .z = 4};
            auto served = (*pipeline)->generate_chunk(probe);
            expect(served.has_value())
                << (served ? "" : served.error().message);
            if (!served) {
                return;
            }
            auto expected = (*reference)->generate(probe);
            expect(expected.has_value())
                << (expected ? "" : expected.error().message);
            if (!expected) {
                return;
            }
            expect_same_chunk(**served, **expected, "natural biomes");
        };

    "the pipeline places structures like the reference mode"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        const std::vector<std::string> every_set{};
        auto pipeline = create_pipeline(8, fixed_biome, every_set);
        auto reference = create_reference({}, fixed_biome, every_set);
        expect(pipeline.has_value())
            << (pipeline ? "" : pipeline.error().message);
        expect(reference.has_value())
            << (reference ? "" : reference.error().message);
        if (!pipeline || !reference) {
            return;
        }
        // At this seed most chunks carry no structure, so the case walks a
        // few probe chunks and compares the first one that owns a start or
        // references one. The cache is cleared between probes so each chunk
        // sees the same fresh neighbourhood the reference mode stages.
        std::size_t records = 0;
        std::size_t compared = 0;
        for (const chunk_pos probe :
             {chunk_pos{.x = 3, .z = 3}, chunk_pos{.x = 4, .z = 4},
              chunk_pos{.x = 5, .z = 5}, chunk_pos{.x = 0, .z = 0},
              chunk_pos{.x = 1, .z = 1}}) {
            (*pipeline)->clear_proto_cache();
            auto served = (*pipeline)->generate_chunk(probe);
            expect(served.has_value())
                << (served ? "" : served.error().message);
            if (!served) {
                return;
            }
            auto expected = (*reference)->generate(probe);
            expect(expected.has_value())
                << (expected ? "" : expected.error().message);
            if (!expected) {
                return;
            }
            ++compared;
            expect_same_chunk(**served, **expected, "structures");
            records = (*served)->structure_starts().size() +
                      (*served)->structure_references().size();
            if (records != 0U) {
                std::fprintf(stderr,
                             "pipeline structures: chunk (%d,%d) starts=%zu "
                             "references=%zu\n",
                             static_cast<int>(probe.x),
                             static_cast<int>(probe.z),
                             (*served)->structure_starts().size(),
                             (*served)->structure_references().size());
                break;
            }
        }
        std::fprintf(stderr,
                     "pipeline structures: compared %zu chunk(s), last "
                     "records=%zu\n",
                     compared, records);
        expect(records > 0U)
            << "no probe chunk carried a structure start or reference";
    };

    "a shared cache keeps the neighbour writes the later chunk reads"_test =
        [] {
            if (!refrain::test_support::vanilla_data().available) {
                return;
            }
            auto pipeline = create_pipeline(8, fixed_biome);
            expect(pipeline.has_value())
                << (pipeline ? "" : pipeline.error().message);
            if (!pipeline) {
                return;
            }
            // Serve (0,0) and then (0,1). The second request finds the first
            // chunk's decoration already in the cache and extends the decorated
            // rectangle by one row, so the union is x -1..1, z -1..2.
            auto first = (*pipeline)->generate_chunk(chunk_pos{.x = 0, .z = 0});
            expect(first.has_value()) << (first ? "" : first.error().message);
            if (!first) {
                return;
            }
            auto second =
                (*pipeline)->generate_chunk(chunk_pos{.x = 0, .z = 1});
            expect(second.has_value())
                << (second ? "" : second.error().message);
            if (!second) {
                return;
            }
            const chunk_rectangle union_box{
                .min_x = -1, .min_z = -1, .max_x = 1, .max_z = 2};
            auto reference = create_reference(union_box, fixed_biome);
            expect(reference.has_value())
                << (reference ? "" : reference.error().message);
            if (!reference) {
                return;
            }
            auto expected = (*reference)->generate(chunk_pos{.x = 0, .z = 1});
            expect(expected.has_value())
                << (expected ? "" : expected.error().message);
            if (!expected) {
                return;
            }
            expect_same_chunk(**second, **expected, "shared-cache neighbour");
            // The neighbour must actually contain writes that the target-only
            // mode does not make: (0,0)'s features cross the border into (0,1).
            auto target_only = chunk_generator::create(chunk_generator_config{
                .data_root = source_data_root(),
                .templates_root = "",
                .seed = test_seed,
                .kind = dimension_kind::overworld,
                .fixed_biome = std::string{fixed_biome},
                .target = generation_target::full,
                .structure_sets = selected_sets(std::nullopt),
            });
            expect(target_only.has_value())
                << (target_only ? "" : target_only.error().message);
            if (!target_only) {
                return;
            }
            auto baseline = (*target_only)->generate(chunk_pos{.x = 0, .z = 1});
            expect(baseline.has_value())
                << (baseline ? "" : baseline.error().message);
            if (!baseline) {
                return;
            }
            const auto differences =
                block_difference_count(**second, **baseline);
            std::fprintf(stderr,
                         "pipeline shared cache: %zu block(s) carry neighbour "
                         "writes\n",
                         differences);
            expect(differences > 0U)
                << "the neighbour's decoration did not reach the cached chunk";
        };

    "a repeated request hits the cache without changing the chunk"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        auto pipeline = create_pipeline(8, fixed_biome);
        expect(pipeline.has_value())
            << (pipeline ? "" : pipeline.error().message);
        if (!pipeline) {
            return;
        }
        auto &generator = **pipeline;
        constexpr chunk_pos probe{.x = 0, .z = 0};
        auto first = generator.generate_chunk(probe);
        expect(first.has_value()) << (first ? "" : first.error().message);
        if (!first) {
            return;
        }
        const auto first_stats = generator.cache_stats();
        const auto first_digest =
            chunk_digest(**first, generator.biome_names());
        auto second = generator.generate_chunk(probe);
        expect(second.has_value()) << (second ? "" : second.error().message);
        if (!second) {
            return;
        }
        const auto second_stats = generator.cache_stats();
        expect(chunk_digest(**second, generator.biome_names()) == first_digest);
        expect(second_stats.entries == first_stats.entries)
            << "a repeated request added cache entries";
        expect(second_stats.hits > first_stats.hits)
            << "a repeated request did not hit the cache";
    };

    "the pipeline area is identical at one and eight workers"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        auto serial = create_pipeline(8, fixed_biome);
        auto parallel = create_pipeline(8, fixed_biome);
        expect(serial.has_value()) << (serial ? "" : serial.error().message);
        expect(parallel.has_value())
            << (parallel ? "" : parallel.error().message);
        if (!serial || !parallel) {
            return;
        }
        constexpr chunk_pos origin{.x = 0, .z = 0};
        const auto one = (*serial)->generate_chunk_area(origin, 2, 2, 1);
        expect(one.has_value()) << (one ? "" : one.error().message);
        if (!one) {
            return;
        }
        const auto eight = (*parallel)->generate_chunk_area(origin, 2, 2, 8);
        expect(eight.has_value()) << (eight ? "" : eight.error().message);
        if (!eight) {
            return;
        }
        expect(one->size() == eight->size());
        const auto names = (*serial)->biome_names();
        for (std::size_t index = 0; index < one->size(); ++index) {
            expect((*one)[index] != nullptr);
            expect((*eight)[index] != nullptr);
            if ((*one)[index] == nullptr || (*eight)[index] == nullptr) {
                continue;
            }
            expect(chunk_digest(*(*one)[index], names) ==
                   chunk_digest(*(*eight)[index], names))
                << "a chunk differs between one and eight workers";
        }
    };

    "the pipeline ignores the configured tile edge"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        std::vector<std::uint64_t> digests;
        for (const jint tile_size : {2, 4, 8}) {
            auto pipeline = create_pipeline(tile_size, fixed_biome);
            expect(pipeline.has_value())
                << (pipeline ? "" : pipeline.error().message);
            if (!pipeline) {
                return;
            }
            auto served =
                (*pipeline)->generate_chunk(chunk_pos{.x = 7, .z = 7});
            expect(served.has_value())
                << (served ? "" : served.error().message);
            if (!served) {
                return;
            }
            digests.push_back(
                chunk_digest(**served, (*pipeline)->biome_names()));
        }
        std::fprintf(stderr, "pipeline tile-edge digests: %llx %llx %llx\n",
                     static_cast<unsigned long long>(digests[0]),
                     static_cast<unsigned long long>(digests[1]),
                     static_cast<unsigned long long>(digests[2]));
        expect(digests[0] == digests[1]);
        expect(digests[1] == digests[2]);
    };

    "the pipeline advances the configured target status"_test = [] {
        if (!refrain::test_support::vanilla_data().available) {
            return;
        }
        auto features = create_pipeline(8, fixed_biome, std::nullopt,
                                        generation_target::features);
        auto full = create_pipeline(8, fixed_biome, std::nullopt,
                                    generation_target::full);
        expect(features.has_value())
            << (features ? "" : features.error().message);
        expect(full.has_value()) << (full ? "" : full.error().message);
        if (!features || !full) {
            return;
        }
        constexpr chunk_pos probe{.x = 0, .z = 0};
        auto served_features = (*features)->generate_chunk(probe);
        auto served_full = (*full)->generate_chunk(probe);
        expect(served_features.has_value())
            << (served_features ? "" : served_features.error().message);
        expect(served_full.has_value())
            << (served_full ? "" : served_full.error().message);
        if (!served_features || !served_full) {
            return;
        }
        expect((*served_features)->status() == chunk_status::features);
        expect((*served_full)->status() == chunk_status::full);
        expect(block_difference_count(**served_features, **served_full) == 0U);
    };

    "the cache evicts its oldest entries over capacity"_test = [] {
        const test_table table;
        proto_chunk_cache cache{2};
        expect(cache.capacity() == 2U);
        (void)cache.insert(chunk_pos{.x = 0, .z = 0}, make_chunk(table, 0, 0));
        (void)cache.insert(chunk_pos{.x = 1, .z = 0}, make_chunk(table, 1, 0));
        expect(cache.find(chunk_pos{.x = 0, .z = 0}) != nullptr);
        expect(cache.find(chunk_pos{.x = 9, .z = 9}) == nullptr);
        (void)cache.insert(chunk_pos{.x = 2, .z = 0}, make_chunk(table, 2, 0));
        expect(cache.size() == 2U);
        expect(cache.find(chunk_pos{.x = 0, .z = 0}) == nullptr)
            << "the oldest entry was not evicted";
        const auto stats = cache.stats();
        expect(stats.evictions == 1U);
        expect(stats.hits == 1U);
        expect(stats.misses == 2U);
    };

    "a served chunk is readable but refuses writes"_test = [] {
        const test_table table;
        auto center = make_chunk(table, 0, 0);
        auto neighbour = make_chunk(table, 1, 0);
        chunk_region region{*center, 1};
        region.attach_read_only(*neighbour);
        expect(region.find(chunk_pos{.x = 1, .z = 0}) == neighbour.get());
        expect(region.read_only(chunk_pos{.x = 1, .z = 0}));
        // A write into the read-only chunk is dropped and counted, while a
        // write into a writable chunk of the same region lands.
        expect(!region.set_block_state(20, -60, 0, stone_block));
        expect(region.dropped_writes() == 1U);
        expect(neighbour->state_at(4, -60, 0) == air_block);
        expect(region.set_block_state(4, -60, 0, stone_block));
        expect(center->state_at(4, -60, 0) == stone_block);
        // Re-attaching the chunk writable restores writes.
        region.attach(*neighbour);
        expect(!region.read_only(chunk_pos{.x = 1, .z = 0}));
        expect(region.set_block_state(20, -60, 0, stone_block));
        expect(neighbour->state_at(4, -60, 0) == stone_block);
    };
};
