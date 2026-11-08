// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure templates: the immutable `.nbt` payload a template piece places.
// Blocks are kept in placement order (full blocks, other blocks, then block
// entities, each stable-sorted by y, x, z), because processor randomness and
// loot seeds follow that order.

#pragma once

#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>

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

    [[nodiscard]] virtual auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> = 0;
    [[nodiscard]] virtual auto evaluates_entire_piece_state() const -> bool {
        return false;
    }
    /// Runs over every processed block of the piece before placement.
    virtual void
    finalize_processing(block_state_registry &states, jint level_seed,
                        const block_pos &position,
                        std::vector<template_block_info> &blocks) const {
        (void)states;
        (void)level_seed;
        (void)position;
        (void)blocks;
    }
};

/// `CappedProcessor` over a single `RuleProcessor` rule: up to `limit`
/// matching blocks are replaced, chosen from the whole piece with the
/// positional random at the piece origin.
class capped_rule_processor final : public template_block_processor {
  public:
    capped_rule_processor(std::string input_block, std::string output_state,
                          std::string loot_table, jint limit)
        : input_block_{std::move(input_block)},
          output_state_{std::move(output_state)},
          loot_table_{std::move(loot_table)}, limit_{limit} {}

    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override {
        (void)states;
        (void)region;
        (void)world;
        (void)random;
        return info;
    }
    [[nodiscard]] auto evaluates_entire_piece_state() const -> bool override {
        return true;
    }
    void finalize_processing(
        block_state_registry &states, jint level_seed,
        const block_pos &position,
        std::vector<template_block_info> &blocks) const override;

  private:
    std::string input_block_{};
    std::string output_state_{};
    std::string loot_table_{};
    jint limit_{0};
};

/// `BlockIgnoreProcessor`: drops blocks whose block name is listed.
class block_ignore_processor final : public template_block_processor {
  public:
    explicit block_ignore_processor(std::vector<std::string> blocks)
        : blocks_{std::move(blocks)} {}

    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    std::vector<std::string> blocks_{};
};

/// One rule of a `RuleProcessor`: a block-name or tag test with an optional
/// probability (the `random_block_match` form).
struct template_rule {
    std::vector<std::string> blocks{};
    std::string tag{};
    /// Negative means the rule matches unconditionally after the block test.
    float probability{-1.0F};
    block_state_id output{no_block_state};
};

/// `RuleProcessor`: the first matching rule replaces the block.
class rule_processor final : public template_block_processor {
  public:
    explicit rule_processor(std::vector<template_rule> rules)
        : rules_{std::move(rules)} {}

    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    std::vector<template_rule> rules_{};
};

/// `ProtectedBlockProcessor`: drops blocks whose current world state is in the
/// protected tag.
class protected_block_processor final : public template_block_processor {
  public:
    explicit protected_block_processor(std::string tag)
        : tag_{std::move(tag)} {}

    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    std::string tag_{};
};

/// `LavaSubmergedBlockProcessor`: restores lava under a non-full template
/// block when the world position currently holds lava.
class lava_submerged_processor final : public template_block_processor {
  public:
    explicit lava_submerged_processor(block_state_id lava) : lava_{lava} {}

    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    block_state_id lava_{no_block_state};
};

/// `BlackstoneReplaceProcessor`: maps nether portal materials to blackstone
/// equivalents while keeping the template's properties.
class blackstone_replace_processor final : public template_block_processor {
  public:
    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;
};

/// `BlockRotProcessor`: drops a fraction of the blocks using the placement
/// random stream, or a per-position legacy source when the stream is null.
class block_rot_processor final : public template_block_processor {
  public:
    block_rot_processor(float integrity, std::vector<std::string> rottable,
                        bool has_rottable)
        : integrity_{integrity}, rottable_{std::move(rottable)},
          has_rottable_{has_rottable} {}

    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    float integrity_{1.0F};
    std::vector<std::string> rottable_{};
    bool has_rottable_{false};
};

/// `ProtectedBlockProcessor`: keeps the template block only when the world
/// block at the position is not protected.
class protected_blocks_processor final : public template_block_processor {
  public:
    explicit protected_blocks_processor(std::string tag)
        : tag_{std::move(tag)} {}

    [[nodiscard]] auto
    process(const block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    std::string tag_{};
};

struct template_place_settings {
    template_transform transform{};
    std::optional<template_bounds> bounds{};
    std::vector<const template_block_processor *> processors{};
    liquid_settings liquids{liquid_settings::apply_waterlogging};
    /// Shared placement stream; processors and multi-palette selection use it
    /// when non-null, otherwise they seed a legacy source per position.
    worldgen_random *random{nullptr};
    /// World seed used by whole-piece processors with a positional random.
    jlong level_seed{0};
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
struct structure_processor_list {
    std::vector<std::unique_ptr<template_block_processor>> owned{};
    std::vector<const template_block_processor *> processors{};
};

/// Decodes `worldgen/processor_list` JSON. Supported processor types are
/// `block_rot`, `protected_blocks` and `block_ignore`; `nop` yields nothing.
[[nodiscard]] auto decode_processor_list(block_state_registry &states,
                                         const data::json::value &document)
    -> result<std::shared_ptr<structure_processor_list>>;

/// `StructureTemplate.getSize(rotation)`.
[[nodiscard]] auto rotated_template_size(const structure_template &templ,
                                         template_rotation rotation)
    -> block_pos;

/// `StructureTemplate.getZeroPositionWithTransform`.
[[nodiscard]] auto zero_position_with_transform(const structure_template &templ,
                                                const block_pos &zero,
                                                template_mirror mirror,
                                                template_rotation rotation)
    -> block_pos;

class structure_template_manager {
  public:
    structure_template_manager(std::string root, block_state_registry &states)
        : root_{std::move(root)}, states_{&states} {}

    [[nodiscard]] auto get_or_create(std::string_view id) const
        -> const structure_template &;

  private:
    std::string root_{};
    block_state_registry *states_{nullptr};
    mutable std::unordered_map<std::string, structure_template> cache_{};
};

/// `StructureTemplate.load`: resolves the palette against `states` (unknown or
/// malformed states become air) and keeps only entities that carry an NBT
/// payload. The root is expected to be a loaded template compound.
[[nodiscard]] auto load_structure_template(block_state_registry &states,
                                           const data::nbt::value &root)
    -> result<structure_template>;

} // namespace refrain::worldgen::structure
