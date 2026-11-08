// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/placement_stage.hpp>

#include <refrain/worldgen/structure/generation.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>

#include <array>
#include <optional>
#include <string>

namespace refrain::worldgen::structure {

namespace {

struct horizontal_direction {
    jint dx;
    jint dz;
    std::string_view name;
};

// `Direction.Plane.HORIZONTAL`: north, east, south, west.
constexpr std::array<horizontal_direction, 4> horizontal{{
    {.dx = 0, .dz = -1, .name = "north"},
    {.dx = 1, .dz = 0, .name = "east"},
    {.dx = 0, .dz = 1, .name = "south"},
    {.dx = -1, .dz = 0, .name = "west"},
}};

struct direction_offset {
    jint dx;
    jint dy;
    jint dz;
};

// `Direction.values()`: down, up, north, south, west, east.
constexpr std::array<direction_offset, 6> all_directions{{
    {.dx = 0, .dy = -1, .dz = 0},
    {.dx = 0, .dy = 1, .dz = 0},
    {.dx = 0, .dy = 0, .dz = -1},
    {.dx = 0, .dy = 0, .dz = 1},
    {.dx = -1, .dy = 0, .dz = 0},
    {.dx = 1, .dy = 0, .dz = 0},
}};

auto block_name(const structure_place_context &context, block_state_id state)
    -> std::string_view {
    return state_block_name(context.states->name(state));
}

auto is_liquid(const structure_place_context &context, block_state_id state)
    -> bool {
    const auto traits = context.states->traits(state);
    return traits.fluid && !traits.air;
}

auto is_solid_render(const structure_place_context &context,
                     block_state_id state) -> bool {
    return context.states->traits(state).blocks_motion;
}

auto opposite(std::size_t index) -> std::size_t { return (index + 2U) % 4U; }

auto clockwise(std::size_t index) -> std::size_t { return (index + 1U) % 4U; }

auto chest_facing(const structure_place_context &context, jint x, jint y,
                  jint z) -> std::string_view {
    std::optional<std::size_t> solid;
    for (std::size_t i = 0; i < horizontal.size(); ++i) {
        const auto &direction = horizontal[i];
        const auto state =
            context.region->state_at(x + direction.dx, y, z + direction.dz);
        if (block_name(context, state) == "minecraft:chest") {
            return "north";
        }
        if (is_solid_render(context, state)) {
            if (solid.has_value()) {
                solid.reset();
                break;
            }
            solid = i;
        }
    }
    if (solid.has_value()) {
        return horizontal[opposite(*solid)].name;
    }
    std::size_t lock = 0; // north
    auto check = [&](std::size_t direction) {
        const auto &entry = horizontal[direction];
        return is_solid_render(
            context, context.region->state_at(x + entry.dx, y, z + entry.dz));
    };
    if (check(lock)) {
        lock = opposite(lock);
    }
    if (check(lock)) {
        lock = clockwise(lock);
    }
    if (check(lock)) {
        lock = opposite(lock);
    }
    return horizontal[lock].name;
}

auto create_chest(const structure_place_context &context, jint x, jint y,
                  jint z, worldgen_random &random) -> bool {
    const auto current = context.region->state_at(x, y, z);
    if (block_name(context, current) == "minecraft:chest") {
        return false;
    }
    const auto facing = chest_facing(context, x, y, z);
    const std::string key =
        "minecraft:chest[facing=" + std::string{facing} + "]";
    const auto chest = context.state(key);
    if (chest == no_block_state ||
        !context.region->set_block_state(x, y, z, chest)) {
        return false;
    }
    data::nbt::compound entity;
    entity.emplace("id", data::nbt::value{std::string{"minecraft:chest"}});
    entity.emplace("x", data::nbt::value{static_cast<jint>(x)});
    entity.emplace("y", data::nbt::value{static_cast<jint>(y)});
    entity.emplace("z", data::nbt::value{static_cast<jint>(z)});
    entity.emplace("LootTable", data::nbt::value{std::string{
                                    "minecraft:chests/buried_treasure"}});
    entity.emplace("LootTableSeed", data::nbt::value{random.next_long()});
    entity.emplace("components", data::nbt::value{data::nbt::compound{}});
    entity.emplace("keepPacked", data::nbt::value{static_cast<std::int8_t>(0)});
    context.chunk->set_block_entity(block_pos{.x = x & 15, .y = y, .z = z & 15},
                                    std::move(entity));
    return true;
}

auto igloo_offset(std::string_view template_id) -> std::optional<block_pos> {
    if (template_id == "minecraft:igloo/top") {
        return block_pos{.x = 0, .y = 0, .z = 0};
    }
    if (template_id == "minecraft:igloo/middle") {
        return block_pos{.x = 2, .y = -3, .z = 4};
    }
    if (template_id == "minecraft:igloo/bottom") {
        return block_pos{.x = 0, .y = -3, .z = -2};
    }
    return std::nullopt;
}

auto swamp_hut_world_pos(const structure_piece_record &piece, jint local_x,
                         jint local_z) -> std::pair<jint, jint> {
    switch (piece.orientation) {
    case 2: // north
        return {piece.min_x + local_x, piece.max_z - local_z};
    case 0: // south
        return {piece.min_x + local_x, piece.min_z + local_z};
    case 1: // west
        return {piece.max_x - local_z, piece.min_z + local_x};
    default: // east
        return {piece.min_x + local_z, piece.min_z + local_x};
    }
}

void place_swamp_hut(const structure_place_context &context,
                     const structure_piece_record &piece) {
    auto &region = *context.region;
    const auto resolve = [&](std::string_view key) {
        return context.state ? context.state(key) : no_block_state;
    };
    structure::template_mirror mirror = structure::template_mirror::none;
    structure::template_rotation rotation = structure::template_rotation::none;
    switch (piece.orientation) {
    case 0: // south
        mirror = structure::template_mirror::left_right;
        break;
    case 1: // west
        mirror = structure::template_mirror::left_right;
        rotation = structure::template_rotation::clockwise_90;
        break;
    case 3: // east
        rotation = structure::template_rotation::clockwise_90;
        break;
    default:
        break;
    }
    const auto orient = [&](block_state_id state) {
        if (context.registry == nullptr) {
            return state;
        }
        return structure::transform_state(*context.registry, state,
                                          structure::template_transform{
                                              .mirror = mirror,
                                              .rotation = rotation,
                                              .pivot = block_pos{},
                                          });
    };
    const auto name_of = [&](const block_pos &pos) {
        return state_block_name(
            context.states->name(region.state_at(pos.x, pos.y, pos.z)));
    };
    jint height_position = piece.height_position;
    if (height_position < 0) {
        jint total = 0;
        jint count = 0;
        for (jint z = piece.min_z; z <= piece.max_z; ++z) {
            for (jint x = piece.min_x; x <= piece.max_x; ++x) {
                if (x >> 4 != context.chunk->chunk_x() ||
                    z >> 4 != context.chunk->chunk_z()) {
                    continue;
                }
                total += context.chunk->first_available(
                    heightmap_kind::motion_blocking_no_leaves, x & 15, z & 15);
                ++count;
            }
        }
        if (count == 0) {
            return;
        }
        height_position = total / count;
    }
    const jint base_y = height_position + piece.height_offset;
    const auto local_pos = [&](jint x, jint y, jint z) {
        return piece_local_to_world(piece, base_y, x, y, z);
    };
    const auto place = [&](std::string_view key, jint x, jint y, jint z) {
        auto state = resolve(key);
        if (state == no_block_state) {
            return;
        }
        state = orient(state);
        const auto pos = local_pos(x, y, z);
        (void)region.set_block_state(pos.x, pos.y, pos.z, state);
    };
    const auto box = [&](jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
                         std::string_view state) {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    place(state, x, y, z);
                }
            }
        }
    };
    constexpr std::string_view spruce = "minecraft:spruce_planks";
    constexpr std::string_view log = "minecraft:oak_log[axis=y]";
    box(1, 1, 1, 5, 1, 7, spruce);
    box(1, 4, 2, 5, 4, 7, spruce);
    box(2, 1, 0, 4, 1, 0, spruce);
    box(2, 2, 2, 3, 3, 2, spruce);
    box(1, 2, 3, 1, 3, 6, spruce);
    box(5, 2, 3, 5, 3, 6, spruce);
    box(2, 2, 7, 4, 3, 7, spruce);
    box(1, 0, 2, 1, 3, 2, log);
    box(5, 0, 2, 5, 3, 2, log);
    box(1, 0, 7, 1, 3, 7, log);
    box(5, 0, 7, 5, 3, 7, log);
    place("minecraft:oak_fence", 2, 3, 2);
    place("minecraft:oak_fence", 3, 3, 7);
    place("minecraft:air", 1, 3, 4);
    place("minecraft:air", 5, 3, 4);
    place("minecraft:air", 5, 3, 5);
    place("minecraft:potted_red_mushroom", 1, 3, 5);
    place("minecraft:crafting_table", 3, 2, 6);
    place("minecraft:cauldron", 4, 2, 6);
    place("minecraft:oak_fence", 1, 2, 1);
    place("minecraft:oak_fence", 5, 2, 1);
    const auto stairs = [&](std::string_view facing, std::string_view shape,
                            jint x, jint y, jint z) {
        const std::string key =
            "minecraft:spruce_stairs[facing=" + std::string{facing} +
            ",half=bottom,shape=" + std::string{shape} + ",waterlogged=false]";
        place(key, x, y, z);
    };
    for (jint x = 0; x <= 6; ++x) {
        stairs("north", "straight", x, 4, 1);
    }
    for (jint z = 2; z <= 7; ++z) {
        stairs("east", "straight", 0, 4, z);
        stairs("west", "straight", 6, 4, z);
    }
    for (jint x = 0; x <= 6; ++x) {
        stairs("south", "straight", x, 4, 8);
    }
    stairs("north", "outer_right", 0, 4, 1);
    stairs("north", "outer_left", 6, 4, 1);
    stairs("south", "outer_left", 0, 4, 8);
    stairs("south", "outer_right", 6, 4, 8);
    const auto replaceable = [&](const block_pos &pos) {
        const auto name = name_of(pos);
        return name == "minecraft:air" || name == "minecraft:cave_air" ||
               name == "minecraft:water" || name == "minecraft:lava" ||
               name == "minecraft:glow_lichen" ||
               name == "minecraft:seagrass" ||
               name == "minecraft:tall_seagrass";
    };
    for (jint z = 2; z <= 7; z += 5) {
        for (jint x = 1; x <= 5; x += 4) {
            auto pos = local_pos(x, -1, z);
            while (pos.y > context.chunk->min_y() + 1 && replaceable(pos)) {
                (void)region.set_block_state(pos.x, pos.y, pos.z, resolve(log));
                --pos.y;
            }
        }
    }
}

