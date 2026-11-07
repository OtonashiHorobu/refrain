// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// A configured feature places blocks through the generation region. The
// runtime object is built from `worldgen/feature` once the stage is entered.

#pragma once

#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/feature/block_predicate.hpp>
#include <refrain/worldgen/feature/placement.hpp>
#include <refrain/worldgen/feature/rule_test.hpp>
#include <refrain/worldgen/feature/state_provider.hpp>

namespace refrain::worldgen::feature {

struct feature_context {
    placement_context placement{};
    /// Places a top-level or nested placed feature by id. The decoration stage
    /// supplies this; selectors use it for their nested entries.
    std::function<bool(std::string_view, worldgen_random &, const block_pos &)>
        place_by_id{};
};

class feature {
  public:
    feature() = default;
    feature(const feature &) = delete;
    auto operator=(const feature &) -> feature & = delete;
    feature(feature &&) = delete;
    auto operator=(feature &&) -> feature & = delete;
    virtual ~feature() = default;

    [[nodiscard]] virtual auto place(feature_context &context,
                                     worldgen_random &random,
                                     const block_pos &origin) const -> bool = 0;
};

/// `SimpleBlockFeature` restricted to the plain block-state form.
class simple_block_feature final : public feature {
  public:
    simple_block_feature(block_state_id state, bool schedule_tick)
        : provider_{simple_state(state)}, schedule_tick_{schedule_tick} {}
    simple_block_feature(std::unique_ptr<state_provider> provider,
                         bool schedule_tick)
        : provider_{std::move(provider)}, schedule_tick_{schedule_tick} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &,
                             const block_pos &origin) const -> bool override;

  private:
    std::unique_ptr<state_provider> provider_{};
    bool schedule_tick_{false};
};

/// `DiskFeature`: a flat disk of one state over a target predicate.
class disk_feature final : public feature {
  public:
    disk_feature(block_state_id state, std::unique_ptr<block_predicate> target,
                 std::unique_ptr<provider::int_provider> radius,
                 jint half_height)
        : provider_{simple_state(state)}, target_{std::move(target)},
          radius_{std::move(radius)}, half_height_{half_height} {}
    disk_feature(std::unique_ptr<state_provider> provider,
                 std::unique_ptr<block_predicate> target,
                 std::unique_ptr<provider::int_provider> radius,
                 jint half_height)
        : provider_{std::move(provider)}, target_{std::move(target)},
          radius_{std::move(radius)}, half_height_{half_height} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::unique_ptr<state_provider> provider_{};
    std::unique_ptr<block_predicate> target_{};
    std::unique_ptr<provider::int_provider> radius_{};
    jint half_height_{0};
};

struct block_replacement {
    std::unique_ptr<rule_test> target{};
    block_state_id state{no_block_state};
};

/// `OreFeature`: a randomly oriented ellipsoid family replacing a target rule
/// test. Air-exposure discard is implemented; the bulk section write path only
/// differs in heightmap bookkeeping.
class ore_feature final : public feature {
  public:
    ore_feature(std::vector<block_replacement> targets, jint size,
                float discard_chance_on_air_exposure)
        : targets_{std::move(targets)}, size_{size},
          discard_chance_on_air_exposure_{discard_chance_on_air_exposure} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto place_ore(feature_context &context,
                                 worldgen_random &random, double x0, double x1,
                                 double z0, double z1, double y0, double y1,
                                 jint x_start, jint y_start, jint z_start,
                                 jint size_xz, jint size_y) const -> bool;
    [[nodiscard]] auto can_place(feature_context &context,
                                 worldgen_random &random, block_state_id state,
                                 const block_pos &pos,
                                 const rule_test &target) const -> bool;
    [[nodiscard]] auto adjacent_to_air(feature_context &context,
                                       const block_pos &pos) const -> bool;
    [[nodiscard]] auto height_at(feature_context &context, jint x, jint z) const
        -> jint;

    std::vector<block_replacement> targets_{};
    jint size_{0};
    float discard_chance_on_air_exposure_{0.0F};
};

