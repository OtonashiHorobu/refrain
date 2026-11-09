// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "state_resolution.hpp"

#include <refrain/worldgen/chunk/block_state_codec.hpp>

#include <algorithm>
#include <charconv>
#include <span>
#include <string>

namespace refrain::worldgen::feature {

namespace {

/// True when every property a state key spells out equals the block's default
/// value. A key that names no property, or names one the report does not
/// carry, is not a default-state spelling.
auto spells_default_properties(std::string_view key,
                               std::span<const block_default_property> defaults)
    -> bool {
    const auto open = key.find('[');
    const auto close = key.rfind(']');
    if (open == std::string_view::npos || close == std::string_view::npos ||
        close <= open) {
        return false;
    }
    auto body = key.substr(open + 1, close - open - 1);
    while (!body.empty()) {
        const auto comma = body.find(',');
        const auto entry = body.substr(0, comma);
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const auto name = entry.substr(0, equals);
        const auto value = entry.substr(equals + 1);
        bool matched = false;
        for (const auto &property : defaults) {
            if (property.name == name) {
                matched = property.value == value;
                break;
            }
        }
        if (!matched) {
            return false;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        body = body.substr(comma + 1);
    }
    return true;
}

auto fold_default_state_spelling(block_state_registry &states,
                                 block_state_id state, std::string_view key)
    -> block_state_id {
    if (key.find('[') == std::string_view::npos) {
        return state;
    }
    const auto defaults = states.default_properties(state);
    if (defaults.empty() || !spells_default_properties(key, defaults)) {
        return state;
    }
    const std::string bare{state_block_name(key)};
    if (const auto found = states.find(bare); found.has_value()) {
        return *found;
    }
    const auto interned = states.intern(bare, states.traits(state));
    return interned.has_value() ? *interned : state;
}

auto resolve_state_impl(block_state_registry &states, std::string_view key)
    -> result<block_state_id> {
    // Fluid JSON spells out the default level. The registry keys states by
    // their data spelling, so fold it to the plain block name that terrain
    // fluids use; the two are the same state in the reference.
    std::string canonical{key};
    for (const std::string_view fluid : {"minecraft:water", "minecraft:lava"}) {
        if (key == std::string{fluid} + "[level=0]") {
            canonical = std::string{fluid};
            break;
        }
    }
    // A key that spells out only default property values names the block's
    // default state. The reference palette abbreviates that state as the bare
    // block name, so fold the spelling onto it: the two are the same state,
    // and a chunk palette then carries one name per state.
    if (const auto found = states.find(canonical); found.has_value()) {
        return fold_default_state_spelling(states, *found, canonical);
    }
    key = canonical;
    // Take the motion traits from the block tags, which describe the shapes a
    // heightmap counts. The listed states have a collision shape the tag
    // counts but a heightmap must ignore; a partial snow layer has no
    // collision shape at all.
    auto traits = states.infer_traits(key);
    const auto block = state_block_name(key);
    if (block == "minecraft:snow" &&
        key.find("layers=8") == std::string_view::npos) {
        traits.blocks_motion = false;
        traits.blocks_motion_no_leaves = false;
    }
    const bool non_solid =
        block == "minecraft:end_portal" || block == "minecraft:end_gateway" ||
        block == "minecraft:fire" || block == "minecraft:torch" ||
        block == "minecraft:wall_torch" || block == "minecraft:soul_torch" ||
        block == "minecraft:soul_wall_torch" ||
        block == "minecraft:glow_lichen" || block == "minecraft:sculk_vein";
    if (non_solid) {
        traits.blocks_motion = false;
        traits.blocks_motion_no_leaves = false;
    }
    const auto interned = states.intern(key, traits);
    if (!interned) {
        return interned;
    }
    return fold_default_state_spelling(states, *interned, key);
}

} // namespace

auto resolve_state(block_state_registry &states, std::string_view key)
    -> result<block_state_id> {
    return resolve_state_impl(states, key);
}

} // namespace refrain::worldgen::feature
