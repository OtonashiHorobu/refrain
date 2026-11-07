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

#include <array>

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

/// `BambooFeature`: a bamboo stalk with a random height and optional podzol
/// patch below it.
class bamboo_feature final : public feature {
  public:
    bamboo_feature(float probability, block_state_id trunk,
                   block_state_id final_large, block_state_id top_large,
                   block_state_id top_small, block_state_id podzol)
        : probability_{probability}, trunk_{trunk}, final_large_{final_large},
          top_large_{top_large}, top_small_{top_small}, podzol_{podzol} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    float probability_{0.0F};
    block_state_id trunk_{no_block_state};
    block_state_id final_large_{no_block_state};
    block_state_id top_large_{no_block_state};
    block_state_id top_small_{no_block_state};
    block_state_id podzol_{no_block_state};
};

/// `VinesFeature`: one vine block attached to the first acceptable horizontal
/// or upward neighbour.
class vines_feature final : public feature {
  public:
    /// Indexed by `Direction.get3DDataValue()`: down, up, north, south, west,
    /// east. The down entry is unused.
    explicit vines_feature(std::array<block_state_id, 6> states)
        : states_{states} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::array<block_state_id, 6> states_{};
};

/// `HugeFungusFeature`: crimson/warped stem plus cap, with optional weeping
/// vines under the cap.
class huge_fungus_feature final : public feature {
  public:
    huge_fungus_feature(block_state_id base, block_state_id stem,
                        block_state_id hat, block_state_id decor,
                        std::array<block_state_id, 3> weeping_heads,
                        block_state_id weeping_plant,
                        std::unique_ptr<block_predicate> replaceable,
                        bool planted, bool place_vines)
        : base_{base}, stem_{stem}, hat_{hat}, decor_{decor},
          weeping_heads_{weeping_heads}, weeping_plant_{weeping_plant},
          replaceable_{std::move(replaceable)}, planted_{planted},
          place_vines_{place_vines} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto replaceable(feature_context &context,
                                   const block_pos &pos,
                                   bool check_plants) const -> bool;
    void place_stem(feature_context &context, worldgen_random &random,
                    const block_pos &origin, jint total_height,
                    bool huge) const;
    void place_hat(feature_context &context, worldgen_random &random,
                   const block_pos &origin, jint total_height, bool huge) const;
    void place_hat_block(feature_context &context, worldgen_random &random,
                         const block_pos &pos, float decor_chance,
                         float hat_chance, float vines_chance) const;
    void place_hat_drop(feature_context &context, worldgen_random &random,
                        const block_pos &pos, bool vines) const;
    void try_weeping_vines(feature_context &context, worldgen_random &random,
                           const block_pos &pos) const;

    block_state_id base_{no_block_state};
    block_state_id stem_{no_block_state};
    block_state_id hat_{no_block_state};
    block_state_id decor_{no_block_state};
    std::array<block_state_id, 3> weeping_heads_{};
    block_state_id weeping_plant_{no_block_state};
    std::unique_ptr<block_predicate> replaceable_{};
    bool planted_{false};
    bool place_vines_{false};
};

/// `HugeRedMushroomFeature` / `HugeBrownMushroomFeature`: a stem with a
/// property-shaped cap.
class huge_mushroom_feature final : public feature {
  public:
    huge_mushroom_feature(bool red, std::unique_ptr<state_provider> cap,
                          std::unique_ptr<state_provider> stem,
                          jint foliage_radius,
                          std::unique_ptr<block_predicate> can_place_on)
        : red_{red}, cap_{std::move(cap)}, stem_{std::move(stem)},
          foliage_radius_{foliage_radius},
          can_place_on_{std::move(can_place_on)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto radius_for_height(jint height, jint y) const -> jint;
    auto make_cap(feature_context &context, worldgen_random &random,
                  const block_pos &origin, jint height) const -> void;
    void place_trunk(feature_context &context, worldgen_random &random,
                     const block_pos &origin, jint height) const;
    void place_mushroom_block(feature_context &context, const block_pos &pos,
                              block_state_id state) const;

    bool red_{false};
    std::unique_ptr<state_provider> cap_{};
    std::unique_ptr<state_provider> stem_{};
    jint foliage_radius_{2};
    std::unique_ptr<block_predicate> can_place_on_{};
};

/// `IcebergFeature`: an ellipse/circle iceberg centred on sea level.
class iceberg_feature final : public feature {
  public:
    iceberg_feature(block_state_id state, block_state_id snow_block,
                    block_state_id snow_layer, block_state_id ice,
                    block_state_id packed_ice, block_state_id blue_ice,
                    block_state_id water)
        : state_{state}, snow_block_{snow_block}, snow_layer_{snow_layer},
          ice_{ice}, packed_ice_{packed_ice}, blue_ice_{blue_ice},
          water_{water} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto is_iceberg(block_state_id state) const -> bool;
    [[nodiscard]] auto height_radius_round(worldgen_random &random, jint y,
                                           jint height, jint width) const
        -> jint;
    [[nodiscard]] auto height_radius_ellipse(jint y, jint height,
                                             jint width) const -> jint;
    [[nodiscard]] auto height_radius_steep(worldgen_random &random, jint y,
                                           jint height, jint width) const
        -> jint;
    [[nodiscard]] static auto signed_distance_circle(worldgen_random &random,
                                                     jint x, jint z,
                                                     jint radius) -> double;
    [[nodiscard]] static auto signed_distance_ellipse(jint x, jint z, jint a,
                                                      jint c, double angle)
        -> double;
    void generate_block(feature_context &context, worldgen_random &random,
                        const block_pos &origin, jint height, jint x, jint y,
                        jint z, jint radius, jint a, bool ellipse,
                        jint ellipse_c, double angle, bool snow_on_top) const;
    void set_iceberg_block(feature_context &context, worldgen_random &random,
                           const block_pos &pos, jint h_diff, jint height,
                           bool ellipse, bool snow_on_top) const;
    void smooth(feature_context &context, const block_pos &origin, jint width,
                jint height, bool ellipse, jint ellipse_a) const;
    void carve(feature_context &context, jint radius, jint y,
               const block_pos &origin, bool underwater, double angle,
               const block_pos &local_origin, jint ellipse_a,
               jint ellipse_c) const;
    void generate_cutout(feature_context &context, worldgen_random &random,
                         jint width, jint height, const block_pos &origin,
                         bool ellipse, jint ellipse_a, double angle,
                         jint ellipse_c) const;