/// `SequenceFeature`: runs nested placed features in order and stops at the
/// first failure.
class sequence_feature final : public feature {
  public:
    explicit sequence_feature(std::vector<std::string> features)
        : features_{std::move(features)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::string> features_{};
};

/// `ReplaceBlobsFeature`: finds the target block below the origin and replaces
/// a Manhattan-radius ellipsoid of it.
class replace_blobs_feature final : public feature {
  public:
    replace_blobs_feature(block_state_id target, block_state_id replacement,
                          std::unique_ptr<provider::int_provider> radius)
        : target_{target}, replacement_{replacement},
          radius_{std::move(radius)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id target_{no_block_state};
    block_state_id replacement_{no_block_state};
    std::unique_ptr<provider::int_provider> radius_{};
};

/// `BlueIceFeature`: grows a blue-ice blob from water next to packed ice.
class blue_ice_feature final : public feature {
  public:
    blue_ice_feature(block_state_id blue_ice, block_state_id water,
                     block_state_id packed_ice, block_state_id ice,
                     block_state_id air)
        : blue_ice_{blue_ice}, water_{water}, packed_ice_{packed_ice},
          ice_{ice}, air_{air} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id blue_ice_{no_block_state};
    block_state_id water_{no_block_state};
    block_state_id packed_ice_{no_block_state};
    block_state_id ice_{no_block_state};
    block_state_id air_{no_block_state};
};

/// `ScatteredOreFeature`: a handful of single-block ore replacements close to
/// the origin.
class scattered_ore_feature final : public feature {
  public:
    scattered_ore_feature(std::vector<block_replacement> targets, jint size,
                          float discard_chance_on_air_exposure)
        : targets_{std::move(targets)}, size_{size},
          discard_chance_on_air_exposure_{discard_chance_on_air_exposure} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto can_place(feature_context &context,
                                 worldgen_random &random, block_state_id state,
                                 const block_pos &pos,
                                 const rule_test &target) const -> bool;
    [[nodiscard]] auto adjacent_to_air(feature_context &context,
                                       const block_pos &pos) const -> bool;

    std::vector<block_replacement> targets_{};
    jint size_{0};
    float discard_chance_on_air_exposure_{0.0F};
};

/// `RandomNeighborSpreadFeature`: writes the origin block, then scatters more
/// of it next to exactly one accepted neighbour.
class random_neighbor_spread_feature final : public feature {
  public:
    random_neighbor_spread_feature(
        std::unique_ptr<state_provider> block,
        std::vector<std::string> accepted_neighbors,
        std::unique_ptr<block_predicate> can_replace,
        std::unique_ptr<provider::int_provider> attempts,
        std::unique_ptr<provider::int_provider> xz_offset,
        std::unique_ptr<provider::int_provider> y_offset)
        : block_{std::move(block)},
          accepted_neighbors_{std::move(accepted_neighbors)},
          can_replace_{std::move(can_replace)}, attempts_{std::move(attempts)},
          xz_offset_{std::move(xz_offset)}, y_offset_{std::move(y_offset)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto accepted(const block_state_table &states,
                                block_state_id state) const -> bool;

    std::unique_ptr<state_provider> block_{};
    std::vector<std::string> accepted_neighbors_{};
    std::unique_ptr<block_predicate> can_replace_{};
    std::unique_ptr<provider::int_provider> attempts_{};
    std::unique_ptr<provider::int_provider> xz_offset_{};
    std::unique_ptr<provider::int_provider> y_offset_{};
};

/// `SpringFeature`: a fluid source surrounded by a fixed rock/hole pattern.
class spring_feature final : public feature {
  public:
    spring_feature(block_state_id state, bool requires_block_below,
                   jint rock_count, jint hole_count,
                   std::vector<std::string> valid_blocks)
        : state_{state}, requires_block_below_{requires_block_below},
          rock_count_{rock_count}, hole_count_{hole_count},
          valid_blocks_{std::move(valid_blocks)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto valid(feature_context &context,
                             const block_pos &pos) const -> bool;

    block_state_id state_{no_block_state};
    bool requires_block_below_{true};
    jint rock_count_{4};
    jint hole_count_{1};
    std::vector<std::string> valid_blocks_{};
};

/// `BlockBlobFeature`: three random blobs anchored on a placeable block.
class block_blob_feature final : public feature {
  public:
    block_blob_feature(block_state_id state,
                       std::unique_ptr<block_predicate> can_place_on)
        : provider_{simple_state(state)},
          can_place_on_{std::move(can_place_on)} {}
    block_blob_feature(std::unique_ptr<state_provider> provider,
                       std::unique_ptr<block_predicate> can_place_on)
        : provider_{std::move(provider)},
          can_place_on_{std::move(can_place_on)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::unique_ptr<state_provider> provider_{};
    std::unique_ptr<block_predicate> can_place_on_{};
};

enum class block_direction : std::uint8_t {
    down = 0,
    up = 1,
    north = 2,
    south = 3,
    west = 4,
    east = 5,
};

[[nodiscard]] auto offset(block_direction direction, const block_pos &pos)
    -> block_pos;

/// `BlockColumnFeature`: layered columns along one axis.
class block_column_feature final : public feature {
  public:
    struct layer {
        std::unique_ptr<provider::int_provider> height{};
        std::unique_ptr<state_provider> provider{};
    };

