// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure templates: the immutable `.nbt` payload a template piece places.
// Blocks are kept in placement order (full blocks, other blocks, then block
// entities, each stable-sorted by y, x, z), because processor randomness and
// loot seeds follow that order.

#pragma once

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace refrain::worldgen::structure {

enum class template_mirror : std::uint8_t {
    none = 0,
    left_right = 1,
    front_back = 2
};

enum class template_rotation : std::uint8_t {
    none = 0,
    clockwise_90 = 1,
    clockwise_180 = 2,
    counterclockwise_90 = 3
};

enum class liquid_settings : std::uint8_t {
    apply_waterlogging = 0,
    ignore_waterlogging = 1
};

/// One template block. `nbt` is present exactly when the file marks a block
/// entity; the tag is shared by every placement of the template.
struct template_block_info {
    jint x{0};
    jint y{0};
    jint z{0};
    block_state_id state{no_block_state};
    std::shared_ptr<const data::nbt::compound> nbt{};
};

struct template_entity_info {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    jint block_x{0};
    jint block_y{0};
    jint block_z{0};
    data::nbt::compound nbt{};
};

struct structure_template {
    jint size_x{0};
    jint size_y{0};
    jint size_z{0};

    /// One entry per palette; blocks are in placement order.
    std::vector<std::vector<template_block_info>> palettes{};
    std::vector<template_entity_info> entities{};
};

/// `StructureTemplate.load`: resolves the palette against `states` (unknown or
/// malformed states become air) and keeps only entities that carry an NBT
/// payload. The root is expected to be a loaded template compound.
[[nodiscard]] auto load_structure_template(const block_state_registry &states,
                                           const data::nbt::value &root)
    -> result<structure_template>;

} // namespace refrain::worldgen::structure