    block_state_id state_{no_block_state};
    block_state_id snow_block_{no_block_state};
    block_state_id snow_layer_{no_block_state};
    block_state_id ice_{no_block_state};
    block_state_id packed_ice_{no_block_state};
    block_state_id blue_ice_{no_block_state};
    block_state_id water_{no_block_state};
};

struct geode_parameters {
    std::unique_ptr<state_provider> filling{};
    std::unique_ptr<state_provider> inner{};
    std::unique_ptr<state_provider> alternate_inner{};
    std::unique_ptr<state_provider> middle{};
    std::unique_ptr<state_provider> outer{};
    std::vector<block_state_id> inner_placements{};
    std::string cannot_replace_tag{};
    std::string invalid_blocks_tag{};
    double filling_threshold{1.7};
    double inner_threshold{2.2};
    double middle_threshold{3.2};
    double outer_threshold{4.2};
    double crack_chance{1.0};
    double base_crack_size{2.0};
    jint crack_point_offset{2};
    double use_potential_placements_chance{0.35};
    double use_alternate_layer0_chance{0.0};
    bool placements_require_layer0_alternate{true};
    std::unique_ptr<provider::int_provider> outer_wall_provider{};
    std::unique_ptr<provider::int_provider> distribution_provider{};
    std::unique_ptr<provider::int_provider> point_offset_provider{};
    jint outer_wall_min{4};
    jint outer_wall_max{5};
    jint distribution_min{3};
    jint distribution_max{4};
    jint point_offset_min{1};
    jint point_offset_max{2};
    jint min_gen_offset{-16};
    jint max_gen_offset{16};
    double noise_multiplier{0.05};
    jint invalid_blocks_threshold{1};
};

/// `GeodeFeature`: layered amethyst geode with an optional crack.
class geode_feature final : public feature {
  public:
    explicit geode_feature(geode_parameters parameters)
        : parameters_{std::move(parameters)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto can_replace(feature_context &context,
                                   const block_pos &pos) const -> bool;
    void safe_set(feature_context &context, const block_pos &pos,
                  block_state_id state) const;

    geode_parameters parameters_{};
};

/// `SnowAndFreezeFeature`: freezes the block below the motion-blocking top and
/// covers the top with snow where the biome weather probes say so.
class freeze_top_layer_feature final : public feature {
  public:
    freeze_top_layer_feature(block_state_id ice, block_state_id snow)
        : ice_{ice}, snow_{snow} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id ice_{no_block_state};
    block_state_id snow_{no_block_state};
};

/// `UnderwaterMagmaFeature`: magma blocks buried in the floor under a water
/// column, chosen from a cube around the scanned floor.
class underwater_magma_feature final : public feature {
  public:
    underwater_magma_feature(jint floor_search_range,
                             jint placement_radius_around_floor,
                             float placement_probability, block_state_id magma)
        : floor_search_range_{floor_search_range},
          placement_radius_{placement_radius_around_floor},
          probability_{placement_probability}, magma_{magma} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    jint floor_search_range_{0};
    jint placement_radius_{0};
    float probability_{0.0F};
    block_state_id magma_{no_block_state};
};

/// `LakeFeature`: an ellipsoid lake carved into solid ground, lined with a
/// barrier block and filled with the fluid (the deprecated lava/water lake).
class lake_feature final : public feature {
  public:
    lake_feature(std::unique_ptr<state_provider> fluid,
                 std::unique_ptr<state_provider> barrier,
                 std::unique_ptr<block_predicate> can_place_feature,
                 std::unique_ptr<block_predicate> can_replace_with_air_or_fluid,
                 std::unique_ptr<block_predicate> can_replace_with_barrier,
                 block_state_id cave_air, block_state_id ice)
        : fluid_{std::move(fluid)}, barrier_{std::move(barrier)},
          can_place_feature_{std::move(can_place_feature)},
          can_replace_with_air_or_fluid_{
              std::move(can_replace_with_air_or_fluid)},
          can_replace_with_barrier_{std::move(can_replace_with_barrier)},
          cave_air_{cave_air}, ice_{ice} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::unique_ptr<state_provider> fluid_{};
    std::unique_ptr<state_provider> barrier_{};
    std::unique_ptr<block_predicate> can_place_feature_{};
    std::unique_ptr<block_predicate> can_replace_with_air_or_fluid_{};
    std::unique_ptr<block_predicate> can_replace_with_barrier_{};
    block_state_id cave_air_{no_block_state};
    block_state_id ice_{no_block_state};
};

/// `DeltaFeature`: a lava pool with a magma rim on a flat cave floor.
class delta_feature final : public feature {
  public:
    delta_feature(block_state_id contents, block_state_id rim,
                  std::unique_ptr<provider::int_provider> size,
                  std::unique_ptr<provider::int_provider> rim_size)
        : contents_{contents}, rim_{rim}, size_{std::move(size)},
          rim_size_{std::move(rim_size)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto is_clear(feature_context &context,
                                const block_pos &pos) const -> bool;
    void write(feature_context &context, const block_pos &pos,
               block_state_id state) const;

    block_state_id contents_{no_block_state};
    block_state_id rim_{no_block_state};
    std::unique_ptr<provider::int_provider> size_{};
    std::unique_ptr<provider::int_provider> rim_size_{};
};

/// `LargeDripstoneFeature`: a stalactite/stalagmite pair in a cave column.
class large_dripstone_feature final : public feature {
  public:
    large_dripstone_feature(
        std::string replaceable_tag, jint floor_to_ceiling_range,
        std::unique_ptr<provider::int_provider> radius, jint radius_min,
        jint radius_max, std::unique_ptr<provider::float_provider> height_scale,
        float radius_to_height_ratio,
        std::unique_ptr<provider::float_provider> stalactite_bluntness,
        std::unique_ptr<provider::float_provider> stalagmite_bluntness,
        std::unique_ptr<provider::float_provider> wind_speed,
        jint min_radius_for_wind, float min_bluntness_for_wind)
        : replaceable_tag_{std::move(replaceable_tag)},
          floor_to_ceiling_range_{floor_to_ceiling_range},
          radius_{std::move(radius)}, radius_min_{radius_min},
          radius_max_{radius_max}, height_scale_{std::move(height_scale)},
          radius_to_height_ratio_{radius_to_height_ratio},
          stalactite_bluntness_{std::move(stalactite_bluntness)},
          stalagmite_bluntness_{std::move(stalagmite_bluntness)},
          wind_speed_{std::move(wind_speed)},
          min_radius_for_wind_{min_radius_for_wind},
          min_bluntness_for_wind_{min_bluntness_for_wind} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    struct column_range {
        jint floor{0};
        jint ceiling{0};
    };
    struct dripstone {
        block_pos root{};
        bool pointing_up{false};
        jint radius{0};
        double bluntness{0.0};
        double scale{0.0};
    };
    struct wind_offsetter {
        jint origin_y{0};
        double x{0.0};
        double z{0.0};
        jint max_offset{0};
        bool active{false};

