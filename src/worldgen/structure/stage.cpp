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
    if (structure.type == "minecraft:jigsaw") {
        if (context.templates == nullptr || context.data == nullptr) {
            return std::nullopt;
        }
        return jigsaw_start(
            chunk, jigsaw_generation_context{
                       .structure = &structure,
                       .data = context.data,
                       .templates = context.templates,
                       .seed = context.seed,
                       .min_y = context.min_y,
                       .world_height = context.world_height,
                       .sea_level = context.sea_level,
                       .height_at = context.height_at,
                       .valid_biome =
                           [&](const block_pos &pos) {
                               return biome_allowed(context, structure, pos.x,
                                                    pos.y, pos.z);
                           },
                       .column_allowed =
                           [&](jint x, jint z) {
                               return biome_column_allowed(context, structure,
                                                           x, z);
                           },
                   });
    }
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
    if (structure.type == "minecraft:igloo") {
        if (context.templates == nullptr) {
            return std::nullopt;
        }
        const jint center_x = chunk.x * 16 + 8;
        const jint center_z = chunk.z * 16 + 8;
        const jint center_y = context.height_at(
            center_x, center_z, heightmap_kind::world_surface_wg);
        if (!biome_allowed(context, structure, center_x, center_y, center_z)) {
            return std::nullopt;
        }
        auto start = igloo_start(chunk, context.seed, *context.templates);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:swamp_hut") {
        const jint middle_x = chunk.x * 16 + 8;
        const jint middle_z = chunk.z * 16 + 8;
        const jint y = context.height_at(middle_x, middle_z,
                                         heightmap_kind::world_surface_wg);
        if (!biome_allowed(context, structure, middle_x, y, middle_z)) {
            return std::nullopt;
        }
        auto start = swamp_hut_start(chunk, context.seed);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:ocean_monument") {
        const jint middle_x = chunk.x * 16 + 8;
        const jint middle_z = chunk.z * 16 + 8;
        const jint middle_y = context.height_at(middle_x, middle_z,
                                                heightmap_kind::ocean_floor_wg);
        if (!biome_allowed(context, structure, middle_x, middle_y, middle_z)) {
            return std::nullopt;
        }
        if (!context.biome_at || !context.biome_name ||
            context.data == nullptr) {
            return std::nullopt;
        }
        // `getBiomesWithin` samples the quart box around the chunk offset
        // (9, sea level, 9); every biome must belong to the required
        // surrounding tag.
        const jint offset_x = chunk.x * 16 + 9;
        const jint offset_z = chunk.z * 16 + 9;
        const jint quart_x0 = (offset_x - 29) >> 2;
        const jint quart_x1 = (offset_x + 29) >> 2;
        const jint quart_z0 = (offset_z - 29) >> 2;
        const jint quart_z1 = (offset_z + 29) >> 2;
        const jint quart_y0 = (context.sea_level - 29) >> 2;
        const jint quart_y1 = (context.sea_level + 29) >> 2;
        for (jint quart_z = quart_z0; quart_z <= quart_z1; ++quart_z) {
            for (jint quart_x = quart_x0; quart_x <= quart_x1; ++quart_x) {
                for (jint quart_y = quart_y0; quart_y <= quart_y1; ++quart_y) {
                    const auto biome =
                        context.biome_at(quart_x, quart_y, quart_z);
                    if (!context.data->biome_matches_tag(
                            "minecraft:required_ocean_monument_surrounding",
                            context.biome_name(biome))) {
                        return std::nullopt;
                    }
                }
            }
        }
        auto start = ocean_monument_start(chunk, context.seed);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:stronghold") {
        if (!biome_allowed(context, structure, chunk.x * 16, 0, chunk.z * 16)) {
            return std::nullopt;
        }
        auto start = stronghold_start(chunk, context.seed, context.min_y,
                                      context.sea_level);
        if (start.pieces.empty()) {
            return std::nullopt;
        }
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:jungle_temple") {
        const jint middle_x = chunk.x * 16 + 8;
        const jint middle_z = chunk.z * 16 + 8;
        const jint y = context.height_at(middle_x, middle_z,
                                         heightmap_kind::world_surface_wg);
        if (!biome_allowed(context, structure, middle_x, y, middle_z)) {
            return std::nullopt;
        }
        auto start = jungle_temple_start(chunk, context.seed, context.sea_level,
                                         context.height_at);
        if (!start) {
            return std::nullopt;
        }
        start->structure = structure.id;
        start->step = structure.step;
        return *start;
    }
    if (structure.type == "minecraft:mineshaft") {
        bool mesa = false;
        if (const auto *value = structure.config.find("mineshaft_type");
            value != nullptr) {
            mesa = value->as_string().value_or("normal") == "mesa";
        }
        const jint stub_x = chunk.x * 16 + 8;
        const jint stub_z = chunk.z * 16;
        if (!biome_allowed(context, structure, stub_x, 50, stub_z)) {
            return std::nullopt;
        }
        auto start = mineshaft_start(chunk, context.seed, context.min_y,
                                     context.sea_level, mesa);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:shipwreck") {
        if (context.templates == nullptr) {
            return std::nullopt;
        }
        const auto *beached_value = structure.config.find("is_beached");
        const bool beached = beached_value != nullptr &&
                             beached_value->as_bool().value_or(false);
        const auto kind = beached ? heightmap_kind::world_surface_wg
                                  : heightmap_kind::ocean_floor_wg;
        const jint middle_x = chunk.x * 16 + 8;
        const jint middle_z = chunk.z * 16 + 8;
        const jint y = context.height_at(middle_x, middle_z, kind);
        if (!biome_allowed(context, structure, middle_x, y, middle_z)) {
            return std::nullopt;
        }
        auto start = shipwreck_start(chunk, context.seed, *context.templates,
                                     beached, context.height_at);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:ocean_ruin") {
        if (context.templates == nullptr) {
            return std::nullopt;
        }
        bool warm = true;
        if (const auto *value = structure.config.find("biome_temp");
            value != nullptr) {
            warm = value->as_string().value_or("warm") == "warm";
        }
        float large_probability = 0.3F;
        if (const auto *value = structure.config.find("large_probability");
            value != nullptr) {
            large_probability =
                static_cast<float>(value->as_double().value_or(0.3));
        }
        float cluster_probability = 0.9F;
        if (const auto *value = structure.config.find("cluster_probability");
            value != nullptr) {
            cluster_probability =
                static_cast<float>(value->as_double().value_or(0.9));
        }
        const jint middle_x = chunk.x * 16 + 8;
        const jint middle_z = chunk.z * 16 + 8;
        const jint y = context.height_at(middle_x, middle_z,
                                         heightmap_kind::ocean_floor_wg);
        if (!biome_allowed(context, structure, middle_x, y, middle_z)) {
            return std::nullopt;
        }
        auto start =
            ocean_ruin_start(chunk, context.seed, *context.templates, warm,
                             large_probability, cluster_probability);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:woodland_mansion") {
        if (context.templates == nullptr) {
            return std::nullopt;
        }
        const jint center_x = chunk.x * 16 + 7;
        const jint center_z = chunk.z * 16 + 7;
        const jint center_y = context.height_at(
            center_x, center_z, heightmap_kind::world_surface_wg);
        if (!biome_allowed(context, structure, center_x, center_y, center_z)) {
            return std::nullopt;
        }
        auto start = woodland_mansion_start(
            chunk, context.seed, *context.templates, context.height_at);
        if (start.pieces.empty()) {
            return std::nullopt;
        }
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:ruined_portal") {
        if (context.templates == nullptr) {
            return std::nullopt;
        }
        auto start = ruined_portal_start(chunk, context.seed, structure,
                                         *context.templates, context.height_at,
                                         context.opaque_at, context.biome_cold,
                                         context.min_y);
        if (start.pieces.empty()) {
            return std::nullopt;
        }
        return start;
    }
    if (structure.type == "minecraft:fortress") {
        if (!biome_allowed(context, structure, chunk.x * 16, 64,
                           chunk.z * 16)) {
            return std::nullopt;
        }
        auto start = fortress_start(chunk, context.seed);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:end_city") {
        if (context.templates == nullptr || context.data == nullptr) {
            return std::nullopt;
        }
        const jint block_x = chunk.x * 16 + 7;
        const jint block_z = chunk.z * 16 + 7;
        if (!biome_column_allowed(context, structure, block_x, block_z)) {
            return std::nullopt;
        }
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(0)};
        random.set_large_feature_seed(context.seed, chunk.x, chunk.z);
        static constexpr std::array<template_rotation, 4> rotations{
            template_rotation::none, template_rotation::clockwise_90,
            template_rotation::clockwise_180,
            template_rotation::counterclockwise_90};
        const auto rotation =
            rotations[static_cast<std::size_t>(random.next_int(4))];
        jint offset_x = 5;
        jint offset_z = 5;
        if (rotation == template_rotation::clockwise_90) {
            offset_x = -5;
        } else if (rotation == template_rotation::clockwise_180) {
            offset_x = -5;
            offset_z = -5;
        } else if (rotation == template_rotation::counterclockwise_90) {
            offset_z = -5;
        }
        // `height_at` returns the first free height; the end city anchors
        // the lowest first-occupied corner.
        const jint y = std::min(
            std::min(context.height_at(block_x, block_z,
                                       heightmap_kind::world_surface_wg) -
                         1,
                     context.height_at(block_x, block_z + offset_z,
                                       heightmap_kind::world_surface_wg) -
                         1),
            std::min(context.height_at(block_x + offset_x, block_z,
                                       heightmap_kind::world_surface_wg) -
                         1,
                     context.height_at(block_x + offset_x, block_z + offset_z,
                                       heightmap_kind::world_surface_wg) -
                         1));
        if (y < 60 || !biome_allowed(context, structure, block_x, y, block_z)) {
            return std::nullopt;
        }
        auto start = end_city_start(
            *context.templates, block_pos{.x = block_x, .y = y, .z = block_z},
            rotation, random);
        if (!start) {
            return std::nullopt;
        }
        start->structure = structure.id;
        start->step = structure.step;
        start->chunk = chunk;
        return *start;
    }
    if (structure.type == "minecraft:desert_pyramid") {
        if (!biome_column_allowed(context, structure, chunk.x * 16 + 8,
                                  chunk.z * 16 + 8)) {
            return std::nullopt;
        }
        const jint west = chunk.x * 16;
        const jint north = chunk.z * 16;
        const auto height = [&](jint x, jint z) {
            return context.height_at(x, z, heightmap_kind::world_surface_wg);
        };
        const jint lowest = std::min(
            std::min(height(west, north), height(west, north + 21)),
            std::min(height(west + 21, north), height(west + 21, north + 21)));
        if (lowest < context.sea_level) {
            return std::nullopt;
        }
        const jint middle_x = west + 8;
        const jint middle_z = north + 8;
        const jint middle_y = height(middle_x, middle_z);
        if (!biome_allowed(context, structure, middle_x, middle_y, middle_z)) {
            return std::nullopt;
        }
        auto start = desert_pyramid_start(chunk, context.seed);
        start.structure = structure.id;
        start.step = structure.step;
        return start;
    }
    if (structure.type == "minecraft:jungle_temple") {
        return surface_start(context, structure, chunk, 12, 10, 15, true);
    }
    return std::nullopt;
}

} // namespace

