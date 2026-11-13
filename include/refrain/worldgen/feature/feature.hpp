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
#include <refrain/worldgen/provider.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <array>
#include <optional>

namespace refrain::worldgen {
class stage_profile;
} // namespace refrain::worldgen

namespace refrain::worldgen::feature {

struct feature_context {
    placement_context placement{};
    /// Places a top-level or nested placed feature by id. The decoration stage
    /// supplies this; selectors use it for their nested entries.
    std::function<bool(std::string_view, worldgen_random &, const block_pos &)>
        place_by_id{};
    /// Places one configured feature by id without its placement modifiers.
    /// Decorators that run a nested configured feature use it.
    std::function<bool(std::string_view, worldgen_random &, const block_pos &)>
        place_configured{};
    /// Template and processor-list sources for features that place `.nbt`
    /// content (fossils).
    const structure::structure_template_manager *templates{nullptr};
    /// Mutable table for template state resolution.
    block_state_registry *registry{nullptr};
    std::function<const structure::structure_processor_list *(std::string_view)>
        processor_list{};
    /// Optional per-stage accounting owned by the caller. Null in production.
    stage_profile *profile{nullptr};
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

/// Places one block state, optionally scheduling its tick.
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

/// A flat disk of one state over a target predicate.
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

/// A randomly oriented ellipsoid family replacing a target rule
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

/// Runs nested placed features in order and stops at the
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

/// Finds the target block below the origin and replaces
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

/// Grows a blue-ice blob from water next to packed ice.
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

/// A handful of single-block ore replacements close to
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

/// Writes the origin block, then scatters more
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

/// A bamboo stalk with a random height and optional podzol
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

/// One vine block attached to the first acceptable horizontal
/// or upward neighbour.
class vines_feature final : public feature {
  public:
    /// Face order: down, up, north, south, west, east. The down entry is
    /// unused.
    explicit vines_feature(std::array<block_state_id, 6> states)
        : states_{states} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::array<block_state_id, 6> states_{};
};

/// Crimson/warped stem plus cap, with optional weeping
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

/// A stem with a
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

/// An ellipse/circle iceberg centred on sea level.
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

/// Layered amethyst geode with an optional crack.
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

/// Freezes the block below the motion-blocking top and
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

/// Glow-lichen style growth on block faces with the
/// reference spreader.
class multiface_growth_feature final : public feature {
  public:
    multiface_growth_feature(std::string block, jint search_range, bool floor,
                             bool ceiling, bool wall, float chance,
                             std::vector<std::string> can_be_placed_on)
        : block_name_{std::move(block)}, search_range_{search_range},
          floor_{floor}, ceiling_{ceiling}, wall_{wall}, chance_{chance},
          can_be_placed_on_{std::move(can_be_placed_on)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::string block_name_{};
    jint search_range_{10};
    bool floor_{false};
    bool ceiling_{false};
    bool wall_{false};
    float chance_{0.5F};
    std::vector<std::string> can_be_placed_on_{};
};

/// A stone platform around (8, 3, 8) in the
/// origin chunk and its neighbours.
class void_start_platform_feature final : public feature {
  public:
    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;
};

/// The obsidian platform at the end spawn point.
class end_platform_feature final : public feature {
  public:
    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;
};

/// A ground patch with optional nested vegetation.
class vegetation_patch_feature final : public feature {
  public:
    vegetation_patch_feature(std::string nested_feature,
                             std::unique_ptr<state_provider> ground_state,
                             std::string replaceable,
                             std::unique_ptr<provider::int_provider> depth,
                             std::unique_ptr<provider::int_provider> xz_radius,
                             bool ceiling, jint vertical_range,
                             float vegetation_chance,
                             float extra_bottom_block_chance,
                             float extra_edge_column_chance, bool waterlogged)
        : nested_feature_{std::move(nested_feature)},
          ground_state_{std::move(ground_state)},
          replaceable_{std::move(replaceable)}, depth_{std::move(depth)},
          xz_radius_{std::move(xz_radius)}, ceiling_{ceiling},
          vertical_range_{vertical_range},
          vegetation_chance_{vegetation_chance},
          extra_bottom_block_chance_{extra_bottom_block_chance},
          extra_edge_column_chance_{extra_edge_column_chance},
          waterlogged_{waterlogged} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto replaceable(feature_context &context,
                                   block_state_id state) const -> bool;
    [[nodiscard]] auto place_ground(feature_context &context,
                                    worldgen_random &random, block_pos &below,
                                    jint depth) const -> bool;
    void distribute(feature_context &context, worldgen_random &random,
                    const std::vector<block_pos> &surface) const;

