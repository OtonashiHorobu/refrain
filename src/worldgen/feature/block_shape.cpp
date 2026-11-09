// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/block_shape.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace refrain::worldgen::feature {

namespace {

auto property(const block_state_table &states, block_state_id state,
              std::string_view name) -> std::string_view {
    const auto value = state_property(states.name(state), name);
    return value.has_value() ? *value : std::string_view{};
}

auto horizontal_clockwise(block_face face) -> block_face {
    switch (face) {
    case block_face::north:
        return block_face::east;
    case block_face::east:
        return block_face::south;
    case block_face::south:
        return block_face::west;
    default:
        return block_face::north;
    }
}

auto horizontal_counter_clockwise(block_face face) -> block_face {
    switch (face) {
    case block_face::north:
        return block_face::west;
    case block_face::west:
        return block_face::south;
    case block_face::south:
        return block_face::east;
    default:
        return block_face::north;
    }
}

/// A stair keeps a full face on its tall side. A straight stair exposes one
/// such side, an inner corner two, and an outer corner none: the corner steps
/// only reach the block edge over half their height.
auto stair_face(const block_state_table &states, block_state_id state,
                block_face face) -> bool {
    auto facing = parse_block_face(property(states, state, "facing"))
                      .value_or(block_face::north);
    const bool bottom = property(states, state, "half") != "top";
    if (face == (bottom ? block_face::down : block_face::up)) {
        return true;
    }
    const auto shape = property(states, state, "shape");
    if (shape.starts_with("outer")) {
        return false;
    }
    if (face == facing) {
        return true;
    }
    if (shape == "inner_left") {
        return face == horizontal_counter_clockwise(facing);
    }
    if (shape == "inner_right") {
        return face == horizontal_clockwise(facing);
    }
    return false;
}

auto slab_face(const block_state_table &states, block_state_id state,
               block_face face) -> bool {
    const auto type = property(states, state, "type");
    if (type == "double") {
        return true;
    }
    return face == (type == "top" ? block_face::up : block_face::down);
}

auto trapdoor_face(const block_state_table &states, block_state_id state,
                   block_face face) -> bool {
    const bool open = property(states, state, "open") == "true";
    if (!open) {
        const bool top = property(states, state, "half") == "top";
        return face == (top ? block_face::up : block_face::down);
    }
    const auto facing = parse_block_face(property(states, state, "facing"))
                            .value_or(block_face::north);
    // An open trapdoor lies against the side opposite the direction it faces,
    // so only that side reaches the block boundary.
    return face == opposite_block_face(facing);
}

auto door_face(const block_state_table &states, block_state_id state,
               block_face face) -> bool {
    auto facing = parse_block_face(property(states, state, "facing"))
                      .value_or(block_face::north);
    if (property(states, state, "open") == "true") {
        facing = property(states, state, "hinge") == "right"
                     ? horizontal_counter_clockwise(facing)
                     : horizontal_clockwise(facing);
    }
    return face == opposite_block_face(facing);
}

} // namespace

auto parse_block_face(std::string_view name) -> std::optional<block_face> {
    if (name == "down") {
        return block_face::down;
    }
    if (name == "up") {
        return block_face::up;
    }
    if (name == "north") {
        return block_face::north;
    }
    if (name == "south") {
        return block_face::south;
    }
    if (name == "west") {
        return block_face::west;
    }
    if (name == "east") {
        return block_face::east;
    }
    return std::nullopt;
}

auto block_face_name(block_face face) -> std::string_view {
    switch (face) {
    case block_face::down:
        return "down";
    case block_face::up:
        return "up";
    case block_face::north:
        return "north";
    case block_face::south:
        return "south";
    case block_face::west:
        return "west";
    default:
        return "east";
    }
}

auto opposite_block_face(block_face face) -> block_face {
    return static_cast<block_face>(static_cast<std::uint8_t>(face) ^ 1U);
}

auto is_sturdy_face(const block_state_table &states, block_state_id state,
                    block_face face) -> bool {
    const auto traits = states.traits(state);
    const auto name = state_block_name(states.name(state));
    if (traits.air) {
        return false;
    }
    if (name == "minecraft:water" || name == "minecraft:lava" ||
        name == "minecraft:bubble_column") {
        return false;
    }
    // The motion tags carry the blocks that count for heightmaps. They also
    // include thin shapes, so check the shape families before trusting them.
    if (!traits.blocks_motion) {
        return false;
    }
    if (states.in_tag(state, "minecraft:speleothems") ||
        states.in_tag(state, "minecraft:cave_vines")) {
        return false;
    }
    // Multiface growth layers have no collision shape at all.
    if (name == "minecraft:sculk_vein" || name == "minecraft:glow_lichen" ||
        name == "minecraft:vine" || name == "minecraft:resin_clump") {
        return false;
    }
    if (states.in_tag(state, "minecraft:slabs")) {
        return slab_face(states, state, face);
    }
    if (states.in_tag(state, "minecraft:stairs")) {
        return stair_face(states, state, face);
    }
    if (states.in_tag(state, "minecraft:trapdoors")) {
        return trapdoor_face(states, state, face);
    }
    if (states.in_tag(state, "minecraft:doors")) {
        return door_face(states, state, face);
    }
    // A wall post, a fence, a fence gate and a pane are never wider than a
    // full block face at the block boundary.
    if (states.in_tag(state, "minecraft:walls") ||
        states.in_tag(state, "minecraft:fences") ||
        states.in_tag(state, "minecraft:fence_gates") ||
        name.ends_with("_pane") || name.ends_with("_bars")) {
        return false;
    }
    if (name.ends_with("_carpet")) {
        return face == block_face::down;
    }
    if (name == "minecraft:snow") {
        const auto layers = property(states, state, "layers");
        if (!layers.empty() && layers != "8") {
            return face == block_face::down;
        }
        return true;
    }
    if (name == "minecraft:dirt_path" || name == "minecraft:farmland") {
        return face == block_face::down;
    }
    // Sculk sensors and shriekers are eight pixels tall: only their bottom
    // face reaches the block boundary.
    if (states.in_tag(state, "minecraft:sculk_growth_inhibitors")) {
        return face == block_face::down;
    }
    return true;
}

} // namespace refrain::worldgen::feature
