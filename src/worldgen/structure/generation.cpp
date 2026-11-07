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

auto ocean_ruin_floor_height(chunk_region &region,
                             const block_state_registry &states, jint min_y,
                             const block_pos &position, const block_pos &corner)
    -> jint {
    const jint top_y = position.y - 1;
    jint lowest = 512;
    jint area = 0;
    const jint x0 = std::min(position.x, corner.x);
    const jint x1 = std::max(position.x, corner.x);
    const jint z0 = std::min(position.z, corner.z);
    const jint z1 = std::max(position.z, corner.z);
    for (jint x = x0; x <= x1; ++x) {
        for (jint z = z0; z <= z1; ++z) {
            jint floor_y = top_y;
            auto state = region.state_at(x, floor_y, z);
            const auto is_open = [&](block_state_id id) {
                const auto name = std::string_view{states.name(id)};
                return states.traits(id).air || name == "minecraft:water" ||
                       name == "minecraft:ice";
            };
            while (floor_y > min_y + 1 && is_open(state)) {
                --floor_y;
                state = region.state_at(x, floor_y, z);
            }
            lowest = std::min(lowest, floor_y);
            if (floor_y < top_y - 2) {
                ++area;
            }
        }
    }
    const jint width = std::abs(position.x - corner.x);
    if (top_y - lowest > 2 && area > width - 2) {
        return lowest + 1;
    }
    return position.y;
}

auto shipwreck_start(
    const chunk_pos &chunk, jlong seed, structure_template_manager &templates,
    bool beached,
    const std::function<jint(jint, jint, heightmap_kind)> &height_at)
    -> structure_start {
    static constexpr std::array<const char *, 11> beached_ids{
        "minecraft:shipwreck/with_mast",
        "minecraft:shipwreck/sideways_full",
        "minecraft:shipwreck/sideways_fronthalf",
        "minecraft:shipwreck/sideways_backhalf",
        "minecraft:shipwreck/rightsideup_full",
        "minecraft:shipwreck/rightsideup_fronthalf",
        "minecraft:shipwreck/rightsideup_backhalf",
        "minecraft:shipwreck/with_mast_degraded",
        "minecraft:shipwreck/rightsideup_full_degraded",
        "minecraft:shipwreck/rightsideup_fronthalf_degraded",
        "minecraft:shipwreck/rightsideup_backhalf_degraded"};
    static constexpr std::array<const char *, 20> ocean_ids{
        "minecraft:shipwreck/with_mast",
        "minecraft:shipwreck/upsidedown_full",
        "minecraft:shipwreck/upsidedown_fronthalf",
        "minecraft:shipwreck/upsidedown_backhalf",
        "minecraft:shipwreck/sideways_full",
        "minecraft:shipwreck/sideways_fronthalf",
        "minecraft:shipwreck/sideways_backhalf",
        "minecraft:shipwreck/rightsideup_full",
        "minecraft:shipwreck/rightsideup_fronthalf",
        "minecraft:shipwreck/rightsideup_backhalf",
        "minecraft:shipwreck/with_mast_degraded",
        "minecraft:shipwreck/upsidedown_full_degraded",
        "minecraft:shipwreck/upsidedown_fronthalf_degraded",
        "minecraft:shipwreck/upsidedown_backhalf_degraded",
        "minecraft:shipwreck/sideways_full_degraded",
        "minecraft:shipwreck/sideways_fronthalf_degraded",
        "minecraft:shipwreck/sideways_backhalf_degraded",
        "minecraft:shipwreck/rightsideup_full_degraded",
        "minecraft:shipwreck/rightsideup_fronthalf_degraded",
        "minecraft:shipwreck/rightsideup_backhalf_degraded"};
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    const jint rotation_index = random.next_int(4);
    const auto rotation = static_cast<template_rotation>(rotation_index);
    const char *const *ids = beached ? beached_ids.data() : ocean_ids.data();
    const std::size_t id_count =
        beached ? beached_ids.size() : ocean_ids.size();
    const auto index =
        static_cast<std::size_t>(random.next_int(static_cast<jint>(id_count)));
    const auto &templ = templates.get_or_create(ids[index]);
    const template_transform transform{
        .mirror = template_mirror::none,
        .rotation = rotation,
        .pivot = block_pos{.x = 4, .y = 0, .z = 15},
    };
    block_pos origin{.x = chunk.x * 16, .y = 90, .z = chunk.z * 16};
    auto bounds = template_bounding_box(templ, transform, origin);
    const heightmap_kind kind = beached ? heightmap_kind::world_surface_wg
                                        : heightmap_kind::ocean_floor_wg;
    if (templ.size_x > 32 || templ.size_y > 32) {
        const std::array<std::pair<jint, jint>, 4> corners{{
            {bounds.min_x, bounds.min_z},
            {bounds.min_x, bounds.max_z + 1},
            {bounds.max_x + 1, bounds.min_z},
            {bounds.max_x + 1, bounds.max_z + 1},
        }};
        if (beached) {
            jint lowest = 1 << 30;
            for (const auto &[x, z] : corners) {
                lowest = std::min(lowest, height_at(x, z, kind));
            }
            origin.y = lowest - templ.size_y / 2 - random.next_int(3);
        } else {
            jint total = 0;
            for (const auto &[x, z] : corners) {
                total += height_at(x, z, kind);
            }
            origin.y = total / 4;
        }
        bounds = template_bounding_box(templ, transform, origin);
    }
    structure_start start{.structure = "minecraft:shipwreck",
                          .chunk = chunk,
                          .references = 0,
                          .max_references = 1,
                          .bounds = {.min_x = bounds.min_x,
                                     .min_y = bounds.min_y,
                                     .min_z = bounds.min_z,
                                     .max_x = bounds.max_x,
                                     .max_y = bounds.max_y,
                                     .max_z = bounds.max_z}};
    start.pieces.push_back(structure_piece{
        .type = "minecraft:shipwreck",
        .bounds = start.bounds,
        .orientation = 2,
        .template_id = std::string{ids[index]},
        .template_origin = origin,
        .template_rotation = rotation_index,
        .template_mirror = 0,
        .template_pivot = block_pos{.x = 4, .y = 0, .z = 15},
    });
    return start;
}

