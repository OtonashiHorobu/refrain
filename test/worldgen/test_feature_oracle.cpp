// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Opt-in feature oracles: the production chunk generator runs the real
// features of one fixed biome and the saved chunk is compared block for block
// and in all four final heightmaps. The reference world keeps the fixed
// biome's real surface pass, carvers and features with structures disabled, so
// no structure set participates on either side.
//
// Each case reads its region, chunk and seed from the environment:
// `<PREFIX>_REGION`, `_CHUNK_X`, `_CHUNK_Z`, `_SEED`, `_BIOME` and the optional
// `_DIMENSION`, `_TARGET`, `_NEIGHBOURS` and `_NEIGHBOURS_FIRST`.
// `REFRAIN_VANILLA_WORLDGEN` points at the datapack `data` root shared with
// the other reference suites. `_NEIGHBOURS_ORDER` decorates the neighbour
// chunks that precede the target in force-load order first and the rest after
// it, `_NEIGHBOUR_RADIUS` widens the decorated square, and `_NEIGHBOUR_BOX`
// names the reference world's generated chunk rectangle as
// `min_x,min_z,max_x,max_z`. `_NEIGHBOURS_SWEEP` follows the ticket queue of a
// force-load instead of ascending x and z: every force-loaded chunk first
// decorates the three-by-three around it, then the propagated full chunks two
// rings out decorate one more layer each.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/dimension.hpp>

#include "worldgen/feature_oracle_support.hpp"

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::test_support;

auto environment(std::string_view name) -> const char * {
    const std::string key{name};
    return std::getenv(key.c_str());
}

