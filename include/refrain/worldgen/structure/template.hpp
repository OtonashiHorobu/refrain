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
#include <refrain/worldgen/structure/placement_types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::structure {

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

/// The transformed corners of the template moved to `origin`, normalised into
/// a box.
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
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> = 0;
    /// Processors that move a block override this. It runs after `process` and
    /// later processors see the moved position.
    [[nodiscard]] virtual auto
    adjust_position(const block_state_registry &states,
                    const chunk_region &region, const template_block_info &info,
                    const block_pos &world) const -> block_pos {
        (void)states;
        (void)region;
        (void)info;
        return world;
    }
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

/// A capped processor over a single rule: up to `limit` matching blocks are
/// replaced, chosen from the whole piece with the positional random at the
/// piece origin.
class capped_rule_processor final : public template_block_processor {
  public:
    capped_rule_processor(std::string input_block, std::string output_state,
                          std::string loot_table, jint limit)
        : input_block_{std::move(input_block)},
          output_state_{std::move(output_state)},
          loot_table_{std::move(loot_table)}, limit_{limit} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
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

/// Drops blocks whose block name is listed.
class block_ignore_processor final : public template_block_processor {
  public:
    explicit block_ignore_processor(std::vector<std::string> blocks)
        : blocks_{std::move(blocks)} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    std::vector<std::string> blocks_{};
};

/// Rule-test kinds a processor rule can use. `block_state_match` compares the
/// full state, `random_*` variants consume one positional draw once the block
/// test passes.
enum class rule_test_type : std::uint8_t {
    always_true,
    block_match,
    random_block_match,
    block_state_match,
    random_block_state_match,
    tag_match,
    all_of,
    any_of,
    not_test,
};

struct rule_test {
    rule_test_type type{rule_test_type::always_true};
    std::vector<std::string> blocks{};
    /// Canonical state key for the `block_state_match` forms.
    std::string state{};
    std::string tag{};
    float probability{-1.0F};
    std::vector<rule_test> children{};
};

/// One processor rule: an input state test, a location test against the world
/// and the replacement state.
struct template_rule {
    rule_test input{};
    rule_test location{};
    block_state_id output{no_block_state};
};

/// The first matching rule replaces the block.
class rule_processor final : public template_block_processor {
  public:
    explicit rule_processor(std::vector<template_rule> rules)
        : rules_{std::move(rules)} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    std::vector<template_rule> rules_{};
};

/// Drops blocks whose current world state is in the protected tag.
class protected_block_processor final : public template_block_processor {
  public:
    explicit protected_block_processor(std::string tag)
        : tag_{std::move(tag)} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    std::string tag_{};
};

/// Restores lava under a non-full template block when the world position
/// currently holds lava.
class lava_submerged_processor final : public template_block_processor {
  public:
    explicit lava_submerged_processor(block_state_id lava) : lava_{lava} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    block_state_id lava_{no_block_state};
};

/// Cracks, mosses or replaces stone-family template blocks using the piece
/// mossiness.
class block_age_processor final : public template_block_processor {
  public:
    explicit block_age_processor(float mossiness) : mossiness_{mossiness} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    float mossiness_{0.0F};
};

/// Maps nether portal materials to blackstone equivalents while keeping the
/// template's properties.
class blackstone_replace_processor final : public template_block_processor {
  public:
    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;
};

/// Replaces a jigsaw block with the `final_state` recorded in its NBT, or
/// drops it for `minecraft:structure_void`.
class jigsaw_replacement_processor final : public template_block_processor {
  public:
    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;
};

/// Moves every processed block to the column height plus the template-local
/// y, used by the `terrain_matching` projection.
class gravity_processor final : public template_block_processor {
  public:
    gravity_processor(heightmap_kind kind, jint offset)
        : kind_{kind}, offset_{offset} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override {
        (void)states;
        (void)region;
        (void)world;
        (void)random;
        return info;
    }
    [[nodiscard]] auto adjust_position(const block_state_registry &states,
                                       const chunk_region &region,
                                       const template_block_info &info,
                                       const block_pos &world) const
        -> block_pos override;