void place_igloo_piece(const structure_place_context &context,
                       const structure_piece_record &piece,
                       worldgen_random &random) {
    if (context.templates == nullptr || context.registry == nullptr) {
        return;
    }
    const auto offset = igloo_offset(piece.template_id);
    if (!offset) {
        return;
    }
    const auto &templ = context.templates->get_or_create(piece.template_id);
    if (templ.palettes.empty()) {
        return;
    }
    const template_transform transform{
        .mirror = static_cast<template_mirror>(piece.template_mirror),
        .rotation = static_cast<template_rotation>(piece.template_rotation),
        .pivot = block_pos{.x = piece.template_pivot_x,
                           .y = piece.template_pivot_y,
                           .z = piece.template_pivot_z},
    };
    const auto entrance_local = transform_position(
        block_pos{.x = 3 - offset->x, .y = 0, .z = -offset->z}, transform);
    const jint entrance_x = piece.template_x + entrance_local.x;
    const jint entrance_z = piece.template_z + entrance_local.z;
    auto *height_chunk = context.region->find(entrance_x >> 4, entrance_z >> 4);
    if (height_chunk == nullptr) {
        return;
    }
    const jint height = height_chunk->first_available(
        heightmap_kind::world_surface_wg, entrance_x & 15, entrance_z & 15);
    block_pos shifted{.x = piece.template_x,
                      .y = piece.template_y + height - 90 - 1,
                      .z = piece.template_z};

    block_ignore_processor ignore{
        std::vector<std::string>{"minecraft:structure_block"}};
    template_place_settings settings{};
    settings.transform = transform;
    settings.liquids = liquid_settings::ignore_waterlogging;
    settings.bounds = template_bounds{
        .min_x = context.chunk->chunk_x() * 16,
        .min_y = context.chunk->min_y(),
        .min_z = context.chunk->chunk_z() * 16,
        .max_x = context.chunk->chunk_x() * 16 + 15,
        .max_y = context.chunk->min_y() + context.chunk->height() - 1,
        .max_z = context.chunk->chunk_z() * 16 + 15,
    };
    settings.processors = {&ignore};
    (void)place_template(*context.region, *context.registry, templ, shifted,
                         settings);

    if (piece.template_id == "minecraft:igloo/top") {
        const auto trapdoor_local =
            transform_position(block_pos{.x = 3, .y = 0, .z = 5}, transform);
        const jint trapdoor_x = shifted.x + trapdoor_local.x;
        const jint trapdoor_y = shifted.y + trapdoor_local.y;
        const jint trapdoor_z = shifted.z + trapdoor_local.z;
        const auto below =
            context.region->state_at(trapdoor_x, trapdoor_y - 1, trapdoor_z);
        const auto below_name = block_name(context, below);
        if (!context.states->traits(below).air &&
            below_name != "minecraft:ladder") {
            const auto snow = context.state("minecraft:snow_block");
            if (snow != no_block_state) {
                (void)context.region->set_block_state(trapdoor_x, trapdoor_y,
                                                      trapdoor_z, snow);
            }
        }
    }

    const auto &palette = templ.palettes.front();
    for (const auto &block : palette) {
        if (block.nbt == nullptr) {
            continue;
        }
        if (block_name(context, block.state) != "minecraft:structure_block") {
            continue;
        }
        const auto mode = block.nbt->find("mode");
        const auto metadata = block.nbt->find("metadata");
        if (mode == block.nbt->end() || metadata == block.nbt->end()) {
            continue;
        }
        const auto mode_text = mode->second.as_string();
        const auto metadata_text = metadata->second.as_string();
        if (!mode_text || !metadata_text || *metadata_text != "chest" ||
            (*mode_text != "DATA" && *mode_text != "data")) {
            continue;
        }
        const auto marker_local = transform_position(
            block_pos{.x = block.x, .y = block.y, .z = block.z}, transform);
        const block_pos marker{.x = shifted.x + marker_local.x,
                               .y = shifted.y + marker_local.y,
                               .z = shifted.z + marker_local.z};
        auto *chest_chunk = context.region->find(marker.x >> 4, marker.z >> 4);
        if (chest_chunk == nullptr) {
            continue;
        }
        const jint chest_y = marker.y - 1;
        if (block_name(context,
                       context.region->state_at(marker.x, chest_y, marker.z)) !=
            "minecraft:chest") {
            continue;
        }
        const auto local_x = static_cast<jint>(marker.x & 15);
        const auto local_z = static_cast<jint>(marker.z & 15);
        auto entity = chest_chunk->block_entity_at(
            block_pos{.x = local_x, .y = chest_y, .z = local_z});
        data::nbt::compound compound =
            entity == nullptr ? data::nbt::compound{} : *entity;
        compound["id"] = data::nbt::value{std::string{"minecraft:chest"}};
        compound["x"] = data::nbt::value{marker.x};
        compound["y"] = data::nbt::value{chest_y};
        compound["z"] = data::nbt::value{marker.z};
        compound["LootTable"] =
            data::nbt::value{std::string{"minecraft:chests/igloo"}};
        compound["LootTableSeed"] = data::nbt::value{random.next_long()};
        (void)context.region->set_block_entity(marker.x, chest_y, marker.z,
                                               std::move(compound));
    }
}

void place_buried_treasure(const structure_place_context &context,
                           const structure_piece_record &piece,
                           worldgen_random &random) {
    const jint x = piece.min_x;
    const jint z = piece.min_z;
    if (x < context.chunk->chunk_x() * 16 ||
        x >= context.chunk->chunk_x() * 16 + 16 ||
        z < context.chunk->chunk_z() * 16 ||
        z >= context.chunk->chunk_z() * 16 + 16) {
        return;
    }
    const jint local_x = x & 15;
    const jint local_z = z & 15;
    jint y = context.chunk->first_available(heightmap_kind::ocean_floor_wg,
                                            local_x, local_z);
    while (y > context.chunk->min_y()) {
        const auto current = context.region->state_at(x, y, z);
        const auto below = context.region->state_at(x, y - 1, z);
        const auto below_name = block_name(context, below);
        if (below_name != "minecraft:sandstone" &&
            below_name != "minecraft:stone" &&
            below_name != "minecraft:andesite" &&
            below_name != "minecraft:granite" &&
            below_name != "minecraft:diorite") {
            --y;
            continue;
        }
        const auto soft = (!context.states->traits(current).air &&
                           !is_liquid(context, current))
                              ? current
                              : context.state("minecraft:sand");
        for (const auto &direction : all_directions) {
            const jint relative_x = x + direction.dx;
            const jint relative_y = y + direction.dy;
            const jint relative_z = z + direction.dz;
            const auto relative =
                context.region->state_at(relative_x, relative_y, relative_z);
            if (!context.states->traits(relative).air &&
                !is_liquid(context, relative)) {
                continue;
            }
            const auto below_relative = context.region->state_at(
                relative_x, relative_y - 1, relative_z);
            const bool below_open =
                context.states->traits(below_relative).air ||
                is_liquid(context, below_relative);
            const auto placed =
                (below_open && direction.dy != 1) ? below : soft;
            (void)context.region->set_block_state(relative_x, relative_y,
                                                  relative_z, placed);
        }
        (void)create_chest(context, x, y, z, random);
        return;
    }
}

