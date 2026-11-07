// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/generation.hpp>

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <array>

#include <algorithm>

namespace refrain::worldgen::structure {

auto buried_treasure_start(const chunk_pos &chunk) -> structure_start {
    const structure_bbox bounds{.min_x = chunk.x * 16 + 9,
                                .min_y = 90,
                                .min_z = chunk.z * 16 + 9,
                                .max_x = chunk.x * 16 + 9,
                                .max_y = 90,
                                .max_z = chunk.z * 16 + 9};
    structure_start start{.structure = "minecraft:buried_treasure",
                          .chunk = chunk,
                          .references = 0,
                          .max_references = 1,
                          .bounds = bounds};
    start.pieces.push_back(
        structure_piece{.type = "minecraft:btp", .bounds = bounds});
    return start;
}

auto igloo_start(const chunk_pos &chunk, jlong seed,
                 structure_template_manager &templates) -> structure_start {
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    const jint rotation = random.next_int(4);
    const auto transform_for = [&](const block_pos &pivot) {
        return template_transform{.mirror = template_mirror::none,
                                  .rotation =
                                      static_cast<template_rotation>(rotation),
                                  .pivot = pivot};
    };
    const block_pos start_pos{.x = chunk.x * 16, .y = 90, .z = chunk.z * 16};
    structure_start start{
        .structure = "minecraft:igloo",
        .chunk = chunk,
        .max_references = 1,
    };
    const auto add_piece = [&](std::string_view template_id,
                               const block_pos &origin,
                               const block_pos &pivot) {
        const auto &templ = templates.get_or_create(template_id);
        const auto transform = transform_for(pivot);
        const auto bounds = template_bounding_box(templ, transform, origin);
        start.pieces.push_back(structure_piece{
            .type = "minecraft:igl",
            .bounds = structure_bbox{.min_x = bounds.min_x,
                                     .min_y = bounds.min_y,
                                     .min_z = bounds.min_z,
                                     .max_x = bounds.max_x,
                                     .max_y = bounds.max_y,
                                     .max_z = bounds.max_z},
            .orientation = 2,
            .template_id = std::string{template_id},
            .template_origin = origin,
            .template_rotation = rotation,
            .template_mirror = 0,
            .template_pivot = pivot,
        });
    };
    if (random.next_double() < 0.5) {
        const jint depth = random.next_int(8) + 4;
        add_piece("minecraft:igloo/bottom",
                  block_pos{.x = start_pos.x,
                            .y = start_pos.y - 3 - depth * 3,
                            .z = start_pos.z - 2},
                  block_pos{.x = 3, .y = 6, .z = 7});
        for (jint i = 0; i < depth - 1; ++i) {
            add_piece("minecraft:igloo/middle",
                      block_pos{.x = start_pos.x + 2,
                                .y = start_pos.y - 3 - i * 3,
                                .z = start_pos.z + 4},
                      block_pos{.x = 1, .y = 3, .z = 1});
        }
    }
    add_piece("minecraft:igloo/top", start_pos,
              block_pos{.x = 3, .y = 5, .z = 5});
    start.bounds = start.pieces.front().bounds;
    for (const auto &piece : start.pieces) {
        start.bounds.min_x = std::min(start.bounds.min_x, piece.bounds.min_x);
        start.bounds.min_y = std::min(start.bounds.min_y, piece.bounds.min_y);
        start.bounds.min_z = std::min(start.bounds.min_z, piece.bounds.min_z);
        start.bounds.max_x = std::max(start.bounds.max_x, piece.bounds.max_x);
        start.bounds.max_y = std::max(start.bounds.max_y, piece.bounds.max_y);
        start.bounds.max_z = std::max(start.bounds.max_z, piece.bounds.max_z);
    }
    return start;
}

auto desert_pyramid_start(const chunk_pos &chunk, jlong seed)
    -> structure_start {
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    static constexpr std::array<jint, 4> orientations{2, 3, 0, 1};
    const jint direction =
        orientations[static_cast<std::size_t>(random.next_int(4))];
    const structure_bbox bounds{.min_x = chunk.x * 16,
                                .min_y = 64,
                                .min_z = chunk.z * 16,
                                .max_x = chunk.x * 16 + 20,
                                .max_y = 64 + 14,
                                .max_z = chunk.z * 16 + 20};
    structure_start start{.structure = "minecraft:desert_pyramid",
                          .chunk = chunk,
                          .references = 0,
                          .max_references = 1,
                          .bounds = bounds};
    start.pieces.push_back(structure_piece{
        .type = "minecraft:tedp", .bounds = bounds, .orientation = direction});
    return start;
}

auto swamp_hut_start(const chunk_pos &chunk, jlong seed) -> structure_start {
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    // `Direction.Plane.HORIZONTAL` order: north, east, south, west.
    static constexpr std::array<jint, 4> orientations{2, 3, 0, 1};
    const jint direction =
        orientations[static_cast<std::size_t>(random.next_int(4))];
    const jint west = chunk.x * 16;
    const jint north = chunk.z * 16;
    const bool along_z = direction == 2 || direction == 0;
    const structure_bbox bounds{
        .min_x = west,
        .min_y = 64,
        .min_z = north,
        .max_x = west + (along_z ? 6 : 8),
        .max_y = 64 + 6,
        .max_z = north + (along_z ? 8 : 6),
    };
    structure_start start{.structure = "minecraft:swamp_hut",
                          .chunk = chunk,
                          .references = 0,
                          .max_references = 1,
                          .bounds = bounds};
    start.pieces.push_back(structure_piece{
        .type = "minecraft:tesh", .bounds = bounds, .orientation = direction});
    return start;
}

} // namespace refrain::worldgen::structure