  private:
    heightmap_kind kind_{heightmap_kind::world_surface_wg};
    jint offset_{0};
};

/// Drops a fraction of the blocks using the placement random stream, or a
/// per-position legacy source when the stream is null.
class block_rot_processor final : public template_block_processor {
  public:
    block_rot_processor(float integrity, std::vector<std::string> rottable,
                        bool has_rottable)
        : integrity_{integrity}, rottable_{std::move(rottable)},
          has_rottable_{has_rottable} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
            const template_block_info &info, const block_pos &world,
            worldgen_random *random) const
        -> std::optional<template_block_info> override;

  private:
    float integrity_{1.0F};
    std::vector<std::string> rottable_{};
    bool has_rottable_{false};
};

/// Keeps the template block only when the world block at the position is not
/// protected.
class protected_blocks_processor final : public template_block_processor {
  public:
    explicit protected_blocks_processor(std::string tag)
        : tag_{std::move(tag)} {}

    [[nodiscard]] auto
    process(block_state_registry &states, const chunk_region &region,
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
    /// Shared piece random: loot-table containers consume one seed from it.
    worldgen_random *loot_random{nullptr};
};

/// Transforms a local position: mirroring is applied first, then the rotation;
/// the pivot only participates in the rotated branches.
[[nodiscard]] auto transform_position(const block_pos &pos,
                                      const template_transform &transform)
    -> block_pos;

/// Transforms the direction, axis, 0-15 rotation and face-boolean properties;
/// other properties survive unchanged. The transformed key is resolved against
/// `states` and interned with the source state's traits when it is new.
[[nodiscard]] auto transform_state(block_state_registry &states,
                                   block_state_id state,
                                   const template_transform &transform)
    -> block_state_id;

[[nodiscard]] auto refresh_block_shape(block_state_registry &states,
                                       chunk_region &region,
                                       const block_pos &pos) -> bool;

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
/// reference manager's cache-on-miss behaviour.
struct structure_processor_list {
    std::vector<std::unique_ptr<template_block_processor>> owned{};
    std::vector<const template_block_processor *> processors{};
};

/// Decodes `worldgen/processor_list` JSON. Supported processor types are
/// `rule`, `block_rot`, `protected_blocks`, `block_ignore` and `nop`.
[[nodiscard]] auto decode_processor_list(block_state_registry &states,
                                         const data::json::value &document)
    -> result<std::shared_ptr<structure_processor_list>>;

/// Resolves a JSON block state (`"minecraft:stone"` or
/// `{"id": ..., "properties": {...}}`) to a state id, interning the canonical
/// key with the block's derived traits when it is unknown.
[[nodiscard]] auto decode_block_state(block_state_registry &states,
                                      const data::json::value &document)
    -> block_state_id;

/// Resolves a block-state key such as `minecraft:oak_stairs[facing=north]`.
[[nodiscard]] auto resolve_block_state(block_state_registry &states,
                                       std::string_view key) -> block_state_id;

/// Template size after rotating the footprint.
[[nodiscard]] auto rotated_template_size(const structure_template &templ,
                                         template_rotation rotation)
    -> block_pos;

/// Maps the template zero position through a transform.
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

    /// Decodes and caches `worldgen/processor_list` document `document` for
    /// `id`. A document that fails to decode yields the empty list.
    [[nodiscard]] auto processor_list(std::string_view id,
                                      const data::json::value &document) const
        -> const structure_processor_list &;

    /// The registry the manager resolves template states against.
    [[nodiscard]] auto states() const noexcept -> block_state_registry & {
        return *states_;
    }

  private:
    std::string root_{};
    block_state_registry *states_{nullptr};
    mutable std::unordered_map<std::string, structure_template> cache_{};
    mutable std::unordered_map<std::string,
                               std::shared_ptr<structure_processor_list>>
        processor_cache_{};
};

/// Loads a template: resolves the palette against `states` (unknown or
/// malformed states become air) and keeps only entities that carry an NBT
/// payload. The root is expected to be a loaded template compound.
[[nodiscard]] auto load_structure_template(block_state_registry &states,
                                           const data::nbt::value &root)
    -> result<structure_template>;

} // namespace refrain::worldgen::structure