void place_jungle_temple(const structure_place_context &context,
                         const structure_piece_record &piece,
                         worldgen_random &random) {
    if (context.region == nullptr || context.states == nullptr) {
        return;
    }
    jint base_y = piece.height_position;
    if (base_y < 0) {
        jint total = 0;
        jint count = 0;
        for (jint z = piece.min_z; z <= piece.max_z; ++z) {
            for (jint x = piece.min_x; x <= piece.max_x; ++x) {
                if ((x >> 4) != context.chunk->chunk_x() ||
                    (z >> 4) != context.chunk->chunk_z()) {
                    continue;
                }
                auto *column = context.region->find(x >> 4, z >> 4);
                if (column == nullptr) {
                    continue;
                }
                total += column->first_available(
                    heightmap_kind::motion_blocking_no_leaves, x & 15, z & 15);
                ++count;
            }
        }
        if (count == 0) {
            return;
        }
        base_y = total / count;
    }
    const auto local = [&](jint x, jint y, jint z) {
        return piece_local_to_world(piece, base_y, x, y, z);
    };
    const auto resolve = [&](std::string_view key) {
        return context.state ? context.state(key) : no_block_state;
    };
    template_mirror mirror = template_mirror::none;
    template_rotation rotation = template_rotation::none;
    switch (piece.orientation) {
    case 0:
        mirror = template_mirror::left_right;
        break;
    case 1:
        mirror = template_mirror::left_right;
        rotation = template_rotation::clockwise_90;
        break;
    case 3:
        rotation = template_rotation::clockwise_90;
        break;
    default:
        break;
    }
    const auto orient_state = [&](block_state_id state) {
        if (context.registry == nullptr) {
            return state;
        }
        return transform_state(*context.registry, state,
                               template_transform{.mirror = mirror,
                                                  .rotation = rotation,
                                                  .pivot = block_pos{}});
    };
    const auto place = [&](block_state_id state, jint x, jint y, jint z) {
        if (state == no_block_state) {
            return;
        }
        const auto pos = local(x, y, z);
        (void)context.region->set_block_state(pos.x, pos.y, pos.z,
                                              orient_state(state));
    };
    const auto place_raw = [&](block_state_id state, jint x, jint y, jint z) {
        if (state == no_block_state) {
            return;
        }
        const auto pos = local(x, y, z);
        (void)context.region->set_block_state(pos.x, pos.y, pos.z, state);
    };
    const auto place_key = [&](std::string_view key, jint x, jint y, jint z) {
        place(resolve(key), x, y, z);
    };
    const auto air = resolve("minecraft:air");
    const auto cobble = resolve("minecraft:cobblestone");
    const auto mossy = resolve("minecraft:mossy_cobblestone");
    const auto box = [&](jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
                         bool skip_air) {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    const auto pos = local(x, y, z);
                    if (skip_air && context.states
                                        ->traits(context.region->state_at(
                                            pos.x, pos.y, pos.z))
                                        .air) {
                        continue;
                    }
                    const auto state =
                        random.next_float() < 0.4F ? cobble : mossy;
                    place(state, x, y, z);
                }
            }
        }
    };
    const auto air_box = [&](jint x0, jint y0, jint z0, jint x1, jint y1,
                             jint z1) {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    place(air, x, y, z);
                }
            }
        }
    };
    const auto stairs = [&](std::string_view facing, jint x, jint y, jint z) {
        const std::string key =
            "minecraft:cobblestone_stairs[facing=" + std::string{facing} +
            ",half=bottom,shape=straight,waterlogged=false]";
        place_key(key, x, y, z);
    };
    const auto chest_at = [&](jint x, jint y, jint z,
                              std::string_view loot) -> bool {
        const auto pos = local(x, y, z);
        if ((pos.x >> 4) != context.chunk->chunk_x() ||
            (pos.z >> 4) != context.chunk->chunk_z()) {
            return false;
        }
        if (block_name(context,
                       context.region->state_at(pos.x, pos.y, pos.z)) ==
            "minecraft:chest") {
            return false;
        }
        const auto facing = chest_facing(context, pos.x, pos.y, pos.z);
        const std::string key =
            "minecraft:chest[facing=" + std::string{facing} +
            ",type=single,waterlogged=false]";
        const auto state = resolve(key);
        if (state == no_block_state) {
            return false;
        }
        place_raw(state, x, y, z);
        data::nbt::compound entity;
        entity["id"] = data::nbt::value{std::string{"minecraft:chest"}};
        entity["x"] = data::nbt::value{pos.x};
        entity["y"] = data::nbt::value{pos.y};
        entity["z"] = data::nbt::value{pos.z};
        entity["LootTable"] = data::nbt::value{std::string{loot}};
        entity["LootTableSeed"] = data::nbt::value{random.next_long()};
        (void)context.region->set_block_entity(pos.x, pos.y, pos.z,
                                               std::move(entity));
        return true;
    };
    const auto dispenser_at = [&](jint x, jint y, jint z,
                                  std::string_view facing,
                                  std::string_view loot) -> bool {
        const auto pos = local(x, y, z);
        if ((pos.x >> 4) != context.chunk->chunk_x() ||
            (pos.z >> 4) != context.chunk->chunk_z()) {
            return false;
        }
        if (block_name(context,
                       context.region->state_at(pos.x, pos.y, pos.z)) ==
            "minecraft:dispenser") {
            return false;
        }
        const std::string key =
            "minecraft:dispenser[facing=" + std::string{facing} +
            ",triggered=false]";
        const auto state = resolve(key);
        if (state == no_block_state) {
            return false;
        }
        place(state, x, y, z);
        data::nbt::compound entity;
        entity["id"] = data::nbt::value{std::string{"minecraft:dispenser"}};
        entity["x"] = data::nbt::value{pos.x};
        entity["y"] = data::nbt::value{pos.y};
        entity["z"] = data::nbt::value{pos.z};
        entity["LootTable"] = data::nbt::value{std::string{loot}};
        entity["LootTableSeed"] = data::nbt::value{random.next_long()};
        (void)context.region->set_block_entity(pos.x, pos.y, pos.z,
                                               std::move(entity));
        return true;
    };

    box(0, -4, 0, 11, 0, 14, false);
    box(2, 1, 2, 9, 2, 2, false);
    box(2, 1, 12, 9, 2, 12, false);
    box(2, 1, 3, 2, 2, 11, false);
    box(9, 1, 3, 9, 2, 11, false);
    box(1, 3, 1, 10, 6, 1, false);
    box(1, 3, 13, 10, 6, 13, false);
    box(1, 3, 2, 1, 6, 12, false);
    box(10, 3, 2, 10, 6, 12, false);
    box(2, 3, 2, 9, 3, 12, false);
    box(2, 6, 2, 9, 6, 12, false);
    box(3, 7, 3, 8, 7, 11, false);
    box(4, 8, 4, 7, 8, 10, false);
    air_box(3, 1, 3, 8, 2, 11);
    air_box(4, 3, 6, 7, 3, 9);
    air_box(2, 4, 2, 9, 5, 12);
    air_box(4, 6, 5, 7, 6, 9);
    air_box(5, 7, 6, 6, 7, 8);
    air_box(5, 1, 2, 6, 2, 2);
    air_box(5, 2, 12, 6, 2, 12);
    air_box(5, 5, 1, 6, 5, 1);
    air_box(5, 5, 13, 6, 5, 13);
    place(air, 1, 5, 5);
    place(air, 10, 5, 5);
    place(air, 1, 5, 9);
    place(air, 10, 5, 9);
    for (jint z = 0; z <= 14; z += 14) {
        box(2, 4, z, 2, 5, z, false);
        box(4, 4, z, 4, 5, z, false);
        box(7, 4, z, 7, 5, z, false);
        box(9, 4, z, 9, 5, z, false);
    }
    box(5, 6, 0, 6, 6, 0, false);
    for (jint x = 0; x <= 11; x += 11) {
        for (jint z = 2; z <= 12; z += 2) {
            box(x, 4, z, x, 5, z, false);
        }
        box(x, 6, 5, x, 6, 5, false);
        box(x, 6, 9, x, 6, 9, false);
    }
    box(2, 7, 2, 2, 9, 2, false);
    box(9, 7, 2, 9, 9, 2, false);
    box(2, 7, 12, 2, 9, 12, false);
    box(9, 7, 12, 9, 9, 12, false);
    box(4, 9, 4, 4, 9, 4, false);
    box(7, 9, 4, 7, 9, 4, false);
    box(4, 9, 10, 4, 9, 10, false);
    box(7, 9, 10, 7, 9, 10, false);
    box(5, 9, 7, 6, 9, 7, false);
    stairs("north", 5, 9, 6);
    stairs("north", 6, 9, 6);
    stairs("south", 5, 9, 8);
    stairs("south", 6, 9, 8);
    stairs("north", 4, 0, 0);
    stairs("north", 5, 0, 0);
    stairs("north", 6, 0, 0);
    stairs("north", 7, 0, 0);
    stairs("north", 4, 1, 8);
    stairs("north", 4, 2, 9);
    stairs("north", 4, 3, 10);
    stairs("north", 7, 1, 8);
    stairs("north", 7, 2, 9);
    stairs("north", 7, 3, 10);
    box(4, 1, 9, 4, 1, 9, false);
    box(7, 1, 9, 7, 1, 9, false);
    box(4, 1, 10, 7, 2, 10, false);
    box(5, 4, 5, 6, 4, 5, false);
    stairs("east", 4, 4, 5);
    stairs("west", 7, 4, 5);
    for (jint i = 0; i < 4; ++i) {
        stairs("south", 5, -i, 6 + i);
        stairs("south", 6, -i, 6 + i);
        air_box(5, -i, 7 + i, 6, -i, 9 + i);
    }
    air_box(1, -3, 12, 10, -1, 13);
    air_box(1, -3, 1, 3, -1, 13);
    air_box(1, -3, 1, 9, -1, 5);
    for (jint z = 1; z <= 13; z += 2) {
        box(1, -3, z, 1, -2, z, false);
    }
    for (jint z = 2; z <= 12; z += 2) {
        box(1, -1, z, 3, -1, z, false);
    }
    box(2, -2, 1, 5, -2, 1, false);
    box(7, -2, 1, 9, -2, 1, false);
    box(6, -3, 1, 6, -3, 1, false);
    box(6, -1, 1, 6, -1, 1, false);
    place_key(
        "minecraft:tripwire_hook[attached=true,facing=east,powered=false]", 1,
        -3, 8);
    place_key(
        "minecraft:tripwire_hook[attached=true,facing=west,powered=false]", 4,
        -3, 8);
    place_key("minecraft:tripwire[attached=true,disarmed=false,east=true,north="
              "false,powered=false,south=false,west=true]",
              2, -3, 8);
    place_key("minecraft:tripwire[attached=true,disarmed=false,east=true,north="
              "false,powered=false,south=false,west=true]",
              3, -3, 8);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              5, -3, 7);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              5, -3, 6);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              5, -3, 5);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              5, -3, 4);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              5, -3, 3);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              5, -3, 2);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=none,"
              "west=side]",
              5, -3, 1);
    place_key("minecraft:redstone_wire[east=side,north=none,power=0,south=none,"
              "west=side]",
              4, -3, 1);
    place(mossy, 3, -3, 1);
    if (!dispenser_at(3, -2, 1, "north",
                      "minecraft:chests/jungle_temple_dispenser")) {
        // The reference always places this dispenser in this template.
    }
    place_key(
        "minecraft:vine[east=false,north=false,south=true,up=false,west=false]",
        3, -2, 2);
    place_key(
        "minecraft:tripwire_hook[attached=true,facing=north,powered=false]", 7,
        -3, 1);
    place_key(
        "minecraft:tripwire_hook[attached=true,facing=south,powered=false]", 7,
        -3, 5);
    place_key("minecraft:tripwire[attached=true,disarmed=false,east=false,"
              "north=true,powered=false,south=true,west=false]",
              7, -3, 2);
    place_key("minecraft:tripwire[attached=true,disarmed=false,east=false,"
              "north=true,powered=false,south=true,west=false]",
              7, -3, 3);
    place_key("minecraft:tripwire[attached=true,disarmed=false,east=false,"
              "north=true,powered=false,south=true,west=false]",
              7, -3, 4);
    place_key("minecraft:redstone_wire[east=side,north=none,power=0,south=none,"
              "west=side]",
              8, -3, 6);
    place_key("minecraft:redstone_wire[east=none,north=none,power=0,south=side,"
              "west=side]",
              9, -3, 6);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=up,"
              "west=none]",
              9, -3, 5);
    place(mossy, 9, -3, 4);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              9, -2, 4);
    if (!dispenser_at(9, -2, 3, "west",
                      "minecraft:chests/jungle_temple_dispenser")) {
    }
    place_key(
        "minecraft:vine[east=true,north=false,south=false,up=false,west=false]",
        8, -1, 3);
    place_key(
        "minecraft:vine[east=true,north=false,south=false,up=false,west=false]",
        8, -2, 3);
    if (!chest_at(8, -3, 3, "minecraft:chests/jungle_temple")) {
    }
    place(mossy, 9, -3, 2);
    place(mossy, 8, -3, 1);
    place(mossy, 4, -3, 5);
    place(mossy, 5, -2, 5);
    place(mossy, 5, -1, 5);
    place(mossy, 6, -3, 5);
    place(mossy, 7, -2, 5);
    place(mossy, 7, -1, 5);
    place(mossy, 8, -3, 5);
    box(9, -1, 1, 9, -1, 5, false);
    air_box(8, -3, 8, 10, -1, 10);
    place_key("minecraft:chiseled_stone_bricks", 8, -2, 11);
    place_key("minecraft:chiseled_stone_bricks", 9, -2, 11);
    place_key("minecraft:chiseled_stone_bricks", 10, -2, 11);
    place_key("minecraft:lever[face=wall,facing=north,powered=false]", 8, -2,
              12);
    place_key("minecraft:lever[face=wall,facing=north,powered=false]", 9, -2,
              12);
    place_key("minecraft:lever[face=wall,facing=north,powered=false]", 10, -2,
              12);
    box(8, -3, 8, 8, -3, 10, false);
    box(10, -3, 8, 10, -3, 10, false);
    place(mossy, 10, -2, 9);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              8, -2, 9);
    place_key("minecraft:redstone_wire[east=none,north=side,power=0,south=side,"
              "west=none]",
              8, -2, 10);
    place_key("minecraft:redstone_wire[east=side,north=side,power=0,south=side,"
              "west=side]",
              10, -1, 9);
    place_key("minecraft:sticky_piston[extended=false,facing=up]", 9, -2, 8);
    place_key("minecraft:sticky_piston[extended=false,facing=west]", 10, -2, 8);
    place_key("minecraft:sticky_piston[extended=false,facing=west]", 10, -1, 8);
    place_key(
        "minecraft:repeater[delay=1,facing=north,locked=false,powered=false]",
        10, -2, 10);
    if (!chest_at(9, -3, 10, "minecraft:chests/jungle_temple")) {
    }
}