    std::string nested_feature_{};
    std::unique_ptr<state_provider> ground_state_{};
    std::string replaceable_{};
    std::unique_ptr<provider::int_provider> depth_{};
    std::unique_ptr<provider::int_provider> xz_radius_{};
    bool ceiling_{false};
    jint vertical_range_{5};
    float vegetation_chance_{0.0F};
    float extra_bottom_block_chance_{0.0F};
    float extra_edge_column_chance_{0.0F};
    bool waterlogged_{false};
};

/// Places rooted dirt and hanging roots around a nested
/// placed feature.
class root_system_feature final : public feature {
  public:
    root_system_feature(
        std::string nested_feature, std::unique_ptr<state_provider> root_state,
        std::unique_ptr<state_provider> hanging_state,
        std::unique_ptr<block_predicate> allowed_tree_position,
        std::string replaceable_tag, jint required_vertical_space,
        jint level_test_distance, jint max_level_deviation, jint root_radius,
        jint root_placement_attempts, jint root_column_max_height,
        jint hanging_root_radius, jint hanging_roots_vertical_span,
        jint hanging_root_placement_attempts, jint allowed_vertical_water)
        : nested_feature_{std::move(nested_feature)},
          root_state_{std::move(root_state)},
          hanging_state_{std::move(hanging_state)},
          allowed_tree_position_{std::move(allowed_tree_position)},
          replaceable_tag_{std::move(replaceable_tag)},
          required_vertical_space_{required_vertical_space},
          level_test_distance_{level_test_distance},
          max_level_deviation_{max_level_deviation}, root_radius_{root_radius},
          root_placement_attempts_{root_placement_attempts},
          root_column_max_height_{root_column_max_height},
          hanging_root_radius_{hanging_root_radius},
          hanging_roots_vertical_span_{hanging_roots_vertical_span},
          hanging_root_placement_attempts_{hanging_root_placement_attempts},
          allowed_vertical_water_{allowed_vertical_water} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto space_for_tree(feature_context &context,
                                      const block_pos &pos) const -> bool;
    [[nodiscard]] auto replaceable(feature_context &context,
                                   block_state_id state) const -> bool;
    auto place_dirt_and_tree(feature_context &context, worldgen_random &random,
                             block_pos &working, const block_pos &origin) const
        -> bool;
    void place_dirt(feature_context &context, worldgen_random &random,
                    const block_pos &origin, jint target_height) const;
    void place_roots(feature_context &context, worldgen_random &random,
                     const block_pos &origin) const;

    std::string nested_feature_{};
    std::unique_ptr<state_provider> root_state_{};
    std::unique_ptr<state_provider> hanging_state_{};
    std::unique_ptr<block_predicate> allowed_tree_position_{};
    std::string replaceable_tag_{};
    jint required_vertical_space_{3};
    jint level_test_distance_{0};
    jint max_level_deviation_{0};
    jint root_radius_{3};
    jint root_placement_attempts_{20};
    jint root_column_max_height_{100};
    jint hanging_root_radius_{3};
    jint hanging_roots_vertical_span_{2};
    jint hanging_root_placement_attempts_{20};
    jint allowed_vertical_water_{2};
};

/// The bedrock frame around an end gateway block.
class end_gateway_feature final : public feature {
  public:
    end_gateway_feature(block_state_id gateway, block_state_id bedrock,
                        std::optional<block_pos> exit, bool exact)
        : gateway_{gateway}, bedrock_{bedrock}, exit_{exit}, exact_{exact} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id gateway_{no_block_state};
    block_state_id bedrock_{no_block_state};
    std::optional<block_pos> exit_{};
    bool exact_{false};
};

/// One configured end spike.
struct end_spike {
    jint center_x{0};
    jint center_z{0};
    jint radius{0};
    jint height{0};
    bool guarded{false};
};

/// Obsidian pillars with an optional iron-bar cage and a
/// fire-topped crystal block.
class end_spike_feature final : public feature {
  public:
    end_spike_feature(std::vector<end_spike> spikes, bool crystal_invulnerable)
        : spikes_{std::move(spikes)},
          crystal_invulnerable_{crystal_invulnerable} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    void place_spike(feature_context &context, const end_spike &spike) const;

