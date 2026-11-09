// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The full-chunk generator against reference worlds and against itself. The
// reference cases are opt-in and read the same environment as the full-stage
// oracle; the deterministic cases run from the content snapshot in this
// repository.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/dimension.hpp>

#include "worldgen/full_stage_support.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::test_support;

auto source_data_root() -> std::string {
    return std::string{REFRAIN_SOURCE_DIR} + "/content/vanilla/26.3/data";
}

auto environment(std::string_view name) -> const char * {
    const std::string key{name};
    return std::getenv(key.c_str());
}

auto integer_environment(std::string_view name, jint fallback) -> jint {
    const auto *value = environment(name);
    return value == nullptr ? fallback : std::atoi(value);
}

auto long_environment(std::string_view name, jlong fallback) -> jlong {
    const auto *value = environment(name);
    return value == nullptr ? fallback : std::strtoll(value, nullptr, 10);
}

auto string_environment(std::string_view name, std::string fallback)
    -> std::string {
    const auto *value = environment(name);
    return value == nullptr ? std::move(fallback) : std::string{value};
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

auto same_structures(const proto_chunk &left, const proto_chunk &right) -> bool;

auto same_chunk(const proto_chunk &left, const proto_chunk &right,
                bool compare_status = true) -> bool {
    if (left.min_y() != right.min_y() || left.height() != right.height() ||
        left.chunk_x() != right.chunk_x() ||
        left.chunk_z() != right.chunk_z() ||
        (compare_status && left.status() != right.status())) {
        return false;
    }
    for (jint y = left.min_y(); y < left.min_y() + left.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                const auto left_name =
                    left.table().name(left.state_at(x, y, z));
                const auto right_name =
                    right.table().name(right.state_at(x, y, z));
                if (left_name != right_name) {
                    std::fprintf(
                        stderr,
                        "chunk (%d,%d) block (%d,%d,%d): %s[%u] vs %s[%u] "
                        "left_air=%s right_air=%s\n",
                        static_cast<int>(left.chunk_x()),
                        static_cast<int>(left.chunk_z()), static_cast<int>(x),
                        static_cast<int>(y), static_cast<int>(z),
                        std::string{left_name}.c_str(),
                        static_cast<unsigned>(left.state_at(x, y, z)),
                        std::string{right_name}.c_str(),
                        static_cast<unsigned>(right.state_at(x, y, z)),
                        std::string{left.table().name(left.table().air())}
                            .c_str(),
                        std::string{right.table().name(right.table().air())}
                            .c_str());
                    return false;
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
                if (left.first_available(kind, x, z) !=
                    right.first_available(kind, x, z)) {
                    std::fprintf(
                        stderr, "chunk (%d,%d) heightmap (%d,%d): %d vs %d\n",
                        static_cast<int>(left.chunk_x()),
                        static_cast<int>(left.chunk_z()), static_cast<int>(x),
                        static_cast<int>(z),
                        static_cast<int>(left.first_available(kind, x, z)),
                        static_cast<int>(right.first_available(kind, x, z)));
                    return false;
                }
            }
        }
    }
    return same_structures(left, right);
}

/// The structure records are part of the output even though they do not
/// change the block grid; compare the fields a batch must reproduce.
auto same_structures(const proto_chunk &left, const proto_chunk &right)
    -> bool {
    const auto &left_starts = left.structure_starts();
    const auto &right_starts = right.structure_starts();
    if (left_starts.size() != right_starts.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left_starts.size(); ++index) {
        const auto &a = left_starts[index];
        const auto &b = right_starts[index];
        if (a.structure != b.structure || a.step != b.step ||
            a.chunk_x != b.chunk_x || a.chunk_z != b.chunk_z ||
            a.min_x != b.min_x || a.min_y != b.min_y || a.min_z != b.min_z ||
            a.max_x != b.max_x || a.max_y != b.max_y || a.max_z != b.max_z ||
            a.pieces.size() != b.pieces.size()) {
            return false;
        }
        for (std::size_t piece = 0; piece < a.pieces.size(); ++piece) {
            const auto &left_piece = a.pieces[piece];
            const auto &right_piece = b.pieces[piece];
            if (left_piece.type != right_piece.type ||
                left_piece.min_x != right_piece.min_x ||
                left_piece.min_y != right_piece.min_y ||
                left_piece.min_z != right_piece.min_z ||
                left_piece.max_x != right_piece.max_x ||
                left_piece.max_y != right_piece.max_y ||
                left_piece.max_z != right_piece.max_z ||
                left_piece.template_id != right_piece.template_id ||
                left_piece.orientation != right_piece.orientation) {
                return false;
            }
        }
    }
    const auto &left_references = left.structure_references();
    const auto &right_references = right.structure_references();
    if (left_references.size() != right_references.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left_references.size(); ++index) {
        const auto &a = left_references[index];
        const auto &b = right_references[index];
        if (a.structure != b.structure || a.chunk_x != b.chunk_x ||
            a.chunk_z != b.chunk_z || a.count != b.count) {
            return false;
        }
    }
    return true;
}