auto integer_environment(std::string_view name) -> std::optional<jint> {
    const auto *value = environment(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return static_cast<jint>(std::atoi(value));
}

auto long_environment(std::string_view name) -> std::optional<jlong> {
    const auto *value = environment(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return static_cast<jlong>(std::strtoll(value, nullptr, 10));
}

auto dimension_environment(std::string_view name) -> dimension_kind {
    const auto *value = environment(name);
    if (value == nullptr) {
        return dimension_kind::overworld;
    }
    const std::string_view text{value};
    if (text == "the_nether") {
        return dimension_kind::nether;
    }
    if (text == "the_end") {
        return dimension_kind::end;
    }
    return dimension_kind::overworld;
}

struct reference_chunk {
    data::nbt::value document{};
    decoded blocks{};
};

auto read_reference(std::string_view region_path, jint chunk_x, jint chunk_z,
                    std::string_view data_root,
                    const dimension_profile &profile)
    -> std::optional<reference_chunk> {
    const auto region = core::io::read_file(region_path);
    expect(region.has_value()) << region.error().message;
    if (!region) {
        return std::nullopt;
    }
    const auto payload = chunk_payload(*region, chunk_x, chunk_z);
    expect(payload.has_value());
    if (!payload) {
        return std::nullopt;
    }
    const auto document =
        data::nbt::read(*payload, data::nbt::compression::zlib);
    expect(document.has_value()) << document.error().message;
    if (!document) {
        return std::nullopt;
    }
    const auto block_tags = load_block_tags(data_root);
    expect(block_tags.has_value()) << block_tags.error().message;
    if (!block_tags) {
        return std::nullopt;
    }
    const auto resolve_tag_names = [&](std::string_view tag) {
        std::set<std::string> names;
        if (!block_tags->has(tag)) {
            return names;
        }
        auto resolved = block_tags->resolve(tag);
        expect(resolved.has_value());
        if (resolved) {
            names.insert(resolved->begin(), resolved->end());
        }
        return names;
    };
    const auto motion =
        resolve_tag_names("minecraft:blocks_motion_in_heightmap");
    const auto motion_no_leaves =
        resolve_tag_names("minecraft:blocks_motion_in_heightmap_no_leaves");
    auto blocks = decode(*document, motion, motion_no_leaves, profile.min_y,
                         profile.height);
    expect(blocks.has_value());
    if (!blocks) {
        return std::nullopt;
    }
    reference_chunk result;
    result.document = std::move(*document);
    result.blocks = std::move(*blocks);
    return result;
}

/// Compares one fixed-biome feature reference chunk. The prefix names the
/// environment group: `<prefix>_REGION`, `_CHUNK_X`, `_CHUNK_Z`, `_SEED`,
/// `_BIOME` and the optional `_DIMENSION` and `_TARGET` (`terrain` stops
/// before structure starts and features).
void run_feature_case(std::string_view prefix) {
    const std::string region_key = std::string{prefix} + "_REGION";
    const char *region_path = environment(region_key);
    const char *data_root = environment("REFRAIN_VANILLA_WORLDGEN");
    const auto chunk_x = integer_environment(std::string{prefix} + "_CHUNK_X");
    const auto chunk_z = integer_environment(std::string{prefix} + "_CHUNK_Z");
    const auto seed = long_environment(std::string{prefix} + "_SEED");
    const char *biome_raw = environment(std::string{prefix} + "_BIOME");
    if (region_path == nullptr || data_root == nullptr || !chunk_x ||
        !chunk_z || !seed || biome_raw == nullptr) {
        return;
    }
    const std::string biome{biome_raw};
    const auto dimension =
        dimension_environment(std::string{prefix} + "_DIMENSION");
    const auto &profile = dimension_profile_for(dimension);
    generation_target target = generation_target::full;
    if (const char *target_raw = environment(std::string{prefix} + "_TARGET");
        target_raw != nullptr) {
        if (std::strcmp(target_raw, "terrain") == 0) {
            target = generation_target::terrain;
        }
    }

    const auto reference =
        read_reference(region_path, *chunk_x, *chunk_z, data_root, profile);
    expect(reference.has_value());
    if (!reference) {
        return;
    }

    // Naming a structure set that does not exist disables structure starts
    // while keeping the feature stage active. A reference world saves the
    // chunks of its generated area, so a compared chunk also contains feature
    // writes from its neighbours; the reference mode decorates the same
    // region, and the optional order switch selects which side wins a contact.
    const bool decorate_neighbours =
        environment(std::string{prefix} + "_NEIGHBOURS") != nullptr;
    const bool neighbours_before_target =
        environment(std::string{prefix} + "_NEIGHBOURS_FIRST") != nullptr;
    const bool neighbours_in_generation_order =
        environment(std::string{prefix} + "_NEIGHBOURS_ORDER") != nullptr;
    const bool neighbours_ticket_sweep =
        environment(std::string{prefix} + "_NEIGHBOURS_SWEEP") != nullptr;
    const auto neighbour_radius =
        integer_environment(std::string{prefix} + "_NEIGHBOUR_RADIUS");
    auto neighbour_box = chunk_rectangle{};
    if (const char *box_raw =
            environment(std::string{prefix} + "_NEIGHBOUR_BOX");
        box_raw != nullptr) {
        jint min_x = 0;
        jint min_z = 0;
        jint max_x = 0;
        jint max_z = 0;
        if (std::sscanf(box_raw, "%d,%d,%d,%d", &min_x, &min_z, &max_x,
                        &max_z) == 4) {
            neighbour_box = chunk_rectangle{
                .min_x = min_x, .min_z = min_z, .max_x = max_x, .max_z = max_z};
        }
    }
    auto generator = chunk_generator::create(chunk_generator_config{
        .data_root = data_root,
        .templates_root = "",
        .seed = *seed,
        .kind = dimension,
        .fixed_biome = biome,
        .target = target,
        .structure_sets = {"oracle:feature_oracle_no_structures"},
        .reference_world = false,
        .decorate_neighbours = decorate_neighbours,
        .neighbours_before_target = neighbours_before_target,
        .neighbours_in_generation_order = neighbours_in_generation_order,
        .neighbours_ticket_sweep = neighbours_ticket_sweep,
        .neighbour_radius = neighbour_radius.value_or(1),
        .neighbour_box = neighbour_box,
    });
    expect(generator.has_value()) << generator.error().message;
    if (!generator) {
        return;
    }
    const auto chunk =
        (*generator)->generate(chunk_pos{.x = *chunk_x, .z = *chunk_z});
    expect(chunk.has_value()) << chunk.error().message;
    if (!chunk) {
        return;
    }
    const auto comparison =
        compare_feature_chunks(**chunk, reference->blocks, reference->document,
                               profile.min_y, profile.height);
    std::fprintf(stderr,
                 "feature-oracle %.*s: %zu block mismatches, %d heightmap "
                 "mismatches\n",
                 static_cast<int>(prefix.size()), prefix.data(),
                 comparison.block_mismatches, comparison.heightmap_mismatches);
    expect(comparison.block_mismatches == 0)
        << "block mismatches: " << comparison.block_mismatches;
    expect(comparison.heightmap_mismatches == 0)
        << "heightmap mismatches: " << comparison.heightmap_mismatches;
}

} // namespace

suite<"worldgen/feature-oracle"> feature_oracle_tests = [] {
    "the runtime feature oracle reproduces a lush caves chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_LUSH_CAVES");
    };

    "the runtime feature oracle reproduces a deep dark chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_DEEP_DARK");
    };

    "the runtime feature oracle reproduces a cherry grove chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_CHERRY_GROVE");
    };

    "the runtime feature oracle reproduces a warm ocean chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_WARM_OCEAN");
    };

    "the runtime feature oracle reproduces a basalt deltas chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_BASALT_DELTAS");
    };

    "the runtime feature oracle reproduces a plains chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_PLAINS");
    };

    "the runtime feature oracle reproduces a taiga chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_TAIGA");
    };

    "the runtime feature oracle reproduces a desert chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_DESERT");
    };

    "the runtime feature oracle reproduces an ice spikes chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_ICE_SPIKES");
    };

    "the runtime feature oracle reproduces a badlands chunk"_test = [] {
        run_feature_case("REFRAIN_FEATURE_BADLANDS");
    };
};