    std::vector<end_spike> spikes_{};
    bool crystal_invulnerable_{false};
};

/// The spawn chest with four torches.
class bonus_chest_feature final : public feature {
  public:
    bonus_chest_feature(block_state_id chest, block_state_id torch)
        : chest_{chest}, torch_{torch} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id chest_{no_block_state};
    block_state_id torch_{no_block_state};
};

/// Grows a chorus plant from the origin.
class chorus_plant_feature final : public feature {
  public:
    chorus_plant_feature(block_state_id plant, block_state_id flower)
        : plant_{plant}, flower_{flower} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    void grow(feature_context &context, worldgen_random &random,
              const block_pos &current, const block_pos &start, jint depth,
              jint max_spread) const;
    void set_plant(feature_context &context, const block_pos &pos) const;
    void set_flower(feature_context &context, const block_pos &pos) const;

    block_state_id plant_{no_block_state};
    block_state_id flower_{no_block_state};
};

/// The exit portal platform with its bedrock pillar and
/// torches.
class end_podium_feature final : public feature {
  public:
    end_podium_feature(bool active, block_state_id bedrock,
                       block_state_id end_stone, block_state_id end_portal,
                       std::array<block_state_id, 4> torches)
        : active_{active}, bedrock_{bedrock}, end_stone_{end_stone},
          end_portal_{end_portal}, torches_{torches} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    bool active_{false};
    block_state_id bedrock_{no_block_state};
    block_state_id end_stone_{no_block_state};
    block_state_id end_portal_{no_block_state};
    std::array<block_state_id, 4> torches_{};
};

/// A shrinking end-stone disc stack.
class end_island_feature final : public feature {
  public:
    explicit end_island_feature(block_state_id end_stone)
        : end_stone_{end_stone} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id end_stone_{no_block_state};
};

/// Places a fossil template below the surface, then the
/// matching coal overlay template.
class fossil_feature final : public feature {
  public:
    fossil_feature(std::vector<std::string> fossil_structures,
                   std::vector<std::string> overlay_structures,
                   std::string fossil_processors,
                   std::string overlay_processors, jint max_empty_corners)
        : fossil_structures_{std::move(fossil_structures)},
          overlay_structures_{std::move(overlay_structures)},
          fossil_processors_{std::move(fossil_processors)},
          overlay_processors_{std::move(overlay_processors)},
          max_empty_corners_{max_empty_corners} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::string> fossil_structures_{};
    std::vector<std::string> overlay_structures_{};
    std::string fossil_processors_{};
    std::string overlay_processors_{};
    jint max_empty_corners_{0};
};

/// A field of pointed dripstone columns grown
/// between a cave floor and ceiling, optionally with water pools.
class speleothem_cluster_feature final : public feature {
  public:
    speleothem_cluster_feature(
        block_state_id base_block, block_state_id pointed_block,
        std::string replaceable_tag, jint floor_to_ceiling_search_range,
        std::unique_ptr<provider::int_provider> height,
        std::unique_ptr<provider::int_provider> radius, jint max_height_diff,
        jint height_deviation,
        std::unique_ptr<provider::int_provider> layer_thickness,
        std::unique_ptr<provider::float_provider> density,
        std::unique_ptr<provider::float_provider> wetness,
        float chance_at_max_distance, jint max_distance_from_edge,
        jint max_distance_from_center)
        : base_block_{base_block}, pointed_block_{pointed_block},
          replaceable_tag_{std::move(replaceable_tag)},
          search_range_{floor_to_ceiling_search_range},
          height_{std::move(height)}, radius_{std::move(radius)},
          max_height_diff_{max_height_diff},
          height_deviation_{height_deviation},
          layer_thickness_{std::move(layer_thickness)},
          density_{std::move(density)}, wetness_{std::move(wetness)},
          chance_at_max_distance_{chance_at_max_distance},
          max_distance_from_edge_{max_distance_from_edge},
          max_distance_from_center_{max_distance_from_center} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id base_block_{no_block_state};
    block_state_id pointed_block_{no_block_state};
    std::string replaceable_tag_{};
    jint search_range_{12};
    std::unique_ptr<provider::int_provider> height_{};
    std::unique_ptr<provider::int_provider> radius_{};
    jint max_height_diff_{0};
    jint height_deviation_{1};
    std::unique_ptr<provider::int_provider> layer_thickness_{};
    std::unique_ptr<provider::float_provider> density_{};
    std::unique_ptr<provider::float_provider> wetness_{};
    float chance_at_max_distance_{0.1F};
    jint max_distance_from_edge_{1};
    jint max_distance_from_center_{1};
};

/// A cobblestone dungeon room with a spawner and up to
/// two loot chests, carved into a solid shell.
class monster_room_feature final : public feature {
  public:
    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;
};

/// A tapering spike above a base block with an optional
/// downward pillar (ice spikes).
class spike_feature final : public feature {
  public:
    spike_feature(block_state_id state,
                  std::unique_ptr<block_predicate> can_place_on,
                  std::unique_ptr<block_predicate> can_replace)
        : state_{state}, can_place_on_{std::move(can_place_on)},
          can_replace_{std::move(can_replace)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id state_{no_block_state};
    std::unique_ptr<block_predicate> can_place_on_{};
    std::unique_ptr<block_predicate> can_replace_{};
};

/// Magma blocks buried in the floor under a water
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

/// An ellipsoid lake carved into solid ground, lined with a
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

/// A lava pool with a magma rim on a flat cave floor.
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

/// A stalactite/stalagmite pair in a cave column.
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

/// A fluid source surrounded by a fixed rock/hole pattern.
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

/// Three random blobs anchored on a placeable block.
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

/// Layered columns along one axis.
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

/// Independent chances, then the default.
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

/// Weighted random selection: entries carry an integer weight and a total.
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

/// Uniform random selection from a list of feature ids.
class simple_selector_feature final : public feature {
  public:
    explicit simple_selector_feature(std::vector<std::string> entries)
        : entries_{std::move(entries)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::string> entries_{};
};

/// Picks one of two features by a random boolean.
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

/// A small pile of one block state on a placeable surface.
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

/// Places one weighted `.nbt` template, rotated by a random choice from the
/// entry's rotation list. The template is centred on the placement origin.
class template_feature final : public feature {
  public:
    struct entry {
        std::string id{};
        std::vector<structure::template_rotation> rotations{};
    };