void place_ocean_ruin_piece(const structure_place_context &context,
                            const structure_piece_record &piece,
                            worldgen_random &random) {
    if (context.templates == nullptr || context.region == nullptr) {
        return;
    }
    const auto &templ = context.templates->get_or_create(piece.template_id);
    if (templ.palettes.empty()) {
        return;
    }
    block_pos origin{
        .x = piece.template_x, .y = piece.template_y, .z = piece.template_z};
    if (piece.height_position >= 0) {
        origin.y = piece.height_position;
    } else {
        const auto column_x = origin.x & 15;
        const auto column_z = origin.z & 15;
        auto *column_chunk = context.region->find(origin.x >> 4, origin.z >> 4);
        if (column_chunk != nullptr) {
            origin.y = column_chunk->first_available(
                heightmap_kind::ocean_floor_wg, column_x, column_z);
            const auto corner = transform_position(
                block_pos{.x = templ.size_x - 1, .y = 0, .z = templ.size_z - 1},
                template_transform{
                    .mirror = template_mirror::none,
                    .rotation =
                        static_cast<template_rotation>(piece.template_rotation),
                    .pivot = block_pos{},
                });
            origin.y =
                ocean_ruin_floor_height(*context.region, *context.registry,
                                        context.chunk->min_y(), origin,
                                        block_pos{.x = origin.x + corner.x,
                                                  .y = origin.y + corner.y,
                                                  .z = origin.z + corner.z});
        }
    }
    block_rot_processor rot{piece.integrity, {}, false};
    block_ignore_processor ignore{
        std::vector<std::string>{"minecraft:air", "minecraft:structure_block"}};
    const bool warm = piece.template_id.find("/warm") != std::string::npos;
    capped_rule_processor suspicious{
        warm ? "minecraft:sand" : "minecraft:gravel",
        warm ? "minecraft:suspicious_sand" : "minecraft:suspicious_gravel",
        warm ? "minecraft:archaeology/ocean_ruin_warm"
             : "minecraft:archaeology/ocean_ruin_cold",
        5};
    template_place_settings settings{};
    settings.transform = template_transform{
        .mirror = template_mirror::none,
        .rotation = static_cast<template_rotation>(piece.template_rotation),
        .pivot = block_pos{},
    };
    settings.liquids = liquid_settings::apply_waterlogging;
    settings.bounds = template_bounds{
        .min_x = context.chunk->chunk_x() * 16,
        .min_y = context.chunk->min_y(),
        .min_z = context.chunk->chunk_z() * 16,
        .max_x = context.chunk->chunk_x() * 16 + 15,
        .max_y = context.chunk->min_y() + context.chunk->height() - 1,
        .max_z = context.chunk->chunk_z() * 16 + 15,
    };
    settings.processors = {&rot, &ignore, &suspicious};
    settings.level_seed = context.seed;
    (void)place_template(*context.region, *context.registry, templ, origin,
                         settings);
    const std::string loot = piece.large
                                 ? "minecraft:chests/underwater_ruin_big"
                                 : "minecraft:chests/underwater_ruin_small";
    for (const auto &palette : templ.palettes) {
        for (const auto &block : palette) {
            if (block.nbt == nullptr) {
                continue;
            }
            const auto metadata = block.nbt->find("metadata");
            if (metadata == block.nbt->end()) {
                continue;
            }
            const auto text = metadata->second.as_string();
            if (!text) {
                continue;
            }
            const auto relative = transform_position(
                block_pos{.x = block.x, .y = block.y, .z = block.z},
                settings.transform);
            const auto world = block_pos{.x = origin.x + relative.x,
                                         .y = origin.y + relative.y,
                                         .z = origin.z + relative.z};
            if (context.chunk == nullptr ||
                (world.x >> 4) != context.chunk->chunk_x() ||
                (world.z >> 4) != context.chunk->chunk_z()) {
                continue;
            }
            if (*text == "chest") {
                const auto previous = block_name(
                    context,
                    context.region->state_at(world.x, world.y, world.z));
                const bool water =
                    previous == "minecraft:water" ||
                    previous.starts_with("minecraft:water[level=0");
                const std::string key = water
                                            ? "minecraft:chest[facing=north,"
                                              "type=single,waterlogged=true]"
                                            : "minecraft:chest[facing=north,"
                                              "type=single,waterlogged=false]";
                const auto chest =
                    context.state ? context.state(key) : no_block_state;
                if (chest != no_block_state) {
                    (void)context.region->set_block_state(world.x, world.y,
                                                          world.z, chest);
                }
                data::nbt::compound entity;
                entity["id"] = data::nbt::value{std::string{"minecraft:chest"}};
                entity["x"] = data::nbt::value{world.x};
                entity["y"] = data::nbt::value{world.y};
                entity["z"] = data::nbt::value{world.z};
                entity["LootTable"] = data::nbt::value{std::string{loot}};
                entity["LootTableSeed"] = data::nbt::value{random.next_long()};
                (void)context.region->set_block_entity(
                    world.x, world.y, world.z, std::move(entity));
            } else if (*text == "drowned") {
                const auto water = context.state("minecraft:water");
                const auto air = context.state("minecraft:air");
                const auto replacement = world.y > 63 ? air : water;
                if (replacement != no_block_state) {
                    (void)context.region->set_block_state(world.x, world.y,
                                                          world.z, replacement);
                }
            }
        }
    }
}

void place_shipwreck_piece(const structure_place_context &context,
                           const structure_piece_record &piece,
                           worldgen_random &random) {
    if (context.templates == nullptr) {
        return;
    }
    const auto &templ = context.templates->get_or_create(piece.template_id);
    if (templ.palettes.empty()) {
        return;
    }
    block_ignore_processor ignore{
        std::vector<std::string>{"minecraft:air", "minecraft:structure_block"}};
    template_place_settings settings{};
    settings.transform = template_transform{
        .mirror = template_mirror::none,
        .rotation = static_cast<template_rotation>(piece.template_rotation),
        .pivot = block_pos{.x = piece.template_pivot_x,
                           .y = piece.template_pivot_y,
                           .z = piece.template_pivot_z},
    };
    settings.liquids = liquid_settings::apply_waterlogging;
    settings.bounds = template_bounds{
        .min_x = context.chunk->chunk_x() * 16,
        .min_y = context.chunk->min_y(),
        .min_z = context.chunk->chunk_z() * 16,
        .max_x = context.chunk->chunk_x() * 16 + 15,
        .max_y = context.chunk->min_y() + context.chunk->height() - 1,
        .max_z = context.chunk->chunk_z() * 16 + 15,
    };
    settings.processors = {&ignore};
    block_pos origin{
        .x = piece.template_x, .y = piece.template_y, .z = piece.template_z};
    if (piece.height_position < 0 && templ.size_x <= 32 && templ.size_y <= 32) {
        const auto kind = piece.beached ? heightmap_kind::world_surface_wg
                                        : heightmap_kind::ocean_floor_wg;
        const auto height_at = [&](jint x, jint z) -> std::optional<jint> {
            auto *column = context.region->find(x >> 4, z >> 4);
            if (column == nullptr) {
                return std::nullopt;
            }
            return column->first_available(kind, x & 15, z & 15);
        };
        const jint base_size = templ.size_x * templ.size_z;
        if (base_size == 0) {
            origin.y = height_at(origin.x, origin.z).value_or(origin.y);
        } else {
            const auto corner = transform_position(
                block_pos{.x = templ.size_x - 1, .y = 0, .z = templ.size_z - 1},
                settings.transform);
            const block_pos corner_world{.x = origin.x + corner.x,
                                         .y = origin.y,
                                         .z = origin.z + corner.z};
            jint min_y = 1 << 30;
            jint total = 0;
            jint count = 0;
            for (jint z = std::min(origin.z, corner_world.z);
                 z <= std::max(origin.z, corner_world.z); ++z) {
                for (jint x = std::min(origin.x, corner_world.x);
                     x <= std::max(origin.x, corner_world.x); ++x) {
                    const auto height = height_at(x, z);
                    if (!height) {
                        continue;
                    }
                    min_y = std::min(min_y, *height);
                    total += *height;
                    ++count;
                }
            }
            if (count == 0) {
                return;
            }
            const jint mean = total / base_size;
            origin.y = piece.beached
                           ? min_y - templ.size_y / 2 - random.next_int(3)
                           : mean;
        }
    }
    (void)place_template(*context.region, *context.registry, templ, origin,
                         settings);
    static const std::array<std::pair<std::string_view, std::string_view>, 3>
        markers{{
            {"map_chest", "minecraft:chests/shipwreck_map"},
            {"treasure_chest", "minecraft:chests/shipwreck_treasure"},
            {"supply_chest", "minecraft:chests/shipwreck_supply"},
        }};
    for (const auto &palette : templ.palettes) {
        for (const auto &block : palette) {
            if (block.nbt == nullptr) {
                continue;
            }
            const auto metadata = block.nbt->find("metadata");
            if (metadata == block.nbt->end()) {
                continue;
            }
            const auto text = metadata->second.as_string();
            if (!text) {
                continue;
            }
            for (const auto &[marker, loot] : markers) {
                if (*text != marker) {
                    continue;
                }
                const auto relative = transform_position(
                    block_pos{.x = block.x, .y = block.y, .z = block.z},
                    settings.transform);
                const block_pos world{.x = origin.x + relative.x,
                                      .y = origin.y + relative.y,
                                      .z = origin.z + relative.z};
                const auto below =
                    block_pos{.x = world.x, .y = world.y - 1, .z = world.z};
                if (context.chunk == nullptr ||
                    (below.x >> 4) != context.chunk->chunk_x() ||
                    (below.z >> 4) != context.chunk->chunk_z()) {
                    continue;
                }
                const auto name = block_name(
                    context,
                    context.region->state_at(below.x, below.y, below.z));
                if (!name.starts_with("minecraft:chest")) {
                    continue;
                }
                data::nbt::compound entity;
                entity["id"] = data::nbt::value{std::string{"minecraft:chest"}};
                entity["x"] = data::nbt::value{below.x};
                entity["y"] = data::nbt::value{below.y};
                entity["z"] = data::nbt::value{below.z};
                entity["LootTable"] = data::nbt::value{std::string{loot}};
                entity["LootTableSeed"] = data::nbt::value{random.next_long()};
                (void)context.region->set_block_entity(
                    below.x, below.y, below.z, std::move(entity));
            }
        }
    }
}

} // namespace

namespace {

auto place_pyramid_chest(const structure_place_context &context, jint x, jint y,
                         jint z, worldgen_random &random) -> bool {
    if (block_name(context, context.region->state_at(x, y, z)) ==
        "minecraft:chest") {
        return false;
    }
    const auto facing = chest_facing(context, x, y, z);
    const std::string key =
        "minecraft:chest[facing=" + std::string{facing} + "]";
    const auto chest = context.state(key);
    if (chest == no_block_state ||
        !context.region->set_block_state(x, y, z, chest)) {
        return false;
    }
    data::nbt::compound entity;
    entity["id"] = data::nbt::value{std::string{"minecraft:chest"}};
    entity["x"] = data::nbt::value{x};
    entity["y"] = data::nbt::value{y};
    entity["z"] = data::nbt::value{z};
    entity["LootTable"] =
        data::nbt::value{std::string{"minecraft:chests/desert_pyramid"}};
    entity["LootTableSeed"] = data::nbt::value{random.next_long()};
    (void)context.region->set_block_entity(x, y, z, std::move(entity));
    return true;
}

} // namespace

