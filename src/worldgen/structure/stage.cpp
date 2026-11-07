// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/stage.hpp>

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/generation.hpp>

#include <algorithm>
#include <memory>
#include <optional>

namespace refrain::worldgen::structure {

namespace {

constexpr jint horizontal_west = 0;
constexpr jint horizontal_east = 1;
constexpr jint horizontal_south = 2;
constexpr jint horizontal_north = 3;

auto quart(jint block) -> jint { return block >> 2; }

auto horizontal_bounds(jint west, jint floor, jint north, jint width,
                       jint height, jint depth, jint direction)
    -> structure_bbox {
    if (direction == horizontal_north || direction == horizontal_south) {
        return structure_bbox{.min_x = west,
                              .min_y = floor,
                              .min_z = north,
                              .max_x = west + width - 1,
                              .max_y = floor + height - 1,
                              .max_z = north + depth - 1};
    }
    return structure_bbox{.min_x = west,
                          .min_y = floor,
                          .min_z = north,
                          .max_x = west + depth - 1,
                          .max_y = floor + height - 1,
                          .max_z = north + width - 1};
}

auto orientation(jint direction) -> jint {
    switch (direction) {
    case horizontal_north:
        return 2;
    case horizontal_east:
        return 3;
    case horizontal_south:
        return 0;
    default:
        return 1;
    }
}

auto make_start(std::string_view id, std::string_view step,
                const chunk_pos &chunk, structure_piece piece)
    -> structure_start {
    structure_start start{
        .structure = std::string{id},
        .step = std::string{step},
        .chunk = chunk,
        .pieces = {std::move(piece)},
        .references = 0,
        .max_references = 1,
        .bounds = {},
    };
    start.bounds = start.pieces.front().bounds;
    return start;
}

auto biome_allowed(const structure_start_context &context,
                   const structure_document &structure, jint block_x,
                   jint block_y, jint block_z) -> bool {
    if (!context.biome_at || !context.biome_name || context.data == nullptr) {
        return false;
    }
    const auto biome =
        context.biome_at(quart(block_x), quart(block_y), quart(block_z));
    return context.data->allows_biome(structure, context.biome_name(biome));
}

auto biome_column_allowed(const structure_start_context &context,
                          const structure_document &structure, jint block_x,
                          jint block_z) -> bool {
    if (!context.biome_at || !context.biome_name || context.data == nullptr) {
        return false;
    }
    const auto min_quart_y = quart(context.min_y - 1);
    const auto max_quart_y = quart(context.min_y + context.world_height - 1);
    for (jint y = min_quart_y; y <= max_quart_y; ++y) {
        const auto biome = context.biome_at(quart(block_x), y, quart(block_z));
        if (context.data->allows_biome(structure, context.biome_name(biome))) {
            return true;
        }
    }
    return false;
}

auto surface_start(const structure_start_context &context,
                   const structure_document &structure, const chunk_pos &chunk,
                   jint piece_width, jint piece_height, jint piece_depth,
                   bool single_piece) -> std::optional<structure_start> {
    if (!biome_column_allowed(context, structure, chunk.x * 16 + 8,
                              chunk.z * 16 + 8)) {
        return std::nullopt;
    }
    if (single_piece) {
        const jint west = chunk.x * 16;
        const jint north = chunk.z * 16;
        const jint lowest = std::min(
            std::min(context.height_at(west, north,
                                       heightmap_kind::world_surface_wg),
                     context.height_at(west, north + piece_depth,
                                       heightmap_kind::world_surface_wg)),
            std::min(context.height_at(west + piece_width, north,
                                       heightmap_kind::world_surface_wg),
                     context.height_at(west + piece_width, north + piece_depth,
                                       heightmap_kind::world_surface_wg)));
        if (lowest < context.sea_level) {
            return std::nullopt;
        }
    }
    const jint middle_x = chunk.x * 16 + 8;
    const jint middle_z = chunk.z * 16 + 8;
    const jint y =
        context.height_at(middle_x, middle_z, heightmap_kind::world_surface_wg);
    if (!biome_allowed(context, structure, middle_x, y, middle_z)) {
        return std::nullopt;
    }
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(context.seed, chunk.x, chunk.z);
    const jint direction = random.next_int(4);
    auto piece = structure_piece{
        .type = std::string{},
        .bounds = horizontal_bounds(chunk.x * 16, 64, chunk.z * 16, piece_width,
                                    piece_height, piece_depth, direction),
        .orientation = orientation(direction),
    };
    if (structure.type == "minecraft:swamp_hut") {
        piece.type = "minecraft:tesh";
    } else if (structure.type == "minecraft:desert_pyramid") {
        piece.type = "minecraft:tedp";
    } else {
        piece.type = "minecraft:tejp";
    }
    return make_start(structure.id, structure.step, chunk, std::move(piece));
}

auto try_generate(const structure_start_context &context,
                  const structure_document &structure, const chunk_pos &chunk)
    -> std::optional<structure_start> {
    if (structure.type == "minecraft:buried_treasure") {
        if (!biome_column_allowed(context, structure, chunk.x * 16 + 8,
                                  chunk.z * 16 + 8)) {
            return std::nullopt;
        }
        const jint y = context.height_at(chunk.x * 16 + 8, chunk.z * 16 + 8,
                                         heightmap_kind::ocean_floor_wg);
        if (!biome_allowed(context, structure, chunk.x * 16 + 8, y,
                           chunk.z * 16 + 8)) {
            return std::nullopt;
        }
        auto start = buried_treasure_start(chunk);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:swamp_hut") {
        return surface_start(context, structure, chunk, 7, 7, 9, false);
    }
    if (structure.type == "minecraft:desert_pyramid") {
        return surface_start(context, structure, chunk, 21, 15, 21, true);
    }
    if (structure.type == "minecraft:jungle_temple") {
        return surface_start(context, structure, chunk, 12, 10, 15, true);
    }
    return std::nullopt;
}

} // namespace

auto generate_structure_starts(const chunk_pos &chunk,
                               const structure_start_context &context)
    -> std::vector<structure_start> {
    std::vector<structure_start> starts;
    if (context.data == nullptr) {
        return starts;
    }
    for (const auto &[set_id, set] : context.data->sets) {
        (void)set_id;
        if (!set.random_spread.has_value() ||
            !set.random_spread->is_structure_chunk(context.seed, chunk.x,
                                                   chunk.z)) {
            continue;
        }
        for (const auto &candidate : set.weighted_order(context.seed, chunk)) {
            const auto found =
                context.data->structures.find(candidate.structure);
            if (found == context.data->structures.end()) {
                continue;
            }
            if (auto start = try_generate(context, found->second, chunk)) {
                starts.push_back(std::move(*start));
                break;
            }
        }
    }
    return starts;
}

void write_structure_stage(proto_chunk &chunk,
                           const buried_starts_result &result) {
    for (const auto &start : result.starts) {
        if (start.chunk.x != chunk.chunk_x() ||
            start.chunk.z != chunk.chunk_z()) {
            continue;
        }
        ::refrain::worldgen::structure_start record{
            .structure = start.structure,
            .step = start.step,
            .chunk_x = start.chunk.x,
            .chunk_z = start.chunk.z,
            .min_x = start.bounds.min_x,
            .min_y = start.bounds.min_y,
            .min_z = start.bounds.min_z,
            .max_x = start.bounds.max_x,
            .max_y = start.bounds.max_y,
            .max_z = start.bounds.max_z,
        };
        record.pieces.reserve(start.pieces.size());
        for (const auto &piece : start.pieces) {
            record.pieces.push_back(::refrain::worldgen::structure_piece_record{
                .type = piece.type,
                .min_x = piece.bounds.min_x,
                .min_y = piece.bounds.min_y,
                .min_z = piece.bounds.min_z,
                .max_x = piece.bounds.max_x,
                .max_y = piece.bounds.max_y,
                .max_z = piece.bounds.max_z,
                .orientation = piece.orientation});
        }
        chunk.add_structure_start(std::move(record));
    }
    for (const auto &reference : result.references) {
        chunk.add_structure_reference(reference.structure, reference.chunk_x,
                                      reference.chunk_z, reference.count);
    }
}

auto generate_buried_starts(jlong seed, const chunk_pos &target,
                            const std::vector<selected_structure> &structures,
                            jint radius) -> buried_starts_result {
    buried_starts_result result;
    for (jint dx = -radius; dx <= radius; ++dx) {
        for (jint dz = -radius; dz <= radius; ++dz) {
            const chunk_pos chunk{.x = target.x + dx, .z = target.z + dz};
            for (const auto &structure : structures) {
                if (!structure.placement.is_structure_chunk(seed, chunk.x,
                                                            chunk.z)) {
                    continue;
                }
                auto start = buried_treasure_start(chunk);
                start.structure = structure.id;
                result.starts.push_back(std::move(start));
            }
        }
    }
    result.references = collect_references(result.starts, target, radius);
    return result;
}

} // namespace refrain::worldgen::structure