namespace {

struct ocean_ruin_builder {
    structure_start start{};
    structure_template_manager *templates{};
    bool warm{true};
    worldgen_random *random{};

    void add(std::string_view id, const block_pos &origin, jint rotation,
             float integrity, bool large) {
        const auto &templ = templates->get_or_create(id);
        const template_transform transform{
            .mirror = template_mirror::none,
            .rotation = static_cast<template_rotation>(rotation),
            .pivot = block_pos{},
        };
        const auto bounds = template_bounding_box(templ, transform, origin);
        structure_piece piece{
            .type = "minecraft:ocean_ruin",
            .bounds = {.min_x = bounds.min_x,
                       .min_y = bounds.min_y,
                       .min_z = bounds.min_z,
                       .max_x = bounds.max_x,
                       .max_y = bounds.max_y,
                       .max_z = bounds.max_z},
            .orientation = 2,
            .template_id = std::string{id},
            .template_origin = origin,
            .template_rotation = rotation,
            .template_mirror = 0,
            .template_pivot = block_pos{},
            .integrity = integrity,
            .large = large,
        };
        if (start.pieces.empty()) {
            start.bounds = {.min_x = bounds.min_x,
                            .min_y = bounds.min_y,
                            .min_z = bounds.min_z,
                            .max_x = bounds.max_x,
                            .max_y = bounds.max_y,
                            .max_z = bounds.max_z};
        } else {
            start.bounds.min_x = std::min(start.bounds.min_x, bounds.min_x);
            start.bounds.min_y = std::min(start.bounds.min_y, bounds.min_y);
            start.bounds.min_z = std::min(start.bounds.min_z, bounds.min_z);
            start.bounds.max_x = std::max(start.bounds.max_x, bounds.max_x);
            start.bounds.max_y = std::max(start.bounds.max_y, bounds.max_y);
            start.bounds.max_z = std::max(start.bounds.max_z, bounds.max_z);
        }
        start.pieces.push_back(std::move(piece));
    }
};

} // namespace