    template_feature(
        std::vector<std::pair<jint, entry>> entries, jint total_weight,
        std::shared_ptr<structure::structure_processor_list> processors,
        std::string processor_id)
        : entries_{std::move(entries)}, total_weight_{total_weight},
          processors_{std::move(processors)},
          processor_id_{std::move(processor_id)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::pair<jint, entry>> entries_{};
    jint total_weight_{0};
    std::shared_ptr<structure::structure_processor_list> processors_{};
    std::string processor_id_{};
};

/// Places every nested placed feature at the same origin.
class overlay_feature final : public feature {
  public:
    explicit overlay_feature(std::vector<std::string> features)
        : features_{std::move(features)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::vector<std::string> features_{};
};

/// Performs no writes and always succeeds.
class no_op_feature final : public feature {
  public:
    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;
};

/// Fills one 16x16 layer `height` blocks above the dimension's bottom with
/// `state`, keeping every non-air block.
class fill_layer_feature final : public feature {
  public:
    fill_layer_feature(jint height, block_state_id state)
        : height_{height}, state_{state} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    jint height_{0};
    block_state_id state_{no_block_state};
};

/// Climbs from the origin while `can_replace` accepts the block and the
/// continue chance holds, then runs the cap placed feature one step back.
class single_block_pillar_feature final : public feature {
  public:
    single_block_pillar_feature(std::unique_ptr<state_provider> block,
                                std::unique_ptr<block_predicate> can_replace,
                                jint direction, float chance_to_continue,
                                std::string cap_feature)
        : block_{std::move(block)}, can_replace_{std::move(can_replace)},
          direction_{direction}, chance_to_continue_{chance_to_continue},
          cap_feature_{std::move(cap_feature)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::unique_ptr<state_provider> block_{};
    std::unique_ptr<block_predicate> can_replace_{};
    jint direction_{1};
    float chance_to_continue_{1.0F};
    std::string cap_feature_{};
};

/// Grows one pointed-block column from a base patch of replaceable blocks.
class speleothem_feature final : public feature {
  public:
    speleothem_feature(block_state_id base_block, block_state_id pointed_block,
                       std::string replaceable_tag,
                       float chance_of_taller_generation,
                       float chance_of_directional_spread,
                       float chance_of_spread_radius2,
                       float chance_of_spread_radius3)
        : base_block_{base_block}, pointed_block_{pointed_block},
          replaceable_tag_{std::move(replaceable_tag)},
          chance_taller_{chance_of_taller_generation},
          chance_directional_{chance_of_directional_spread},
          chance_radius2_{chance_of_spread_radius2},
          chance_radius3_{chance_of_spread_radius3} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto is_base(feature_context &context,
                               block_state_id state) const -> bool;
    void place_base(feature_context &context, const block_pos &pos) const;
    void create_patch(feature_context &context, worldgen_random &random,
                      const block_pos &root) const;
    void grow(feature_context &context, const block_pos &origin, jint direction,
              jint height) const;

