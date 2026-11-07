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

void place_structures(proto_chunk &chunk,
                      const structure_place_context &context,
                      std::string_view step, worldgen_random &random) {
    if (context.region == nullptr || context.chunk == nullptr ||
        context.states == nullptr) {
        return;
    }
    for (const auto &reference : chunk.structure_references()) {
        auto *source =
            context.region->find(reference.chunk_x, reference.chunk_z);
        if (source == nullptr) {
            continue;
        }
        for (const auto &start : source->structure_starts()) {
            if (start.structure != reference.structure || start.step != step) {
                continue;
            }
            for (const auto &piece : start.pieces) {
                if (piece.type == "minecraft:btp") {
                    place_buried_treasure(context, piece, random);
                }
            }
        }
    }
}

} // namespace refrain::worldgen::structure