struct reference_data {
    data::nbt::value document{};
    decoded reference{};
};

auto read_reference(std::string_view region_path, jint chunk_x, jint chunk_z,
                    std::string_view data_root,
                    const dimension_profile &profile)
    -> std::optional<reference_data> {
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
    auto reference = decode(*document, motion, motion_no_leaves, profile.min_y,
                            profile.height);
    expect(reference.has_value());
    if (!reference) {
        return std::nullopt;
    }
    reference_data result;
    result.document = std::move(*document);
    result.reference = std::move(*reference);
    return result;
}

/// Compares one opt-in reference chunk. `prefix` names the environment group:
/// `<prefix>_REGION`, `_CHUNK_X`, `_CHUNK_Z`, `_SEED`, `_BIOME`, `_DIMENSION`.
void run_reference_case(std::string_view prefix) {
    const std::string region_key = std::string{prefix} + "_REGION";
    const char *region_path = environment(region_key);
    const char *data_root = environment("REFRAIN_VANILLA_WORLDGEN");
    const char *templates_root = environment("REFRAIN_VANILLA_STRUCTURES");
    if (region_path == nullptr || data_root == nullptr ||
        templates_root == nullptr) {
        return;
    }
    const jint chunk_x =
        integer_environment(std::string{prefix} + "_CHUNK_X", 0);
    const jint chunk_z =
        integer_environment(std::string{prefix} + "_CHUNK_Z", 0);
    const jlong seed =
        long_environment(std::string{prefix} + "_SEED", jlong{1});
    const auto biome =
        string_environment(std::string{prefix} + "_BIOME", "minecraft:plains");
    const auto dimension =
        dimension_environment(std::string{prefix} + "_DIMENSION");
    const auto &profile = dimension_profile_for(dimension);

    const auto reference =
        read_reference(region_path, chunk_x, chunk_z, data_root, profile);
    expect(reference.has_value());
    if (!reference) {
        return;
    }

    std::vector<std::string> structure_sets;
    if (const auto *set = environment(std::string{prefix} + "_STRUCTURE_SET");
        set != nullptr) {
        structure_sets.emplace_back(set);
    }

    auto generator = chunk_generator::create(chunk_generator_config{
        .data_root = data_root,
        .templates_root = templates_root,
        .seed = seed,
        .kind = dimension,
        .fixed_biome = biome,
        .target = generation_target::full,
        .structure_sets = std::move(structure_sets),
        .reference_world = true,
    });
    expect(generator.has_value()) << generator.error().message;
    if (!generator) {
        return;
    }
    const auto chunk =
        (*generator)->generate(chunk_pos{.x = chunk_x, .z = chunk_z});
    expect(chunk.has_value()) << chunk.error().message;
    if (!chunk) {
        return;
    }
    const auto comparison =
        compare_chunks(**chunk, reference->reference, reference->document,
                       profile.min_y, profile.height);
    std::fprintf(stderr,
                 "chunk-generator %.*s: %zu block mismatches, %d heightmap "
                 "mismatches\n",
                 static_cast<int>(prefix.size()), prefix.data(),
                 comparison.block_mismatches, comparison.heightmap_mismatches);
    expect(comparison.block_mismatches == 0)
        << "block mismatches: " << comparison.block_mismatches;
    expect(comparison.heightmap_mismatches == 0)
        << "heightmap mismatches: " << comparison.heightmap_mismatches;
}