void place_desert_pyramid(const structure_place_context &context,
                          const structure_piece_record &piece,
                          worldgen_random &random) {
    auto &region = *context.region;
    const auto resolve = [&](std::string_view key) {
        return context.state ? context.state(key) : no_block_state;
    };
    structure::template_mirror mirror = structure::template_mirror::none;
    structure::template_rotation rotation = structure::template_rotation::none;
    switch (piece.orientation) {
    case 0:
        mirror = structure::template_mirror::left_right;
        break;
    case 1:
        mirror = structure::template_mirror::left_right;
        rotation = structure::template_rotation::clockwise_90;
        break;
    case 3:
        rotation = structure::template_rotation::clockwise_90;
        break;
    default:
        break;
    }
    const auto orient_state = [&](block_state_id state) {
        if (context.registry == nullptr) {
            return state;
        }
        return structure::transform_state(
            *context.registry, state,
            structure::template_transform{
                .mirror = mirror, .rotation = rotation, .pivot = block_pos{}});
    };
    jint base_y = piece.height_position + piece.height_offset;
    const auto local = [&](jint x, jint y, jint z) {
        return piece_local_to_world(piece, base_y, x, y, z);
    };
    const auto place = [&](std::string_view key, jint x, jint y, jint z) {
        auto state = resolve(key);
        if (state == no_block_state) {
            return;
        }
        state = orient_state(state);
        const auto pos = local(x, y, z);
        (void)region.set_block_state(pos.x, pos.y, pos.z, state);
    };
    const auto box = [&](jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
                         std::string_view edge, std::string_view fill,
                         bool skip_air) {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    const auto pos = local(x, y, z);
                    if (skip_air &&
                        context.states
                            ->traits(region.state_at(pos.x, pos.y, pos.z))
                            .air) {
                        continue;
                    }
                    const bool on_edge = x == x0 || x == x1 || y == y0 ||
                                         y == y1 || z == z0 || z == z1;
                    place(on_edge ? edge : fill, x, y, z);
                }
            }
        }
    };
    const auto column_down = [&](std::string_view key, jint x, jint y, jint z) {
        jint current_y = y;
        const auto replaceable = [&](const block_pos &pos) {
            const auto name =
                block_name(context, region.state_at(pos.x, pos.y, pos.z));
            return name == "minecraft:air" || name == "minecraft:cave_air" ||
                   name == "minecraft:void_air" || name == "minecraft:water" ||
                   name == "minecraft:lava" ||
                   name == "minecraft:glow_lichen" ||
                   name == "minecraft:seagrass" ||
                   name == "minecraft:tall_seagrass";
        };
        auto pos = local(x, current_y, z);
        while (pos.y > context.chunk->min_y() + 1 && replaceable(pos)) {
            place(key, x, current_y, z);
            --current_y;
            pos = local(x, current_y, z);
        }
    };
    constexpr std::string_view sand = "minecraft:sand";
    constexpr std::string_view sandstone = "minecraft:sandstone";
    constexpr std::string_view cut = "minecraft:cut_sandstone";
    constexpr std::string_view chiseled = "minecraft:chiseled_sandstone";
    constexpr std::string_view air = "minecraft:air";
    constexpr std::string_view orange = "minecraft:orange_terracotta";
    constexpr std::string_view blue = "minecraft:blue_terracotta";
    constexpr std::string_view tnt = "minecraft:tnt";
    const std::string north_stairs =
        "minecraft:sandstone_stairs[facing=north,half=bottom,shape=straight,"
        "waterlogged=false]";
    const std::string south_stairs =
        "minecraft:sandstone_stairs[facing=south,half=bottom,shape=straight,"
        "waterlogged=false]";
    const std::string east_stairs =
        "minecraft:sandstone_stairs[facing=east,half=bottom,shape=straight,"
        "waterlogged=false]";
    const std::string west_stairs =
        "minecraft:sandstone_stairs[facing=west,half=bottom,shape=straight,"
        "waterlogged=false]";
    const std::string cellar_stairs =
        "minecraft:sandstone_stairs[facing=west,half=bottom,shape=straight,"
        "waterlogged=false]";
    constexpr std::string_view slab = "minecraft:sandstone_slab";
    constexpr std::string_view pressure = "minecraft:stone_pressure_plate";

    box(0, -4, 0, 20, 0, 20, sandstone, sandstone, false);
    for (jint pos = 1; pos <= 9; ++pos) {
        box(pos, pos, pos, 20 - pos, pos, 20 - pos, sandstone, sandstone,
            false);
        box(pos + 1, pos, pos + 1, 19 - pos, pos, 19 - pos, air, air, false);
    }
    for (jint x = 0; x <= 20; ++x) {
        for (jint z = 0; z <= 20; ++z) {
            column_down(sandstone, x, -5, z);
        }
    }
    box(0, 0, 0, 4, 9, 4, sandstone, air, false);
    box(1, 10, 1, 3, 10, 3, sandstone, sandstone, false);
    place(north_stairs, 2, 10, 0);
    place(south_stairs, 2, 10, 4);
    place(east_stairs, 0, 10, 2);
    place(west_stairs, 4, 10, 2);
    box(16, 0, 0, 20, 9, 4, sandstone, air, false);
    box(17, 10, 1, 19, 10, 3, sandstone, sandstone, false);
    place(north_stairs, 18, 10, 0);
    place(south_stairs, 18, 10, 4);
    place(east_stairs, 16, 10, 2);
    place(west_stairs, 20, 10, 2);
    box(8, 0, 0, 12, 4, 4, sandstone, air, false);
    box(9, 1, 0, 11, 3, 4, air, air, false);
    place(cut, 9, 1, 1);
    place(cut, 9, 2, 1);
    place(cut, 9, 3, 1);
    place(cut, 10, 3, 1);
    place(cut, 11, 3, 1);
    place(cut, 11, 2, 1);
    place(cut, 11, 1, 1);
    box(4, 1, 1, 8, 3, 3, sandstone, air, false);
    box(4, 1, 2, 8, 2, 2, air, air, false);
    box(12, 1, 1, 16, 3, 3, sandstone, air, false);
    box(12, 1, 2, 16, 2, 2, air, air, false);
    box(5, 4, 5, 15, 4, 15, sandstone, sandstone, false);
    box(9, 4, 9, 11, 4, 11, air, air, false);
    box(8, 1, 8, 8, 3, 8, cut, cut, false);
    box(12, 1, 8, 12, 3, 8, cut, cut, false);
    box(8, 1, 12, 8, 3, 12, cut, cut, false);
    box(12, 1, 12, 12, 3, 12, cut, cut, false);
    box(1, 1, 5, 4, 4, 11, sandstone, sandstone, false);
    box(16, 1, 5, 19, 4, 11, sandstone, sandstone, false);
    box(6, 7, 9, 6, 7, 11, sandstone, sandstone, false);
    box(14, 7, 9, 14, 7, 11, sandstone, sandstone, false);
    box(5, 5, 9, 5, 7, 11, cut, cut, false);
    box(15, 5, 9, 15, 7, 11, cut, cut, false);
    place(air, 5, 5, 10);
    place(air, 5, 6, 10);
    place(air, 6, 6, 10);
    place(air, 15, 5, 10);
    place(air, 15, 6, 10);
    place(air, 14, 6, 10);
    box(2, 4, 4, 2, 6, 4, air, air, false);
    box(18, 4, 4, 18, 6, 4, air, air, false);
    place(north_stairs, 2, 4, 5);
    place(north_stairs, 2, 3, 4);
    place(north_stairs, 18, 4, 5);
    place(north_stairs, 18, 3, 4);
    box(1, 1, 3, 2, 2, 3, sandstone, sandstone, false);
    box(18, 1, 3, 19, 2, 3, sandstone, sandstone, false);
    place(sandstone, 1, 1, 2);
    place(sandstone, 19, 1, 2);
    place(slab, 1, 2, 2);
    place(slab, 19, 2, 2);
    place(west_stairs, 2, 1, 2);
    place(east_stairs, 18, 1, 2);
    box(4, 3, 5, 4, 3, 17, sandstone, sandstone, false);
    box(16, 3, 5, 16, 3, 17, sandstone, sandstone, false);
    box(3, 1, 5, 4, 2, 16, air, air, false);
    box(15, 1, 5, 16, 2, 16, air, air, false);
    for (jint z = 5; z <= 17; z += 2) {
        place(cut, 4, 1, z);
        place(chiseled, 4, 2, z);
        place(cut, 16, 1, z);
        place(chiseled, 16, 2, z);
    }
    place(orange, 10, 0, 7);
    place(orange, 10, 0, 8);
    place(orange, 9, 0, 9);
    place(orange, 11, 0, 9);
    place(orange, 8, 0, 10);
    place(orange, 12, 0, 10);
    place(orange, 7, 0, 10);
    place(orange, 13, 0, 10);
    place(orange, 9, 0, 11);
    place(orange, 11, 0, 11);
    place(orange, 10, 0, 12);
    place(orange, 10, 0, 13);
    place(blue, 10, 0, 10);
    for (jint x = 0; x <= 20; x += 20) {
        place(cut, x, 2, 1);
        place(orange, x, 2, 2);
        place(cut, x, 2, 3);
        place(cut, x, 3, 1);
        place(orange, x, 3, 2);
        place(cut, x, 3, 3);
        place(orange, x, 4, 1);
        place(chiseled, x, 4, 2);
        place(orange, x, 4, 3);
        place(cut, x, 5, 1);
        place(orange, x, 5, 2);
        place(cut, x, 5, 3);
        place(orange, x, 6, 1);
        place(chiseled, x, 6, 2);
        place(orange, x, 6, 3);
        place(orange, x, 7, 1);
        place(orange, x, 7, 2);
        place(orange, x, 7, 3);
        place(cut, x, 8, 1);
        place(cut, x, 8, 2);
        place(cut, x, 8, 3);
    }
    for (jint x = 2; x <= 18; x += 15) {
        place(cut, x - 1, 2, 0);
        place(orange, x, 2, 0);
        place(cut, x + 1, 2, 0);
        place(cut, x - 1, 3, 0);
        place(orange, x, 3, 0);
        place(cut, x + 1, 3, 0);
        place(orange, x - 1, 4, 0);
        place(chiseled, x, 4, 0);
        place(orange, x + 1, 4, 0);
        place(cut, x - 1, 5, 0);
        place(orange, x, 5, 0);
        place(cut, x + 1, 5, 0);
        place(orange, x - 1, 6, 0);
        place(chiseled, x, 6, 0);
        place(orange, x + 1, 6, 0);
        place(orange, x - 1, 7, 0);
        place(orange, x, 7, 0);
        place(orange, x + 1, 7, 0);
        place(cut, x - 1, 8, 0);
        place(cut, x, 8, 0);
        place(cut, x + 1, 8, 0);
    }
    box(8, 4, 0, 12, 6, 0, cut, cut, false);
    place(air, 8, 6, 0);
    place(air, 12, 6, 0);
    place(orange, 9, 5, 0);
    place(chiseled, 10, 5, 0);
    place(orange, 11, 5, 0);
    box(8, -14, 8, 12, -11, 12, cut, cut, false);
    box(8, -10, 8, 12, -10, 12, chiseled, chiseled, false);
    box(8, -9, 8, 12, -9, 12, cut, cut, false);
    box(8, -8, 8, 12, -1, 12, sandstone, sandstone, false);
    box(9, -11, 9, 11, -1, 11, air, air, false);
    place(pressure, 10, -11, 10);
    box(9, -13, 9, 11, -13, 11, tnt, air, false);
    place(air, 8, -11, 10);
    place(air, 8, -10, 10);
    place(chiseled, 7, -10, 10);
    place(cut, 7, -11, 10);
    place(air, 12, -11, 10);
    place(air, 12, -10, 10);
    place(chiseled, 13, -10, 10);
    place(cut, 13, -11, 10);
    place(air, 10, -11, 8);
    place(air, 10, -10, 8);
    place(chiseled, 10, -10, 7);
    place(cut, 10, -11, 7);
    place(air, 10, -11, 12);
    place(air, 10, -10, 12);
    place(chiseled, 10, -10, 13);
    place(cut, 10, -11, 13);
    std::vector<block_pos> sand_positions;
    const auto record_sand = [&](jint x, jint y, jint z) {
        sand_positions.push_back(local(x, y, z));
    };
    for (jint x = 14; x <= 18; ++x) {
        for (jint y = -3; y <= -1; ++y) {
            for (jint z = 11; z <= 15; ++z) {
                record_sand(x, y, z);
            }
        }
    }
    for (const auto &[x, y, z] :
         std::array<std::tuple<jint, jint, jint>, 8>{{{19, -3, 13},
                                                      {19, -2, 13},
                                                      {13, -3, 13},
                                                      {13, -2, 13},
                                                      {16, -3, 16},
                                                      {16, -2, 16},
                                                      {16, -3, 10},
                                                      {16, -2, 10}}}) {
        record_sand(x, y, z);
    }
    const std::array<std::pair<jint, jint>, 4> chests{
        {{10, 8}, {12, 10}, {10, 12}, {8, 10}}};
    for (const auto &[x, z] : chests) {
        const auto pos = local(x, -11, z);
        (void)place_pyramid_chest(context, pos.x, pos.y, pos.z, random);
    }
    // Cellar stairs and room.
    place(cellar_stairs, 13, -1, 17);
    place(cellar_stairs, 14, -2, 17);
    place(cellar_stairs, 15, -3, 17);
    place(sand, 12, 0, 17);
    place(sand, 13, 0, 17);
    place(sand, 14, 0, 17);
    place(sand, 15, 0, 17);
    place(sand, 16, 0, 17);
    place(sand, 14, -1, 17);
    place(sand, 15, -1, 17);
    place(sandstone, 16, -1, 17);
    place(sand, 15, -2, 17);
    place(sandstone, 16, -2, 17);
    place(sand, 16, -3, 17);
    box(13, -3, 10, 13, -3, 15, cut, cut, true);
    box(19, -3, 10, 19, -3, 15, cut, cut, true);
    box(13, -3, 10, 19, -3, 11, cut, cut, true);
    box(13, -3, 16, 19, -3, 16, cut, cut, true);
    box(13, -2, 10, 13, -2, 15, chiseled, chiseled, true);
    box(19, -2, 10, 19, -2, 15, chiseled, chiseled, true);
    box(13, -2, 10, 19, -2, 11, chiseled, chiseled, true);
    box(13, -2, 16, 19, -2, 16, chiseled, chiseled, true);
    box(13, -1, 10, 13, -1, 15, cut, cut, true);
    box(19, -1, 10, 19, -1, 15, cut, cut, true);
    box(13, -1, 10, 19, -1, 11, cut, cut, true);
    box(13, -1, 16, 19, -1, 16, cut, cut, true);
    place(blue, 16, -4, 13);
    place(orange, 17, -4, 12);
    place(orange, 17, -4, 14);
    place(orange, 15, -4, 12);
    place(orange, 15, -4, 14);
    place(orange, 18, -4, 13);
    place(orange, 14, -4, 13);
    place(orange, 16, -4, 15);
    place(orange, 16, -4, 11);
    place(orange, 19, -4, 13);
    place(orange, 13, -4, 13);
    place(orange, 16, -4, 16);
    place(orange, 16, -4, 10);
    place(cut, 20, -3, 13);
    place(chiseled, 20, -2, 13);
    place(cut, 12, -3, 13);
    place(chiseled, 12, -2, 13);
    place(cut, 16, -3, 17);
    place(chiseled, 16, -2, 17);
    place(cut, 16, -3, 9);
    place(chiseled, 16, -2, 9);
    if (context.level_random != nullptr) {
        const bool variant = context.level_random->next_boolean();
        place(variant ? sand : sandstone, 15, -1, 17);
        place(variant ? sandstone : sand, 16, -1, 17);
        for (jint x = 14; x <= 18; ++x) {
            for (jint z = 11; z <= 15; ++z) {
                if (context.level_random->next_float() < 0.33F) {
                    place(sandstone, x, 0, z);
                } else {
                    place(sand, x, 0, z);
                }
            }
        }
    }
    if (context.seed != 0) {
        legacy_random_source base{context.seed};
        const jlong factory = base.next_long();
        const auto bbox_min = local(0, 0, 0);
        const auto bbox_max = local(20, 14, 20);
        const block_pos center{.x = (bbox_min.x + bbox_max.x) / 2,
                               .y = (bbox_min.y + bbox_max.y) / 2,
                               .z = (bbox_min.z + bbox_max.z) / 2};
        legacy_random_source after{mth_get_seed(center.x, center.y, center.z) ^
                                   factory};
        std::vector<block_pos> unique;
        for (const auto &pos : sand_positions) {
            const bool seen = std::any_of(
                unique.begin(), unique.end(), [&](const block_pos &value) {
                    return value.x == pos.x && value.y == pos.y &&
                           value.z == pos.z;
                });
            if (!seen) {
                unique.push_back(pos);
            }
        }
        std::sort(unique.begin(), unique.end(),
                  [](const block_pos &left, const block_pos &right) {
                      return std::tie(left.y, left.z, left.x) <
                             std::tie(right.y, right.z, right.x);
                  });
        for (std::size_t i = unique.size(); i > 1; --i) {
            const auto swap_to =
                static_cast<std::size_t>(after.next_int(static_cast<jint>(i)));
            std::swap(unique[i - 1], unique[swap_to]);
        }
        jint suspicious =
            std::min(static_cast<jint>(unique.size()), 5 + after.next_int(3));
        const auto suspicious_sand = resolve("minecraft:suspicious_sand");
        const auto inside_chunk = [&](const block_pos &pos) {
            return pos.x >> 4 == context.chunk->chunk_x() &&
                   pos.z >> 4 == context.chunk->chunk_z();
        };
        for (const auto &pos : unique) {
            if (suspicious > 0) {
                --suspicious;
                if (!inside_chunk(pos)) {
                    continue;
                }
                if (suspicious_sand != no_block_state) {
                    (void)region.set_block_state(pos.x, pos.y, pos.z,
                                                 suspicious_sand);
                }
                data::nbt::compound entity;
                entity["id"] =
                    data::nbt::value{std::string{"minecraft:brushable_block"}};
                entity["LootTable"] = data::nbt::value{
                    std::string{"minecraft:archaeology/desert_pyramid"}};
                entity["LootTableSeed"] = data::nbt::value{
                    (static_cast<jlong>(pos.x & 0x3FFFFFF) << 38) |
                    (static_cast<jlong>(pos.z & 0x3FFFFFF) << 12) |
                    static_cast<jlong>(pos.y & 0xFFF)};
                (void)region.set_block_entity(pos.x, pos.y, pos.z,
                                              std::move(entity));
            } else if (inside_chunk(pos)) {
                (void)region.set_block_state(pos.x, pos.y, pos.z,
                                             resolve(sand));
            }
        }
        const auto roof_local = local(14, 0, 11);
        legacy_random_source roof_random{
            mth_get_seed(roof_local.x, roof_local.y, roof_local.z) ^ factory};
        const jint roof_x = 14 + roof_random.next_int(5);
        const jint roof_z = 11 + roof_random.next_int(5);
        const auto roof_pos = local(roof_x, 0, roof_z);
        if (inside_chunk(roof_pos) && suspicious_sand != no_block_state) {
            (void)region.set_block_state(roof_pos.x, roof_pos.y, roof_pos.z,
                                         suspicious_sand);
        }
    }
    (void)base_y;
}

