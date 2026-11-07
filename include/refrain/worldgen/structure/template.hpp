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
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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

/// Mirror/rotation/pivot of one placement.
struct template_transform {
    template_mirror mirror{template_mirror::none};
    template_rotation rotation{template_rotation::none};
    block_pos pivot{};
};

/// Axis-aligned block box used to clip a placement to a chunk.
struct template_bounds {
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};

    [[nodiscard]] auto contains(const block_pos &pos) const -> bool {
        return pos.x >= min_x && pos.x <= max_x && pos.y >= min_y &&
               pos.y <= max_y && pos.z >= min_z && pos.z <= max_z;
    }
};

/// `StructureTemplate.getBoundingBox`: the transformed corners of the template
/// moved to `origin`, normalised into a box.
[[nodiscard]] auto template_bounding_box(const structure_template &templ,
                                         const template_transform &transform,
                                         const block_pos &origin)
    -> template_bounds;

/// One block-level processor. `process` returns the replacement entry or
/// nothing when the block is dropped.
class template_block_processor {
  public:
    template_block_processor() = default;
    template_block_processor(const template_block_processor &) = delete;
    auto operator=(const template_block_processor &)
        -> template_block_processor & = delete;
    template_block_processor(template_block_processor &&) = delete;
    auto operator=(template_block_processor &&)
        -> template_block_processor & = delete;
    virtual ~template_block_processor() = default;

    [[nodiscard]] virtual auto process(const block_state_registry &states,
                                       const template_block_info &info,
                                       const block_pos &world) const
        -> std::optional<template_block_info> = 0;
    [[nodiscard]] virtual auto evaluates_entire_piece_state() const -> bool {
        return false;
    }
};

/// `BlockIgnoreProcessor`: drops blocks whose block name is listed.
class block_ignore_processor final : public template_block_processor {
  public:
    explicit block_ignore_processor(std::vector<std::string> blocks)
        : blocks_{std::move(blocks)} {}

    [[nodiscard]] auto process(const block_state_registry &states,
                               const template_block_info &info,
                               const block_pos &world) const
        -> std::optional<template_block_info> override;

  private:
    std::vector<std::string> blocks_{};
};

struct template_place_settings {
    template_transform transform{};
    std::optional<template_bounds> bounds{};
    std::vector<const template_block_processor *> processors{};
    liquid_settings liquids{liquid_settings::apply_waterlogging};
};

/// `StructureTemplate.transform(BlockPos, Mirror, Rotation, pivot)`. Mirroring
/// is applied to the local position first, then the rotation; the pivot only
/// participates in the rotated branches.
[[nodiscard]] auto transform_position(const block_pos &pos,
                                      const template_transform &transform)
    -> block_pos;

/// `BlockState.mirror(mirror).rotate(rotation)` for the direction, axis,
/// 0-15 rotation and face-boolean properties; other properties survive
/// unchanged. The transformed key is resolved against `states` and interned
/// with the source state's traits when it is new.
[[nodiscard]] auto transform_state(block_state_registry &states,
                                   block_state_id state,
                                   const template_transform &transform)
    -> block_state_id;

/// Places palette 0 (or the palette chosen from the template position) into
/// `region`. Blocks are mirrored then rotated, block entities are written after
/// their block, and writes outside the writable region are dropped. Returns
/// the number of blocks that were written.
[[nodiscard]] auto
place_template(chunk_region &region, block_state_registry &states,
               const structure_template &templ, const block_pos &origin,
               const template_place_settings &settings) -> std::size_t;

/// Convenience overload without processors or clipping.
[[nodiscard]] auto
place_template(chunk_region &region, block_state_registry &states,
               const structure_template &templ, const block_pos &origin,
               const template_transform &transform) -> std::size_t;

/// Loads and caches templates from `<root>/<namespace>/structure/<path>.nbt`.
/// A missing or malformed file yields the empty template, exactly like the
/// reference manager's `getOrCreate`.
class structure_template_manager {
  public:
    structure_template_manager(std::string root,
                               const block_state_registry &states)
        : root_{std::move(root)}, states_{&states} {}

    [[nodiscard]] auto get_or_create(std::string_view id)
        -> const structure_template &;

  private:
    std::string root_{};
    const block_state_registry *states_{nullptr};
    std::unordered_map<std::string, structure_template> cache_{};
};

/// `StructureTemplate.load`: resolves the palette against `states` (unknown or
/// malformed states become air) and keeps only entities that carry an NBT
/// payload. The root is expected to be a loaded template compound.
[[nodiscard]] auto load_structure_template(const block_state_registry &states,
                                           const data::nbt::value &root)
    -> result<structure_template>;

} // namespace refrain::worldgen::structure