auto make_terrain_generator(generation_target target,
                            std::vector<std::string> structure_sets)
    -> std::unique_ptr<chunk_generator> {
    auto created = chunk_generator::create(chunk_generator_config{
        .data_root = source_data_root(),
        .templates_root = "",
        .seed = 12345,
        .kind = dimension_kind::end,
        .fixed_biome = "minecraft:end_barrens",
        .target = target,
        .structure_sets = std::move(structure_sets),
        .reference_world = false,
    });
    expect(created.has_value()) << created.error().message;
    return created ? std::move(*created) : nullptr;
}

} // namespace

suite<"worldgen/chunk-generator"> chunk_generator_tests = [] {
    "the runtime service reproduces the reference full-stage chunk"_test = [] {
        run_reference_case("REFRAIN_FULL_STAGE");
    };

    "the runtime service reproduces a reference mansion chunk"_test = [] {
        run_reference_case("REFRAIN_RUNTIME_MANSION");
    };

    "the runtime service reproduces a reference stronghold chunk"_test = [] {
        run_reference_case("REFRAIN_RUNTIME_STRONGHOLD");
    };

    "serial and parallel batches produce identical chunks"_test = [] {
        auto generator = make_terrain_generator(generation_target::features,
                                                {"oracle:no_structure_set"});
        if (generator == nullptr) {
            return;
        }
        const std::array<chunk_pos, 4> positions{{
            {0, 0},
            {1, 0},
            {0, 1},
            {2, 2},
        }};
        const auto serial = generator->generate_batch(positions, 1);
        expect(serial.has_value()) << serial.error().message;
        if (!serial) {
            return;
        }
        expect(serial->size() == positions.size());
        const auto parallel = generator->generate_batch(positions, 3);
        expect(parallel.has_value()) << parallel.error().message;
        if (!parallel) {
            return;
        }
        expect(parallel->size() == positions.size());
        for (std::size_t index = 0; index < positions.size(); ++index) {
            expect((*serial)[index] != nullptr);
            expect((*parallel)[index] != nullptr);
            if ((*serial)[index] != nullptr && (*parallel)[index] != nullptr) {
                expect(same_chunk(*(*serial)[index], *(*parallel)[index]))
                    << "chunk differs between serial and parallel generation";
            }
        }
        const auto area =
            generator->generate_area(chunk_pos{.x = 0, .z = 0}, 2, 2, 2);
        expect(area.has_value()) << area.error().message;
        if (area) {
            const std::array<chunk_pos, 4> area_positions{{
                {0, 0},
                {0, 1},
                {1, 0},
                {1, 1},
            }};
            for (std::size_t index = 0; index < area_positions.size();
                 ++index) {
                expect((*area)[index] != nullptr);
                if ((*area)[index] == nullptr) {
                    continue;
                }
                expect((*area)[index]->chunk_x() == area_positions[index].x);
                expect((*area)[index]->chunk_z() == area_positions[index].z);
            }
        }
    };

    "serial and parallel batches agree on structure chunks"_test = [] {
        auto created = chunk_generator::create(chunk_generator_config{
            .data_root = source_data_root(),
            .templates_root = "",
            .seed = 1,
            .kind = dimension_kind::overworld,
            .fixed_biome = "minecraft:plains",
            .target = generation_target::full,
            .structure_sets = {"minecraft:mineshafts"},
            .reference_world = true,
        });
        expect(created.has_value()) << created.error().message;
        if (!created) {
            return;
        }
        auto generator = std::move(*created);
        const std::array<chunk_pos, 3> positions{{
            {-4, 0},
            {-3, 0},
            {-4, 1},
        }};
        const auto serial = generator->generate_batch(positions, 1);
        expect(serial.has_value()) << serial.error().message;
        if (!serial) {
            return;
        }
        const auto parallel = generator->generate_batch(positions, 3);
        expect(parallel.has_value()) << parallel.error().message;
        if (!parallel) {
            return;
        }
        for (std::size_t index = 0; index < positions.size(); ++index) {
            expect((*serial)[index] != nullptr);
            expect((*parallel)[index] != nullptr);
            if ((*serial)[index] != nullptr && (*parallel)[index] != nullptr) {
                expect(same_chunk(*(*serial)[index], *(*parallel)[index]));
            }
        }
        expect(!(*serial)[0]->structure_starts().empty());
    };

    "full batches are identical across repeats and workers"_test = [] {
        auto created = chunk_generator::create(chunk_generator_config{
            .data_root = source_data_root(),
            .templates_root = "",
            .seed = 7,
            .kind = dimension_kind::end,
            .fixed_biome = "minecraft:end_barrens",
            .target = generation_target::full,
            .structure_sets = {},
            .reference_world = false,
        });
        expect(created.has_value()) << created.error().message;
        if (!created) {
            return;
        }
        auto generator = std::move(*created);
        const chunk_pos origin{.x = 0, .z = 0};
        // The first call fills the start-probe caches, the second reuses them,
        // and the parallel call uses one cache per worker.
        const auto cold = generator->generate_area(origin, 4, 2, 1);
        expect(cold.has_value()) << cold.error().message;
        if (!cold) {
            return;
        }
        const auto warm = generator->generate_area(origin, 4, 2, 1);
        expect(warm.has_value()) << warm.error().message;
        if (!warm) {
            return;
        }
        const auto parallel = generator->generate_area(origin, 4, 2, 4);
        expect(parallel.has_value()) << parallel.error().message;
        if (!parallel) {
            return;
        }
        expect(warm->size() == cold->size());
        expect(parallel->size() == cold->size());
        for (std::size_t index = 0;
             index < std::min(cold->size(), warm->size()); ++index) {
            expect((*cold)[index] != nullptr);
            expect((*warm)[index] != nullptr);
            if ((*cold)[index] != nullptr && (*warm)[index] != nullptr) {
                expect(same_chunk(*(*cold)[index], *(*warm)[index]))
                    << "a repeated batch changed the chunk";
            }
        }
        for (std::size_t index = 0;
             index < std::min(cold->size(), parallel->size()); ++index) {
            expect((*parallel)[index] != nullptr);
            if ((*cold)[index] != nullptr && (*parallel)[index] != nullptr) {
                expect(same_chunk(*(*cold)[index], *(*parallel)[index]))
                    << "serial and parallel batches disagree";
            }
        }

        // Jigsaw pools are shared by the workers, so generate a village
        // neighbourhood in parallel as well.
        auto village = chunk_generator::create(chunk_generator_config{
            .data_root = source_data_root(),
            .templates_root = "",
            .seed = 11,
            .kind = dimension_kind::overworld,
            .fixed_biome = "minecraft:plains",
            .target = generation_target::full,
            .structure_sets = {"minecraft:villages"},
            .reference_world = true,
        });
        expect(village.has_value()) << village.error().message;
        if (!village) {
            return;
        }
        const chunk_pos village_origin{.x = 0, .z = 0};
        const auto village_serial =
            (*village)->generate_area(village_origin, 3, 2, 1);
        expect(village_serial.has_value()) << village_serial.error().message;
        if (!village_serial) {
            return;
        }
        const auto village_parallel =
            (*village)->generate_area(village_origin, 3, 2, 3);
        expect(village_parallel.has_value())
            << village_parallel.error().message;
        if (!village_parallel) {
            return;
        }
        expect(village_parallel->size() == village_serial->size());
        for (std::size_t index = 0;
             index < std::min(village_serial->size(), village_parallel->size());
             ++index) {
            expect((*village_serial)[index] != nullptr);
            expect((*village_parallel)[index] != nullptr);
            if ((*village_serial)[index] != nullptr &&
                (*village_parallel)[index] != nullptr) {
                expect(same_chunk(*(*village_serial)[index],
                                  *(*village_parallel)[index]))
                    << "serial and parallel village chunks disagree";
            }
        }
    };

    "configuration errors are values"_test = [] {
        const auto missing = chunk_generator::create(chunk_generator_config{
            .data_root = "",
            .templates_root = "",
            .seed = 0,
            .kind = dimension_kind::overworld,
            .fixed_biome = "minecraft:plains",
            .target = generation_target::terrain,
            .structure_sets = {},
            .reference_world = true,
        });
        expect(!missing.has_value());

        auto generator = make_terrain_generator(generation_target::terrain, {});
        if (generator == nullptr) {
            return;
        }
        expect(!generator->generate_area(chunk_pos{.x = 0, .z = 0}, 0, 1, 1)
                    .has_value());
        const auto empty =
            generator->generate_batch(std::span<const chunk_pos>{}, 2);
        expect(empty.has_value());
        if (empty) {
            expect(empty->empty());
        }

        const auto chunk = generator->generate(chunk_pos{.x = 0, .z = 0});
        expect(chunk.has_value()) << chunk.error().message;
        if (chunk) {
            expect((*chunk)->status() == chunk_status::terrain);
            expect((*chunk)->structure_starts().empty());
            expect((*chunk)->structure_references().empty());
        }
    };

    "the runtime generator creates the reference overworld biomes"_test = [] {
        constexpr std::array<std::string_view, 9> biomes{{
            "minecraft:plains",
            "minecraft:forest",
            "minecraft:taiga",
            "minecraft:desert",
            "minecraft:jungle",
            "minecraft:ocean",
            "minecraft:deep_ocean",
            "minecraft:dark_forest",
            "minecraft:eroded_badlands",
        }};
        for (const auto biome : biomes) {
            auto created = chunk_generator::create(chunk_generator_config{
                .data_root = source_data_root(),
                .templates_root = "",
                .seed = 1,
                .kind = dimension_kind::overworld,
                .fixed_biome = std::string{biome},
                .target = generation_target::features,
                .structure_sets = {"oracle:no_structure_set"},
                .reference_world = false,
            });
            expect(created.has_value())
                << biome << ": "
                << (created ? std::string{} : created.error().message);
            if (!created) {
                expect(created.error().code != error_code::unsupported)
                    << biome << " reported an unsupported type";
            }
        }
    };

    "a full plains chunk generates at the features and full targets"_test = [] {
        for (const auto target :
             {generation_target::features, generation_target::full}) {
            auto created = chunk_generator::create(chunk_generator_config{
                .data_root = source_data_root(),
                .templates_root = "",
                .seed = 1,
                .kind = dimension_kind::overworld,
                .fixed_biome = "minecraft:plains",
                .target = target,
                // Structure starts only add cost to this feature case; the
                // feature stage still runs every step for the fixed biome.
                .structure_sets = {"oracle:no_structure_set"},
                .reference_world = false,
            });
            expect(created.has_value()) << created.error().message;
            if (!created) {
                return;
            }
            const auto chunk = (*created)->generate(chunk_pos{.x = 0, .z = 0});
            expect(chunk.has_value()) << chunk.error().message;
            if (!chunk) {
                return;
            }
            expect((*chunk)->status() == (target == generation_target::features
                                              ? chunk_status::features
                                              : chunk_status::full));
        }
    };

    "the target ladder advances terrain, features and full"_test = [] {
        auto terrain_generator = make_terrain_generator(
            generation_target::terrain, {"oracle:no_structure_set"});
        auto features_generator = make_terrain_generator(
            generation_target::features, {"oracle:no_structure_set"});
        auto full_generator = make_terrain_generator(
            generation_target::full, {"oracle:no_structure_set"});
        if (terrain_generator == nullptr || features_generator == nullptr ||
            full_generator == nullptr) {
            return;
        }
        auto terrain = terrain_generator->generate(chunk_pos{.x = 0, .z = 0});
        auto features = features_generator->generate(chunk_pos{.x = 0, .z = 0});
        auto full = full_generator->generate(chunk_pos{.x = 0, .z = 0});
        expect(terrain.has_value()) << terrain.error().message;
        expect(features.has_value()) << features.error().message;
        expect(full.has_value()) << full.error().message;
        if (!terrain || !features || !full) {
            return;
        }
        expect((*terrain)->status() == chunk_status::terrain);
        expect((*features)->status() == chunk_status::features);
        expect((*full)->status() == chunk_status::full);
        expect(same_chunk(**features, **full, false));
    };
};