auto piece_local_to_world(const structure_piece_record &piece, jint base_y,
                          jint local_x, jint local_y, jint local_z)
    -> block_pos {
    const auto [wx, wz] = swamp_hut_world_pos(piece, local_x, local_z);
    return block_pos{.x = wx, .y = base_y + local_y, .z = wz};
}

struct mineshaft_access {
    const structure_place_context &context;
    const structure_piece_record &piece;
    block_state_id air{no_block_state};
    block_state_id cave_air{no_block_state};
    block_state_id planks{no_block_state};

    [[nodiscard]] auto inside(jint x, jint y, jint z) const -> bool {
        return (x >> 4) == context.chunk->chunk_x() &&
               (z >> 4) == context.chunk->chunk_z() &&
               y >= context.chunk->min_y() &&
               y < context.chunk->min_y() + context.chunk->height();
    }

    [[nodiscard]] auto get(jint x, jint y, jint z) const -> block_state_id {
        return inside(x, y, z) ? context.region->state_at(x, y, z) : air;
    }

    void set(jint x, jint y, jint z, block_state_id state) const {
        if (inside(x, y, z)) {
            (void)context.region->set_block_state(x, y, z, state);
        }
    }

    void fill(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
              block_state_id state, bool skip_air = false) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    if (!inside(x, y, z)) {
                        continue;
                    }
                    if (skip_air && context.states->traits(get(x, y, z)).air) {
                        continue;
                    }
                    set(x, y, z, state);
                }
            }
        }
    }

    void fill_local(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
                    block_state_id state) const {
        const auto first = piece_local_to_world(piece, piece.min_y, x0, y0, z0);
        const auto last = piece_local_to_world(piece, piece.min_y, x1, y1, z1);
        fill(std::min(first.x, last.x), std::min(first.y, last.y),
             std::min(first.z, last.z), std::max(first.x, last.x),
             std::max(first.y, last.y), std::max(first.z, last.z), state);
    }

    [[nodiscard]] auto interior(jint x, jint y, jint z) const -> bool {
        if (!inside(x, y + 1, z)) {
            return false;
        }
        auto *chunk = context.region->find(x >> 4, z >> 4);
        if (chunk == nullptr) {
            return false;
        }
        return y + 1 < chunk->first_available(heightmap_kind::ocean_floor_wg,
                                              x & 15, z & 15);
    }

    [[nodiscard]] auto sturdy_up(jint x, jint y, jint z) const -> bool {
        return context.states->traits(get(x, y, z)).blocks_motion;
    }

    void planks_block(jint x, jint y, jint z) const {
        if (interior(x, y, z) && !sturdy_up(x, y, z)) {
            set(x, y, z, planks);
        }
    }

    void support_pillar(jint x, jint y0, jint z, jint y1) const {
        if (!context.states->traits(get(x, y1 + 1, z)).air) {
            fill(x, y0, z, x, y1, z, planks);
        }
    }

    [[nodiscard]] auto get_local(jint x, jint y, jint z) const
        -> block_state_id {
        const auto world = piece_local_to_world(piece, piece.min_y, x, y, z);
        return get(world.x, world.y, world.z);
    }

    void set_local(jint x, jint y, jint z, block_state_id state) const {
        const auto world = piece_local_to_world(piece, piece.min_y, x, y, z);
        set(world.x, world.y, world.z, state);
    }

    [[nodiscard]] auto interior_local(jint x, jint y, jint z) const -> bool {
        const auto world = piece_local_to_world(piece, piece.min_y, x, y, z);
        return interior(world.x, world.y, world.z);
    }

    [[nodiscard]] auto name_of(block_state_id state) const -> std::string_view {
        return context.states->name(state);
    }

    [[nodiscard]] auto replaceable_local(jint x, jint y, jint z) const -> bool {
        const auto state = get_local(x, y, z);
        const auto traits = context.states->traits(state);
        const auto name = name_of(state);
        return traits.air || traits.fluid || name == "minecraft:glow_lichen" ||
               name.starts_with("minecraft:seagrass") ||
               name.starts_with("minecraft:tall_seagrass");
    }

    [[nodiscard]] auto planks_at(jint x, jint y, jint z) const -> bool {
        return name_of(get_local(x, y, z)) == name_of(planks);
    }

    void fill_column_local(jint x, jint y0, jint z, jint y1,
                           block_state_id state) const {
        for (jint y = y0; y < y1; ++y) {
            set_local(x, y, z, state);
        }
    }

    void fill_pillar_or_chain(jint x, jint local_y, jint z) const {
        const auto world =
            piece_local_to_world(piece, piece.min_y, x, local_y, z);
        if (!inside(world.x, world.y, world.z)) {
            return;
        }
        jint distance = 1;
        bool check_below = true;
        bool check_above = true;
        while (check_below || check_above) {
            if (check_below) {
                const auto below = get_local(x, local_y - distance, z);
                const bool empty =
                    replaceable_local(x, local_y - distance, z) &&
                    name_of(below) != "minecraft:lava";
                if (!empty && context.states->traits(below).blocks_motion) {
                    fill_column_local(x, local_y - distance + 1, z, local_y,
                                      wood);
                    return;
                }
                check_below = distance <= 20 && empty &&
                              world.y - distance > context.chunk->min_y() + 1;
            }
            if (check_above) {
                const auto above = get_local(x, local_y + distance, z);
                const bool empty = replaceable_local(x, local_y + distance, z);
                const auto above_name = name_of(above);
                const bool can_hang =
                    context.states->traits(above).blocks_motion &&
                    !above_name.starts_with("minecraft:sand") &&
                    !above_name.starts_with("minecraft:gravel") &&
                    !above_name.starts_with("minecraft:anvil");
                if (!empty && can_hang) {
                    set_local(x, local_y + 1, z, fence);
                    fill_column_local(x, local_y + 2, z, local_y + distance,
                                      chain);
                    return;
                }
                check_above = distance <= 50 && empty &&
                              world.y + distance < context.chunk->min_y() +
                                                       context.chunk->height();
            }
            ++distance;
        }
    }

    void support_at(jint x0, jint y0, jint z, jint y1, jint x1,
                    worldgen_random &random) const {
        bool supported = true;
        for (jint x = x0; x <= x1; ++x) {
            if (context.states->traits(get_local(x, y1 + 1, z)).air) {
                supported = false;
            }
        }
        if (!supported) {
            return;
        }
        fill_column_local(x0, y0, z, y1, fence_west);
        fill_column_local(x1, y0, z, y1, fence_east);
        if (random.next_int(4) == 0) {
            set_local(x0, y1, z, planks);
            set_local(x1, y1, z, planks);
        } else {
            for (jint x = x0; x <= x1; ++x) {
                set_local(x, y1, z, planks);
            }
            auto torch = [&](jint tx, jint ty, jint tz, block_state_id state) {
                if (random.next_float() < 0.05F) {
                    set_local(tx, ty, tz, state);
                }
            };
            torch(x0 + 1, y1, z - 1, torch_south);
            torch(x0 + 1, y1, z + 1, torch_north);
        }
    }

    void maybe_box(float probability, jint x0, jint y0, jint z0, jint x1,
                   jint y1, jint z1, block_state_id fill_state,
                   block_state_id edge_state, bool has_to_be_inside,
                   worldgen_random &random) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    if (random.next_float() > probability) {
                        continue;
                    }
                    if (has_to_be_inside && !interior_local(x, y, z)) {
                        continue;
                    }
                    const bool edge = x == x0 || x == x1 || y == y0 ||
                                      y == y1 || z == z0 || z == z1;
                    set_local(x, y, z, edge ? edge_state : fill_state);
                }
            }
        }
    }

    void maybe_block(float probability, jint x, jint y, jint z,
                     block_state_id state, worldgen_random &random) const {
        if (random.next_float() < probability) {
            set_local(x, y, z, state);
        }
    }

    void maybe_cobweb(float probability, jint x, jint y, jint z,
                      worldgen_random &random) const {
        if (!interior_local(x, y, z)) {
            return;
        }
        if (random.next_float() >= probability) {
            return;
        }
        jint sturdy = 0;
        for (const auto &direction : all_directions) {
            const auto neighbour =
                get_local(x + direction.dx, y + direction.dy, z + direction.dz);
            if (context.states->traits(neighbour).blocks_motion) {
                ++sturdy;
            }
        }
        if (sturdy >= 2) {
            set_local(x, y, z, cobweb);
        }
    }

    void planks_floor(jint x, jint y, jint z) const {
        if (interior_local(x, y, z) &&
            !context.states->traits(get_local(x, y, z)).blocks_motion) {
            set_local(x, y, z, planks);
        }
    }

    void create_chest_local(jint x, jint y, jint z,
                            worldgen_random &random) const {
        const auto world = piece_local_to_world(piece, piece.min_y, x, y, z);
        if (!inside(world.x, world.y, world.z)) {
            return;
        }
        if (!context.states->traits(get(world.x, world.y, world.z)).air ||
            context.states->traits(get(world.x, world.y - 1, world.z)).air) {
            return;
        }
        set_local(x, y, z, random.next_boolean() ? rail_ns : rail_ew);
        (void)random.next_long();
    }

    void double_support(jint x, jint y, jint z) const {
        if (planks_at(x, y, z)) {
            fill_pillar_or_chain(x, y, z);
        }
        if (planks_at(x + 2, y, z)) {
            fill_pillar_or_chain(x + 2, y, z);
        }
    }

    block_state_id wood{no_block_state};
    block_state_id fence{no_block_state};
    block_state_id fence_west{no_block_state};
    block_state_id fence_east{no_block_state};
    block_state_id chain{no_block_state};
    block_state_id rail_ns{no_block_state};
    block_state_id rail_ew{no_block_state};
    block_state_id cobweb{no_block_state};
    block_state_id spawner{no_block_state};
    block_state_id torch_south{no_block_state};
    block_state_id torch_north{no_block_state};
};