    block_state_id base_block_{no_block_state};
    block_state_id pointed_block_{no_block_state};
    std::string replaceable_tag_{};
    float chance_taller_{0.2F};
    float chance_directional_{0.7F};
    float chance_radius2_{0.5F};
    float chance_radius3_{0.5F};
};

/// Repeats a nested placed feature along a trunk and side branches.
class coral_tree_feature final : public feature {
  public:
    explicit coral_tree_feature(std::string feature_id)
        : feature_{std::move(feature_id)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::string feature_{};
};

/// Repeats a nested placed feature along a claw of side branches.
class coral_claw_feature final : public feature {
  public:
    explicit coral_claw_feature(std::string feature_id)
        : feature_{std::move(feature_id)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::string feature_{};
};

/// Drops a randomised square of one state downwards through a predicate.
class projected_random_patchy_square_feature final : public feature {
  public:
    projected_random_patchy_square_feature(
        std::unique_ptr<state_provider> block,
        std::unique_ptr<block_predicate> project_through,
        std::unique_ptr<provider::int_provider> size,
        jint max_projection_height)
        : block_{std::move(block)},
          project_through_{std::move(project_through)}, size_{std::move(size)},
          max_projection_height_{max_projection_height} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    std::unique_ptr<state_provider> block_{};
    std::unique_ptr<block_predicate> project_through_{};
    std::unique_ptr<provider::int_provider> size_{};
    jint max_projection_height_{0};
};

/// Grows stepped columns from a cluster of surface positions.
class stepped_column_cluster_feature final : public feature {
  public:
    stepped_column_cluster_feature(
        std::unique_ptr<state_provider> block,
        std::unique_ptr<block_predicate> continue_through,
        std::unique_ptr<block_predicate> can_replace,
        std::string cannot_place_on_tag,
        std::vector<std::string> cannot_place_on_blocks,
        std::unique_ptr<provider::int_provider> cluster_reach,
        std::unique_ptr<provider::int_provider> column_count,
        std::unique_ptr<provider::int_provider> column_reach,
        std::unique_ptr<provider::int_provider> height)
        : block_{std::move(block)},
          continue_through_{std::move(continue_through)},
          can_replace_{std::move(can_replace)},
          cannot_place_on_tag_{std::move(cannot_place_on_tag)},
          cannot_place_on_blocks_{std::move(cannot_place_on_blocks)},
          cluster_reach_{std::move(cluster_reach)},
          column_count_{std::move(column_count)},
          column_reach_{std::move(column_reach)}, height_{std::move(height)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto cannot_place_on(feature_context &context,
                                       block_state_id state) const -> bool;
    [[nodiscard]] auto can_place_at(feature_context &context,
                                    const block_pos &pos) const -> bool;
    [[nodiscard]] auto find_surface(feature_context &context,
                                    const block_pos &start, jint limit) const
        -> std::optional<block_pos>;
    [[nodiscard]] auto find_air(feature_context &context,
                                const block_pos &start, jint limit) const
        -> std::optional<block_pos>;
    auto place_column(feature_context &context, worldgen_random &random,
                      jint sea_level, const block_pos &origin,
                      jint column_height, jint reach) const -> bool;

    std::unique_ptr<state_provider> block_{};
    std::unique_ptr<block_predicate> continue_through_{};
    std::unique_ptr<block_predicate> can_replace_{};
    std::string cannot_place_on_tag_{};
    std::vector<std::string> cannot_place_on_blocks_{};
    std::unique_ptr<provider::int_provider> cluster_reach_{};
    std::unique_ptr<provider::int_provider> column_count_{};
    std::unique_ptr<provider::int_provider> column_reach_{};
    std::unique_ptr<provider::int_provider> height_{};
};

/// Spreads sculk charges from the origin over a growing patch.
class sculk_patch_feature final : public feature {
  public:
    sculk_patch_feature(jint charge_count, jint amount_per_charge,
                        jint spread_attempts, jint growth_rounds,
                        jint spread_rounds)
        : charge_count_{charge_count}, amount_per_charge_{amount_per_charge},
          spread_attempts_{spread_attempts}, growth_rounds_{growth_rounds},
          spread_rounds_{spread_rounds} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    jint charge_count_{0};
    jint amount_per_charge_{0};
    jint spread_attempts_{0};
    jint growth_rounds_{0};
    jint spread_rounds_{0};
};

} // namespace refrain::worldgen::feature