    block_column_feature(std::vector<layer> layers, block_direction direction,
                         std::unique_ptr<block_predicate> allowed_placement,
                         bool prioritize_tip)
        : layers_{std::move(layers)}, direction_{direction},
          allowed_placement_{std::move(allowed_placement)},
          prioritize_tip_{prioritize_tip} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    static void truncate(std::vector<jint> &heights, jint total_height,
                         jint new_height, bool prioritize_tip);

    std::vector<layer> layers_{};
    block_direction direction_{block_direction::up};
    std::unique_ptr<block_predicate> allowed_placement_{};
    bool prioritize_tip_{false};
};

/// `RandomSelectorFeature`: independent chances, then the default.
class random_selector_feature final : public feature {
  public:
    random_selector_feature(std::vector<std::pair<float, std::string>> entries,
                            std::string default_id)
        : entries_{std::move(entries)}, default_id_{std::move(default_id)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::pair<float, std::string>> entries_{};
    std::string default_id_{};
};

/// `WeightedRandomSelectorFeature`.
class weighted_selector_feature final : public feature {
  public:
    weighted_selector_feature(std::vector<std::pair<jint, std::string>> entries,
                              jint total)
        : entries_{std::move(entries)}, total_{total} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::pair<jint, std::string>> entries_{};
    jint total_{0};
};

/// `SimpleRandomSelectorFeature`.
class simple_selector_feature final : public feature {
  public:
    explicit simple_selector_feature(std::vector<std::string> entries)
        : entries_{std::move(entries)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::string> entries_{};
};

/// `RandomBooleanSelectorFeature`.
class boolean_selector_feature final : public feature {
  public:
    boolean_selector_feature(std::string feature_true,
                             std::string feature_false)
        : feature_true_{std::move(feature_true)},
          feature_false_{std::move(feature_false)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::string feature_true_{};
    std::string feature_false_{};
};

/// `BlockPileFeature`.
class block_pile_feature final : public feature {
  public:
    explicit block_pile_feature(std::unique_ptr<state_provider> provider)
        : provider_{std::move(provider)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto may_place_on(feature_context &context,
                                    const block_pos &pos,
                                    worldgen_random &random) const -> bool;
    void try_place(feature_context &context, const block_pos &pos,
                   worldgen_random &random) const;

    std::unique_ptr<state_provider> provider_{};
};

/// `OverlayFeature`: places every nested placed feature at the same origin.
class overlay_feature final : public feature {
  public:
    explicit overlay_feature(std::vector<std::string> features)
        : features_{std::move(features)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::string> features_{};
};

} // namespace refrain::worldgen::feature