auto mineshaft_access_for(const structure_place_context &context,
                          const structure_piece_record &piece)
    -> mineshaft_access {
    mineshaft_access access{.context = context, .piece = piece};
    access.air = context.state("minecraft:air");
    access.cave_air = context.state("minecraft:cave_air");
    const std::string wood =
        piece.mesa ? "minecraft:dark_oak" : "minecraft:oak";
    access.planks = context.state(wood + "_planks");
    access.wood = context.state(wood + "_log");
    access.fence = context.state(wood + "_fence");
    access.fence_west =
        context.state(wood + "_fence[east=false,north=false,south=false,"
                             "waterlogged=false,west=true]");
    access.fence_east =
        context.state(wood + "_fence[east=true,north=false,south=false,"
                             "waterlogged=false,west=false]");
    access.chain = context.state("minecraft:iron_chain");
    access.rail_ns = context.state("minecraft:rail[shape=north_south]");
    access.rail_ew = context.state("minecraft:rail[shape=east_west]");
    access.cobweb = context.state("minecraft:cobweb");
    access.spawner = context.state("minecraft:spawner");
    access.torch_south = context.state("minecraft:wall_torch[facing=south]");
    access.torch_north = context.state("minecraft:wall_torch[facing=north]");
    return access;
}

void place_mineshaft_room(const structure_place_context &context,
                          const structure_piece_record &piece) {
    const auto access = mineshaft_access_for(context, piece);
    access.fill(piece.min_x, piece.min_y + 1, piece.min_z, piece.max_x,
                std::min(piece.min_y + 3, piece.max_y), piece.max_z,
                access.cave_air);
    for (const auto &entrance : piece.entrances) {
        access.fill(entrance.min_x, entrance.max_y - 2, entrance.min_z,
                    entrance.max_x, entrance.max_y, entrance.max_z,
                    access.cave_air);
    }
    const jint x0 = piece.min_x;
    const jint y0 = piece.min_y + 4;
    const jint z0 = piece.min_z;
    const jint x1 = piece.max_x;
    const jint y1 = piece.max_y;
    const jint z1 = piece.max_z;
    const float diag_x = static_cast<float>(x1 - x0 + 1);
    const float diag_y = static_cast<float>(y1 - y0 + 1);
    const float diag_z = static_cast<float>(z1 - z0 + 1);
    const float center_x = static_cast<float>(x0) + diag_x / 2.0F;
    const float center_z = static_cast<float>(z0) + diag_z / 2.0F;
    for (jint y = y0; y <= y1; ++y) {
        const float ny = static_cast<float>(y - y0) / diag_y;
        for (jint x = x0; x <= x1; ++x) {
            const float nx =
                (static_cast<float>(x) - center_x) / (diag_x * 0.5F);
            for (jint z = z0; z <= z1; ++z) {
                const float nz =
                    (static_cast<float>(z) - center_z) / (diag_z * 0.5F);
                if (nx * nx + ny * ny + nz * nz <= 1.05F) {
                    access.set(x, y, z, access.cave_air);
                }
            }
        }
    }
}

void place_mineshaft_crossing(const structure_place_context &context,
                              const structure_piece_record &piece) {
    const auto access = mineshaft_access_for(context, piece);
    if (piece.two_floored) {
        access.fill(piece.min_x + 1, piece.min_y, piece.min_z, piece.max_x - 1,
                    piece.min_y + 2, piece.max_z, access.cave_air);
        access.fill(piece.min_x, piece.min_y, piece.min_z + 1, piece.max_x,
                    piece.min_y + 2, piece.max_z - 1, access.cave_air);
        access.fill(piece.min_x + 1, piece.max_y - 2, piece.min_z,
                    piece.max_x - 1, piece.max_y, piece.max_z, access.cave_air);
        access.fill(piece.min_x, piece.max_y - 2, piece.min_z + 1, piece.max_x,
                    piece.max_y, piece.max_z - 1, access.cave_air);
        access.fill(piece.min_x + 1, piece.min_y + 3, piece.min_z + 1,
                    piece.max_x - 1, piece.min_y + 3, piece.max_z - 1,
                    access.cave_air);
    } else {
        access.fill(piece.min_x + 1, piece.min_y, piece.min_z, piece.max_x - 1,
                    piece.max_y, piece.max_z, access.cave_air);
        access.fill(piece.min_x, piece.min_y, piece.min_z + 1, piece.max_x,
                    piece.max_y, piece.max_z - 1, access.cave_air);
    }
    access.support_pillar(piece.min_x + 1, piece.min_y, piece.min_z + 1,
                          piece.max_y);
    access.support_pillar(piece.min_x + 1, piece.min_y, piece.max_z - 1,
                          piece.max_y);
    access.support_pillar(piece.max_x - 1, piece.min_y, piece.min_z + 1,
                          piece.max_y);
    access.support_pillar(piece.max_x - 1, piece.min_y, piece.max_z - 1,
                          piece.max_y);
    for (jint x = piece.min_x; x <= piece.max_x; ++x) {
        for (jint z = piece.min_z; z <= piece.max_z; ++z) {
            access.planks_block(x, piece.min_y - 1, z);
        }
    }
}

void place_mineshaft_corridor(const structure_place_context &context,
                              const structure_piece_record &piece,
                              worldgen_random &random) {
    auto access = mineshaft_access_for(context, piece);
    const jint length = piece.sections * 5 - 1;
    access.fill_local(0, 0, 0, 2, 1, length, access.cave_air);
    access.maybe_box(0.8F, 0, 2, 0, 2, 2, length, access.cave_air,
                     access.cave_air, false, random);
    if (piece.spider) {
        access.maybe_box(0.6F, 0, 0, 0, 2, 1, length, access.cave_air,
                         access.cobweb, true, random);
    }
    bool placed_spider = false;
    for (jint section = 0; section < piece.sections; ++section) {
        const jint z = 2 + section * 5;
        access.support_at(0, 0, z, 2, 2, random);
        access.maybe_cobweb(0.1F, 0, 2, z - 1, random);
        access.maybe_cobweb(0.1F, 2, 2, z - 1, random);
        access.maybe_cobweb(0.1F, 0, 2, z + 1, random);
        access.maybe_cobweb(0.1F, 2, 2, z + 1, random);
        access.maybe_cobweb(0.05F, 0, 2, z - 2, random);
        access.maybe_cobweb(0.05F, 2, 2, z - 2, random);
        access.maybe_cobweb(0.05F, 0, 2, z + 2, random);
        access.maybe_cobweb(0.05F, 2, 2, z + 2, random);
        if (random.next_int(100) == 0) {
            access.create_chest_local(2, 0, z - 1, random);
        }
        if (random.next_int(100) == 0) {
            access.create_chest_local(0, 0, z + 1, random);
        }
        if (piece.spider && !placed_spider) {
            const jint new_z = z - 1 + random.next_int(3);
            if (access.interior_local(1, 0, new_z)) {
                placed_spider = true;
                access.set_local(1, 0, new_z, access.spawner);
            }
        }
    }
    for (jint x = 0; x <= 2; ++x) {
        for (jint z = 0; z <= length; ++z) {
            access.planks_floor(x, -1, z);
        }
    }
    access.double_support(0, -1, 2);
    if (piece.sections > 1) {
        access.double_support(0, -1, length - 2);
    }
    if (piece.rails) {
        for (jint z = 0; z <= length; ++z) {
            const auto floor = access.get_local(1, -1, z);
            if (!context.states->traits(floor).air &&
                context.states->traits(floor).blocks_motion) {
                const float probability =
                    access.interior_local(1, 0, z) ? 0.7F : 0.9F;
                access.maybe_block(probability, 1, 0, z, access.rail_ns,
                                   random);
            }
        }
    }
}

void place_mineshaft_stairs(const structure_place_context &context,
                            const structure_piece_record &piece) {
    const auto access = mineshaft_access_for(context, piece);
    access.fill_local(0, 5, 0, 2, 7, 1, access.cave_air);
    access.fill_local(0, 0, 7, 2, 2, 8, access.cave_air);
    for (jint i = 0; i < 5; ++i) {
        const jint y0 = 5 - i - (i < 4 ? 1 : 0);
        access.fill_local(0, y0, 2 + i, 2, 7 - i, 2 + i, access.cave_air);
    }
}