auto generate_structure_area(const chunk_pos &target,
                             const structure_start_context &context,
                             jint radius) -> structure_area {
    structure_area area;
    for (jint dx = -radius; dx <= radius; ++dx) {
        for (jint dz = -radius; dz <= radius; ++dz) {
            const chunk_pos chunk{.x = target.x + dx, .z = target.z + dz};
            auto starts = generate_structure_starts(chunk, context);
            area.starts.insert(area.starts.end(),
                               std::make_move_iterator(starts.begin()),
                               std::make_move_iterator(starts.end()));
        }
    }
    area.references = collect_references(area.starts, target, radius);
    return area;
}

auto generate_structure_starts(const chunk_pos &chunk,
                               const structure_start_context &context)
    -> std::vector<structure_start> {
    std::vector<structure_start> starts;
    if (context.data == nullptr) {
        return starts;
    }
    for (const auto &[set_id, set] : context.data->sets) {
        (void)set_id;
        bool set_candidate = false;
        if (set.random_spread.has_value()) {
            set_candidate = set.random_spread->is_structure_chunk(
                context.seed, chunk.x, chunk.z);
        } else if (set.concentric_rings.has_value()) {
            const auto positions = concentric_ring_positions(
                context.seed, *set.concentric_rings, context);
            set_candidate = std::find(positions.begin(), positions.end(),
                                      chunk) != positions.end();
        }
        if (!set_candidate) {
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
            record.pieces.push_back(to_piece_record(piece));
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

auto locate_structure(std::string_view structure_id, const chunk_pos &origin,
                      jint max_radius, const structure_start_context &context)
    -> std::optional<located_structure> {
    if (context.data == nullptr || max_radius < 0) {
        return std::nullopt;
    }
    if (context.data->structures.find(std::string{structure_id}) ==
        context.data->structures.end()) {
        return std::nullopt;
    }
    for (jint radius = 0; radius <= max_radius; ++radius) {
        for (jint dz = -radius; dz <= radius; ++dz) {
            for (jint dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != radius) {
                    continue;
                }
                const chunk_pos chunk{.x = origin.x + dx, .z = origin.z + dz};
                const auto starts = generate_structure_starts(chunk, context);
                for (const auto &start : starts) {
                    if (start.structure == structure_id) {
                        return located_structure{.chunk = chunk,
                                                 .structure = start.structure};
                    }
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace refrain::worldgen::structure
