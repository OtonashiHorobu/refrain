// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/placement_stage.hpp>

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
                if (piece.type == "minecraft:btp") {
                    place_buried_treasure(context, piece, random);
                } else if (piece.type == "minecraft:igl") {
                    place_igloo_piece(context, piece, random);
                } else if (piece.type == "minecraft:tesh") {
                    place_swamp_hut(context, piece);
                } else if (piece.type == "minecraft:tedp") {
                    place_desert_pyramid(context, piece, random);
                }
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