        [[nodiscard]] auto offset(const block_pos &pos) const -> block_pos;
    };

    [[nodiscard]] auto scan(feature_context &context,
                            const block_pos &origin) const
        -> std::optional<column_range>;
    [[nodiscard]] auto height(const dripstone &stone) const -> jint;
    [[nodiscard]] auto height_at(const dripstone &stone, float radius) const
        -> jint;
    [[nodiscard]] auto embedded(feature_context &context,
                                const block_pos &center, jint radius) const
        -> bool;
    [[nodiscard]] auto empty_or_water(feature_context &context,
                                      const block_pos &pos) const -> bool;
    [[nodiscard]] auto empty_or_water_or_lava(feature_context &context,
                                              const block_pos &pos) const
        -> bool;
    [[nodiscard]] auto is_base_or_lava(feature_context &context,
                                       const block_pos &pos) const -> bool;
    auto move_back(feature_context &context, dripstone &stone,
                   const wind_offsetter &wind) const -> bool;
    void place_blocks(feature_context &context, worldgen_random &random,
                      const dripstone &stone, const wind_offsetter &wind) const;

    std::string replaceable_tag_{};
    jint floor_to_ceiling_range_{30};
    std::unique_ptr<provider::int_provider> radius_{};
    jint radius_min_{1};
    jint radius_max_{16};
    std::unique_ptr<provider::float_provider> height_scale_{};
    float radius_to_height_ratio_{0.33F};
    std::unique_ptr<provider::float_provider> stalactite_bluntness_{};
    std::unique_ptr<provider::float_provider> stalagmite_bluntness_{};
    std::unique_ptr<provider::float_provider> wind_speed_{};
    jint min_radius_for_wind_{4};
    float min_bluntness_for_wind_{0.6F};
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