auto ocean_ruin_start(const chunk_pos &chunk, jlong seed,
                      structure_template_manager &templates, bool warm,
                      float large_probability, float cluster_probability)
    -> structure_start {
    static constexpr std::array<const char *, 8> warm_small{
        "minecraft:underwater_ruin/warm_1", "minecraft:underwater_ruin/warm_2",
        "minecraft:underwater_ruin/warm_3", "minecraft:underwater_ruin/warm_4",
        "minecraft:underwater_ruin/warm_5", "minecraft:underwater_ruin/warm_6",
        "minecraft:underwater_ruin/warm_7", "minecraft:underwater_ruin/warm_8",
    };
    static constexpr std::array<const char *, 4> warm_large{
        "minecraft:underwater_ruin/big_warm_4",
        "minecraft:underwater_ruin/big_warm_5",
        "minecraft:underwater_ruin/big_warm_6",
        "minecraft:underwater_ruin/big_warm_7",
    };
    static constexpr std::array<const char *, 8> cold_bricks{
        "minecraft:underwater_ruin/brick_1",
        "minecraft:underwater_ruin/brick_2",
        "minecraft:underwater_ruin/brick_3",
        "minecraft:underwater_ruin/brick_4",
        "minecraft:underwater_ruin/brick_5",
        "minecraft:underwater_ruin/brick_6",
        "minecraft:underwater_ruin/brick_7",
        "minecraft:underwater_ruin/brick_8",
    };
    static constexpr std::array<const char *, 8> cold_cracked{
        "minecraft:underwater_ruin/cracked_1",
        "minecraft:underwater_ruin/cracked_2",
        "minecraft:underwater_ruin/cracked_3",
        "minecraft:underwater_ruin/cracked_4",
        "minecraft:underwater_ruin/cracked_5",
        "minecraft:underwater_ruin/cracked_6",
        "minecraft:underwater_ruin/cracked_7",
        "minecraft:underwater_ruin/cracked_8",
    };
    static constexpr std::array<const char *, 8> cold_mossy{
        "minecraft:underwater_ruin/mossy_1",
        "minecraft:underwater_ruin/mossy_2",
        "minecraft:underwater_ruin/mossy_3",
        "minecraft:underwater_ruin/mossy_4",
        "minecraft:underwater_ruin/mossy_5",
        "minecraft:underwater_ruin/mossy_6",
        "minecraft:underwater_ruin/mossy_7",
        "minecraft:underwater_ruin/mossy_8",
    };
    static constexpr std::array<const char *, 4> big_bricks{
        "minecraft:underwater_ruin/big_brick_1",
        "minecraft:underwater_ruin/big_brick_2",
        "minecraft:underwater_ruin/big_brick_3",
        "minecraft:underwater_ruin/big_brick_8",
    };
    static constexpr std::array<const char *, 4> big_cracked{
        "minecraft:underwater_ruin/big_cracked_1",
        "minecraft:underwater_ruin/big_cracked_2",
        "minecraft:underwater_ruin/big_cracked_3",
        "minecraft:underwater_ruin/big_cracked_8",
    };
    static constexpr std::array<const char *, 4> big_mossy{
        "minecraft:underwater_ruin/big_mossy_1",
        "minecraft:underwater_ruin/big_mossy_2",
        "minecraft:underwater_ruin/big_mossy_3",
        "minecraft:underwater_ruin/big_mossy_8",
    };
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    ocean_ruin_builder builder{
        .templates = &templates, .warm = warm, .random = &random};
    builder.start.structure = "minecraft:ocean_ruin";
    builder.start.chunk = chunk;
    builder.start.references = 0;
    builder.start.max_references = 1;
    const auto next_int = [&](jint min, jint max) {
        return min + random.next_int(max - min + 1);
    };
    const auto add_piece = [&](const block_pos &origin, jint rotation,
                               bool large) {
        const float integrity = large ? 0.9F : 0.8F;
        if (warm) {
            const char *const *ids =
                large ? warm_large.data() : warm_small.data();
            const std::size_t count =
                large ? warm_large.size() : warm_small.size();
            const auto index = static_cast<std::size_t>(
                random.next_int(static_cast<jint>(count)));
            builder.add(ids[index], origin, rotation, integrity, large);
        } else {
            const std::size_t index =
                static_cast<std::size_t>(random.next_int(8));
            if (large) {
                builder.add(big_bricks[index], origin, rotation, integrity,
                            large);
                builder.add(big_cracked[index], origin, rotation, 0.7F, large);
                builder.add(big_mossy[index], origin, rotation, 0.5F, large);
            } else {
                builder.add(cold_bricks[index], origin, rotation, integrity,
                            large);
                builder.add(cold_cracked[index], origin, rotation, 0.7F, large);
                builder.add(cold_mossy[index], origin, rotation, 0.5F, large);
            }
        }
    };
    const jint rotation = random.next_int(4);
    const block_pos offset{.x = chunk.x * 16, .y = 90, .z = chunk.z * 16};
    const bool large = random.next_float() <= large_probability;
    add_piece(offset, rotation, large);
    if (large && random.next_float() <= cluster_probability) {
        const auto transform_corner = [&](jint x, jint z, jint rot) {
            return transform_position(
                block_pos{.x = x, .y = 0, .z = z},
                template_transform{
                    .mirror = template_mirror::none,
                    .rotation = static_cast<template_rotation>(rot),
                    .pivot = block_pos{},
                });
        };
        const auto parent_corner = transform_corner(15, 15, rotation);
        const block_pos parent_pos = offset;
        const block_pos parent_corner_pos{.x = parent_pos.x + parent_corner.x,
                                          .y = parent_pos.y,
                                          .z = parent_pos.z + parent_corner.z};
        const template_bounds parent_bb{
            .min_x = std::min(parent_pos.x, parent_corner_pos.x),
            .min_y = parent_pos.y,
            .min_z = std::min(parent_pos.z, parent_corner_pos.z),
            .max_x = std::max(parent_pos.x, parent_corner_pos.x),
            .max_y = parent_pos.y,
            .max_z = std::max(parent_pos.z, parent_corner_pos.z)};
        const block_pos parent_bottom_left{
            .x = parent_bb.min_x, .y = parent_pos.y, .z = parent_bb.min_z};
        std::vector<block_pos> positions;
        positions.push_back(
            {.x = parent_bottom_left.x + (-16 + next_int(1, 8)),
             .y = parent_bottom_left.y,
             .z = parent_bottom_left.z + (16 + next_int(1, 7))});
        positions.push_back({.x = parent_bottom_left.x + (-16 + next_int(1, 8)),
                             .y = parent_bottom_left.y,
                             .z = parent_bottom_left.z + next_int(1, 7)});
        positions.push_back(
            {.x = parent_bottom_left.x + (-16 + next_int(1, 8)),
             .y = parent_bottom_left.y,
             .z = parent_bottom_left.z + (-16 + next_int(4, 8))});
        positions.push_back(
            {.x = parent_bottom_left.x + next_int(1, 7),
             .y = parent_bottom_left.y,
             .z = parent_bottom_left.z + (16 + next_int(1, 7))});
        positions.push_back(
            {.x = parent_bottom_left.x + next_int(1, 7),
             .y = parent_bottom_left.y,
             .z = parent_bottom_left.z + (-16 + next_int(4, 6))});
        positions.push_back(
            {.x = parent_bottom_left.x + (16 + next_int(1, 7)),
             .y = parent_bottom_left.y,
             .z = parent_bottom_left.z + (16 + next_int(3, 8))});
        positions.push_back({.x = parent_bottom_left.x + (16 + next_int(1, 7)),
                             .y = parent_bottom_left.y,
                             .z = parent_bottom_left.z + next_int(1, 7)});
        positions.push_back(
            {.x = parent_bottom_left.x + (16 + next_int(1, 7)),
             .y = parent_bottom_left.y,
             .z = parent_bottom_left.z + (-16 + next_int(4, 8))});
        const jint ruins = 4 + random.next_int(4);
        for (jint i = 0; i < ruins; ++i) {
            if (positions.empty()) {
                break;
            }
            const auto index = static_cast<std::size_t>(
                random.next_int(static_cast<jint>(positions.size())));
            const auto pos = positions[index];
            positions.erase(positions.begin() +
                            static_cast<std::ptrdiff_t>(index));
            const jint next_rotation = random.next_int(4);
            const auto next_corner = transform_corner(5, 6, next_rotation);
            const template_bounds next_bb{
                .min_x = std::min(pos.x, pos.x + next_corner.x),
                .min_y = pos.y,
                .min_z = std::min(pos.z, pos.z + next_corner.z),
                .max_x = std::max(pos.x, pos.x + next_corner.x),
                .max_y = pos.y,
                .max_z = std::max(pos.z, pos.z + next_corner.z)};
            if (next_bb.min_x <= parent_bb.max_x &&
                next_bb.max_x >= parent_bb.min_x &&
                next_bb.min_z <= parent_bb.max_z &&
                next_bb.max_z >= parent_bb.min_z) {
                continue;
            }
            add_piece(pos, next_rotation, false);
        }
    }
    return builder.start;
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
