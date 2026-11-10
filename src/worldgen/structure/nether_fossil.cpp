// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/nether_fossil.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace refrain::worldgen::structure {

namespace {

constexpr std::array<std::string_view, 14> fossil_templates{{
    "minecraft:nether_fossils/fossil_1",
    "minecraft:nether_fossils/fossil_2",
    "minecraft:nether_fossils/fossil_3",
    "minecraft:nether_fossils/fossil_4",
    "minecraft:nether_fossils/fossil_5",
    "minecraft:nether_fossils/fossil_6",
    "minecraft:nether_fossils/fossil_7",
    "minecraft:nether_fossils/fossil_8",
    "minecraft:nether_fossils/fossil_9",
    "minecraft:nether_fossils/fossil_10",
    "minecraft:nether_fossils/fossil_11",
    "minecraft:nether_fossils/fossil_12",
    "minecraft:nether_fossils/fossil_13",
    "minecraft:nether_fossils/fossil_14",
}};

auto is_air(const nether_fossil_generation_context &context,
            block_state_id state) -> bool {
    if (context.states == nullptr) {
        return false;
    }
    const auto traits = context.states->traits(state);
    return is_air_block_name(state_block_name(context.states->name(state))) ||
           traits.air;
}

auto supports_fossil(const nether_fossil_generation_context &context,
                     block_state_id state) -> bool {
    if (context.states == nullptr) {
        return false;
    }
    const auto name = state_block_name(context.states->name(state));
    if (name == "minecraft:soul_sand") {
        return true;
    }
    const auto traits = context.states->traits(state);
    return !traits.air && !traits.fluid;
}

} // namespace

auto nether_fossil_start(const chunk_pos &chunk,
                         const nether_fossil_generation_context &context)
    -> std::optional<structure_start> {
    if (context.structure == nullptr || context.templates == nullptr ||
        !context.block_at) {
        return std::nullopt;
    }
    const auto *height = context.structure->config.find("height");
    if (height == nullptr) {
        return std::nullopt;
    }
    const auto provider = decode_height_provider(*height);
    if (!provider) {
        return std::nullopt;
    }
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(context.seed, chunk.x, chunk.z);
    const jint block_x = chunk.x * 16 + random.next_int(16);
    const jint block_z = chunk.z * 16 + random.next_int(16);
    const jint provider_min = context.generation_height > 0
                                  ? context.generation_min_y
                                  : context.min_y;
    const jint provider_height = context.generation_height > 0
                                     ? context.generation_height
                                     : context.world_height;
    jint y = provider->sample(random, provider_min, provider_height,
                              context.sea_level);
    while (y > context.sea_level) {
        const auto current = context.block_at(block_x, y, block_z);
        const auto below = context.block_at(block_x, y - 1, block_z);
        --y;
        if (is_air(context, current) && supports_fossil(context, below)) {
            break;
        }
    }
    if (y <= context.sea_level) {
        return std::nullopt;
    }
    const block_pos position{.x = block_x, .y = y, .z = block_z};
    if (context.valid_biome && !context.valid_biome(position)) {
        return std::nullopt;
    }
    const auto rotation = static_cast<template_rotation>(random.next_int(4));
    const auto *template_id = &fossil_templates[static_cast<std::size_t>(
        random.next_int(static_cast<jint>(fossil_templates.size())))];
    const auto &templ = context.templates->get_or_create(*template_id);
    if (templ.palettes.empty()) {
        return std::nullopt;
    }
    const template_transform transform{
        .mirror = template_mirror::none,
        .rotation = rotation,
        .pivot = block_pos{},
    };
    const auto bounds = template_bounding_box(templ, transform, position);
    structure_piece piece{
        .type = "minecraft:nether_fossil",
        .bounds = structure_bbox{.min_x = bounds.min_x,
                                 .min_y = bounds.min_y,
                                 .min_z = bounds.min_z,
                                 .max_x = bounds.max_x,
                                 .max_y = bounds.max_y,
                                 .max_z = bounds.max_z},
        .orientation = 2,
        .template_id = std::string{*template_id},
        .template_origin = position,
        .template_rotation = static_cast<jint>(rotation),
        .template_mirror = 0,
        .template_pivot = block_pos{},
    };
    structure_start start{
        .structure = context.structure->id,
        .step = context.structure->step,
        .chunk = chunk,
        .pieces = {std::move(piece)},
        .references = 0,
        .max_references = 1,
        .bounds = {},
    };
    start.bounds = start.pieces.front().bounds;
    return start;
}

} // namespace refrain::worldgen::structure
