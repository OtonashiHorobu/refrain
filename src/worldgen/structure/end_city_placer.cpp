// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// End city template placement. Pieces ignore the structure block and, for the
// non-overwriting floor pieces, air as well. After the template is written its
// data markers seed the chest block entities below the marker positions.

#include <refrain/worldgen/structure/placement_stage.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace refrain::worldgen::structure {

namespace {

auto nbt_string(const data::nbt::compound &entries, std::string_view key)
    -> std::optional<std::string> {
    const auto found = entries.find(key);
    if (found == entries.end()) {
        return std::nullopt;
    }
    const auto text = found->second.as_string();
    if (!text) {
        return std::nullopt;
    }
    return std::string{*text};
}

auto inside_chunk(const proto_chunk &chunk, const block_pos &pos) -> bool {
    return pos.x >= chunk.chunk_x() * 16 &&
           pos.x <= chunk.chunk_x() * 16 + 15 &&
           pos.z >= chunk.chunk_z() * 16 &&
           pos.z <= chunk.chunk_z() * 16 + 15 && pos.y >= chunk.min_y() &&
           pos.y < chunk.min_y() + chunk.height();
}

} // namespace

void place_end_city_piece(const structure_place_context &context,
                          const structure_piece_record &piece,
                          worldgen_random &random) {
    // TODO: read the end city loot table from the structure document instead
    // of naming it here; the reference structure hard-codes the id, so the
    // structure document has no field for it yet.
    if (context.templates == nullptr || context.registry == nullptr ||
        context.chunk == nullptr || context.region == nullptr) {
        return;
    }
    const auto &templ = context.templates->get_or_create(piece.template_id);
    if (templ.palettes.empty()) {
        return;
    }
    const auto transform = template_transform{
        .mirror = template_mirror::none,
        .rotation = static_cast<template_rotation>(piece.template_rotation),
        .pivot = block_pos{},
    };
    block_ignore_processor ignore{
        piece.overwrite ? std::vector<std::string>{"minecraft:structure_block"}
                        : std::vector<std::string>{
                              "minecraft:air", "minecraft:structure_block"}};
    template_place_settings settings{};
    settings.transform = transform;
    settings.bounds = template_bounds{
        .min_x = context.chunk->chunk_x() * 16,
        .min_y = context.chunk->min_y(),
        .min_z = context.chunk->chunk_z() * 16,
        .max_x = context.chunk->chunk_x() * 16 + 15,
        .max_y = context.chunk->min_y() + context.chunk->height() - 1,
        .max_z = context.chunk->chunk_z() * 16 + 15,
    };
    settings.processors = {&ignore};
    settings.liquids = liquid_settings::apply_waterlogging;
    settings.level_seed = context.seed;
    settings.loot_random = &random;
    const block_pos origin{
        .x = piece.template_x, .y = piece.template_y, .z = piece.template_z};
    (void)place_template(*context.region, *context.registry, templ, origin,
                         settings);

    std::size_t palette_index = 0;
    if (templ.palettes.size() > 1) {
        legacy_random_source local{mth_get_seed(origin.x, origin.y, origin.z)};
        palette_index = static_cast<std::size_t>(
            local.next_int(static_cast<jint>(templ.palettes.size())));
    }
    for (const auto &block : templ.palettes[palette_index]) {
        if (block.nbt == nullptr) {
            continue;
        }
        if (state_block_name(context.registry->name(block.state)) !=
            "minecraft:structure_block") {
            continue;
        }
        const auto mode = nbt_string(*block.nbt, "mode");
        if (!mode.has_value() || (*mode != "DATA" && *mode != "data")) {
            continue;
        }
        const auto metadata = nbt_string(*block.nbt, "metadata");
        if (!metadata.has_value() || !metadata->starts_with("Chest")) {
            continue;
        }
        const auto relative = transform_position(
            block_pos{.x = block.x, .y = block.y, .z = block.z}, transform);
        const block_pos marker{.x = origin.x + relative.x,
                               .y = origin.y + relative.y,
                               .z = origin.z + relative.z};
        const block_pos chest{.x = marker.x, .y = marker.y - 1, .z = marker.z};
        if (!inside_chunk(*context.chunk, chest)) {
            continue;
        }
        if (state_block_name(context.states->name(context.region->state_at(
                chest.x, chest.y, chest.z))) != "minecraft:chest") {
            continue;
        }
        data::nbt::compound entity;
        entity.emplace("id", data::nbt::value{std::string{"minecraft:chest"}});
        entity.emplace("x", data::nbt::value{chest.x});
        entity.emplace("y", data::nbt::value{chest.y});
        entity.emplace("z", data::nbt::value{chest.z});
        entity.emplace("LootTable",
                       data::nbt::value{std::string{"minecraft:chests/"
                                                    "end_city_treasure"}});
        entity.emplace("LootTableSeed", data::nbt::value{random.next_long()});
        entity.emplace("components", data::nbt::value{data::nbt::compound{}});
        entity.emplace("keepPacked",
                       data::nbt::value{static_cast<std::int8_t>(0)});
        context.chunk->set_block_entity(
            block_pos{.x = chest.x & 15, .y = chest.y, .z = chest.z & 15},
            std::move(entity));
    }
}

} // namespace refrain::worldgen::structure