void place_ruined_portal(const structure_place_context &context,
                         const structure_piece_record &piece,
                         worldgen_random &random) {
    if (context.templates == nullptr || !piece.portal_valid) {
        return;
    }
    const auto &templ = context.templates->get_or_create(piece.template_id);
    if (templ.palettes.empty()) {
        return;
    }
    const auto &props = piece;
    const auto resolve = [&](std::string_view key) -> block_state_id {
        return context.state(key);
    };
    const auto air = resolve("minecraft:air");
    const auto netherrack = resolve("minecraft:netherrack");
    const auto magma = resolve("minecraft:magma_block");
    const auto lava = resolve("minecraft:lava");

    block_ignore_processor ignore{
        props.portal_air_pocket
            ? std::vector<std::string>{"minecraft:structure_block"}
            : std::vector<std::string>{"minecraft:structure_block",
                                       "minecraft:air"}};
    std::vector<template_rule> rules;
    rules.push_back(template_rule{.blocks = {"minecraft:gold_block"},
                                  .probability = 0.3F,
                                  .output = air});
    if (props.portal_placement == "on_ocean_floor") {
        rules.push_back(template_rule{.blocks = {"minecraft:lava"},
                                      .probability = -1.0F,
                                      .output = magma});
    } else if (props.portal_cold) {
        rules.push_back(template_rule{.blocks = {"minecraft:lava"},
                                      .probability = -1.0F,
                                      .output = netherrack});
    } else {
        rules.push_back(template_rule{.blocks = {"minecraft:lava"},
                                      .probability = 0.2F,
                                      .output = magma});
    }
    if (!props.portal_cold) {
        rules.push_back(template_rule{.blocks = {"minecraft:netherrack"},
                                      .probability = 0.07F,
                                      .output = magma});
    }
    rule_processor rule_processor_{std::move(rules)};
    block_age_processor age{props.portal_mossiness};
    protected_block_processor protected_blocks{
        "minecraft:features_cannot_replace"};
    lava_submerged_processor submerged{lava};
    blackstone_replace_processor blackstone;
    std::vector<const template_block_processor *> processors{
        &ignore, &rule_processor_, &age, &protected_blocks, &submerged};
    if (props.portal_blackstone) {
        processors.push_back(&blackstone);
    }
    template_place_settings settings{};
    settings.transform = template_transform{
        .mirror = static_cast<template_mirror>(piece.template_mirror),
        .rotation = static_cast<template_rotation>(piece.template_rotation),
        .pivot = block_pos{.x = piece.template_pivot_x,
                           .y = piece.template_pivot_y,
                           .z = piece.template_pivot_z},
    };
    settings.bounds = template_bounds{
        .min_x = context.chunk->chunk_x() * 16,
        .min_y = context.chunk->min_y(),
        .min_z = context.chunk->chunk_z() * 16,
        .max_x = context.chunk->chunk_x() * 16 + 15,
        .max_y = context.chunk->min_y() + context.chunk->height() - 1,
        .max_z = context.chunk->chunk_z() * 16 + 15,
    };
    settings.processors = std::move(processors);
    settings.loot_random = &random;
    const block_pos origin{
        .x = piece.template_x, .y = piece.template_y, .z = piece.template_z};
    (void)place_template(*context.region, *context.registry, templ, origin,
                         settings);

    const auto &states = *context.registry;
    const jint chunk_center_x = (piece.min_x + piece.max_x) / 2;
    const jint chunk_center_z = (piece.min_z + piece.max_z) / 2;
    if ((chunk_center_x >> 4) != context.chunk->chunk_x() ||
        (chunk_center_z >> 4) != context.chunk->chunk_z()) {
        return;
    }
    const auto height_kind = props.portal_placement == "on_ocean_floor"
                                 ? heightmap_kind::ocean_floor_wg
                                 : heightmap_kind::world_surface_wg;
    const auto height_at = [&](jint x, jint z) -> std::optional<jint> {
        auto *column = context.region->find(x >> 4, z >> 4);
        if (column == nullptr) {
            return std::nullopt;
        }
        return column->first_available(height_kind, x & 15, z & 15);
    };
    const auto in_protected_tag = [&](block_state_id state) {
        return states.in_tag(state, "minecraft:features_cannot_replace");
    };
    const auto can_replace = [&](block_state_id state) -> bool {
        const auto name = states.name(state);
        if (name == "minecraft:air" || name == "minecraft:obsidian" ||
            in_protected_tag(state)) {
            return false;
        }
        return props.portal_placement == "in_nether" ||
               name != "minecraft:lava";
    };
    const auto replaceable_at = [&](jint x, jint y, jint z) {
        return can_replace(context.region->state_at(x, y, z));
    };
    const auto place_netherrack = [&](jint x, jint y, jint z) {
        if (!replaceable_at(x, y, z)) {
            return;
        }
        const auto state = !props.portal_cold && random.next_float() < 0.07F
                               ? magma
                               : netherrack;
        (void)context.region->set_block_state(x, y, z, state);
    };
    const auto drip = [&](jint x, jint y, jint z) {
        jint remaining = 8;
        place_netherrack(x, y, z);
        while (remaining > 0 && random.next_float() < 0.5F) {
            --remaining;
            --y;
            place_netherrack(x, y, z);
        }
    };
    const jint min_x = piece.min_x;
    const jint max_x = piece.max_x;
    const jint min_y = piece.min_y;
    const jint max_y = piece.max_y;
    const jint min_z = piece.min_z;
    const jint max_z = piece.max_z;
    const bool follow_surface = props.portal_placement == "on_land_surface" ||
                                props.portal_placement == "on_ocean_floor";
    static constexpr std::array<float, 14> probabilities{
        1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
        0.9F, 0.9F, 0.8F, 0.7F, 0.6F, 0.4F, 0.2F};
    const jint center_x = (min_x + max_x) / 2;
    const jint center_z = (min_z + max_z) / 2;
    const jint width = (max_x - min_x + 1 + max_z - min_z + 1) / 2;
    const jint adjustment = random.next_int(std::max(1, 8 - width / 2));
    const jint max_distance = static_cast<jint>(probabilities.size());
    for (jint x = center_x - max_distance; x <= center_x + max_distance; ++x) {
        for (jint z = center_z - max_distance; z <= center_z + max_distance;
             ++z) {
            const jint distance =
                std::abs(x - center_x) + std::abs(z - center_z);
            const jint adjusted = std::max(0, distance + adjustment);
            if (adjusted >= max_distance) {
                continue;
            }
            if (random.next_double() >= probabilities[adjusted]) {
                continue;
            }
            const auto surface = height_at(x, z);
            if (!surface) {
                continue;
            }
            const jint surface_y = *surface - 1;
            const jint y =
                follow_surface ? surface_y : std::min(min_y, surface_y);
            if (std::abs(y - min_y) > 3 || !replaceable_at(x, y, z)) {
                continue;
            }
            place_netherrack(x, y, z);
            if (props.portal_overgrown) {
                if (random.next_float() < 0.5F &&
                    states.name(context.region->state_at(x, y, z)) ==
                        "minecraft:netherrack" &&
                    states.traits(context.region->state_at(x, y + 1, z)).air) {
                    const auto leaves = resolve(
                        "minecraft:jungle_leaves[distance=7,persistent=true]");
                    (void)context.region->set_block_state(x, y + 1, z, leaves);
                }
            }
            drip(x, y - 1, z);
        }
    }
    for (jint x = min_x + 1; x < max_x; ++x) {
        for (jint z = min_z + 1; z < max_z; ++z) {
            if (states.name(context.region->state_at(x, min_y, z)) ==
                "minecraft:netherrack") {
                drip(x, min_y - 1, z);
            }
        }
    }
    if (props.portal_vines || props.portal_overgrown) {
        for (jint y = min_y; y <= max_y; ++y) {
            for (jint z = min_z; z <= max_z; ++z) {
                for (jint x = min_x; x <= max_x; ++x) {
                    const auto state = context.region->state_at(x, y, z);
                    const auto name = states.name(state);
                    if (name != "minecraft:air" && name != "minecraft:vine") {
                        static constexpr std::array<const char *, 4> faces{
                            "north", "east", "south", "west"};
                        static constexpr std::array<std::pair<jint, jint>, 4>
                            offsets{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
                        const auto face =
                            static_cast<std::size_t>(random.next_int(4));
                        const auto [dx, dz] = offsets[face];
                        const auto neighbour =
                            context.region->state_at(x + dx, y, z + dz);
                        if (states.traits(neighbour).air &&
                            states.traits(state).blocks_motion) {
                            const std::string key =
                                std::string{"minecraft:vine["} +
                                faces[(face + 2) % 4] + "=true]";
                            const auto vine = context.registry->intern(
                                key, {.air = false, .blocks_motion = false});
                            if (vine) {
                                (void)context.region->set_block_state(
                                    x + dx, y, z + dz, *vine);
                            }
                        }
                    }
                    if (props.portal_overgrown && random.next_float() < 0.5F &&
                        name == "minecraft:netherrack" &&
                        states.traits(context.region->state_at(x, y + 1, z))
                            .air) {
                        const auto leaves =
                            resolve("minecraft:jungle_leaves[distance=7,"
                                    "persistent=true]");
                        (void)context.region->set_block_state(x, y + 1, z,
                                                              leaves);
                    }
                }
            }
        }
    }
}

void place_structure_piece(const structure_place_context &context,
                           const structure_piece_record &piece,
                           worldgen_random &random) {
    if (piece.type == "minecraft:btp") {
        place_buried_treasure(context, piece, random);
    } else if (piece.type == "minecraft:igl") {
        place_igloo_piece(context, piece, random);
    } else if (piece.type == "minecraft:tesh") {
        place_swamp_hut(context, piece);
    } else if (piece.type == "minecraft:tedp") {
        place_desert_pyramid(context, piece, random);
    } else if (piece.type == "minecraft:shipwreck") {
        place_shipwreck_piece(context, piece, random);
    } else if (piece.type == "minecraft:ocean_ruin") {
        place_ocean_ruin_piece(context, piece, random);
    } else if (piece.type == "minecraft:tejp") {
        place_jungle_temple(context, piece, random);
    } else if (piece.type == "minecraft:ruined_portal") {
        place_ruined_portal(context, piece, random);
    } else if (piece.type == "minecraft:msroom") {
        place_mineshaft_room(context, piece);
    } else if (piece.type == "minecraft:mscrossing") {
        place_mineshaft_crossing(context, piece);
    } else if (piece.type == "minecraft:msstairs") {
        place_mineshaft_stairs(context, piece);
    } else if (piece.type == "minecraft:mscorridor") {
        place_mineshaft_corridor(context, piece, random);
    }
}

void place_structures(proto_chunk &chunk,
                      const structure_place_context &context,
                      std::string_view step, worldgen_random &random) {
    if (context.region == nullptr || context.chunk == nullptr ||
        context.states == nullptr) {
        return;
    }
    const auto place_pieces =
        [&](const ::refrain::worldgen::structure_start &start) {
            for (const auto &piece : start.pieces) {
                place_structure_piece(context, piece, random);
            }
        };
    // The chunk owns its starts; references only point at starts owned by
    // other chunks.
    for (const auto &start : chunk.structure_starts()) {
        if (start.step == step) {
            place_pieces(start);
        }
    }
    for (const auto &reference : chunk.structure_references()) {
        if (reference.chunk_x == chunk.chunk_x() &&
            reference.chunk_z == chunk.chunk_z()) {
            continue;
        }
        auto *source =
            context.region->find(reference.chunk_x, reference.chunk_z);
        if (source == nullptr) {
            continue;
        }
        for (const auto &start : source->structure_starts()) {
            if (start.structure != reference.structure || start.step != step) {
                continue;
            }
            place_pieces(start);
        }
    }
}

} // namespace refrain::worldgen::structure
