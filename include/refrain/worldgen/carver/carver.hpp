// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The carver stage: cave and canyon shapes write a local `CarvingMask`, which
// the terrain stage applies to the chunk after the surface pass.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/carving_mask.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/provider.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen::carver {

/// `WorldCarver.CarveSkipChecker`.
using skip_checker =
    std::function<bool(double xd, double yd, double zd, jint y)>;

/// `WorldCarver.carveEllipsoid`.
void carve_ellipsoid(const chunk_pos &chunk_pos, double x, double y, double z,
                     double horizontal_radius, double vertical_radius,
                     carver_output &output, const skip_checker &skip);

/// `WorldCarver.canReach`.
[[nodiscard]] auto can_reach(const chunk_pos &chunk_pos, double x, double z,
                             jint current_step, jint total_steps,
                             float thickness) -> bool;

class world_carver {
  public:
    world_carver() = default;
    world_carver(const world_carver &) = delete;
    auto operator=(const world_carver &) -> world_carver & = delete;
    world_carver(world_carver &&) = delete;
    auto operator=(world_carver &&) -> world_carver & = delete;
    virtual ~world_carver() = default;

    [[nodiscard]] virtual auto range() const -> jint { return 4; }
    [[nodiscard]] virtual auto is_start_chunk(worldgen_random &random) const
        -> bool = 0;
    [[nodiscard]] virtual auto
    carve(const provider::generation_context &context, worldgen_random &random,
          const chunk_pos &chunk_pos,
          const ::refrain::worldgen::chunk_pos &source_pos,
          carver_output &output) const -> bool = 0;
};

class cave_carver final : public world_carver {
  public:
    cave_carver(
        float probability, std::unique_ptr<provider::height_provider> y,
        std::unique_ptr<provider::int_provider> count,
        std::unique_ptr<provider::float_provider> thickness,
        bool weird_thickness_bias,
        std::unique_ptr<provider::float_provider>
            room_vertical_radius_multiplier,
        std::unique_ptr<provider::float_provider> horizontal_radius_multiplier,
        std::unique_ptr<provider::float_provider> vertical_radius_multiplier,
        std::unique_ptr<provider::float_provider>
            start_vertical_radius_multiplier,
        std::unique_ptr<provider::float_provider> floor_level);

    [[nodiscard]] auto is_start_chunk(worldgen_random &random) const
        -> bool override;
    [[nodiscard]] auto carve(const provider::generation_context &context,
                             worldgen_random &random,
                             const chunk_pos &chunk_pos,
                             const ::refrain::worldgen::chunk_pos &source_pos,
                             carver_output &output) const -> bool override;

  private:
    void create_room(const chunk_pos &chunk_pos, double x, double y, double z,
                     float thickness, double y_scale, carver_output &output,
                     const skip_checker &skip) const;
    void create_tunnel(const chunk_pos &chunk_pos, jlong tunnel_seed, double x,
                       double y, double z, double horizontal_radius_multiplier,
                       double vertical_radius_multiplier, float thickness,
                       float horizontal_rotation, float vertical_rotation,
                       jint step, jint distance, double y_scale,
                       carver_output &output, const skip_checker &skip) const;
    [[nodiscard]] auto get_thickness(worldgen_random &random) const -> float;
    [[nodiscard]] static auto should_skip(double xd, double yd, double zd,
                                          double floor_level) -> bool;

    float probability_{0.0F};
    std::unique_ptr<provider::height_provider> y_{};
    std::unique_ptr<provider::int_provider> count_{};
    std::unique_ptr<provider::float_provider> thickness_{};
    bool weird_thickness_bias_{false};
    std::unique_ptr<provider::float_provider>
        room_vertical_radius_multiplier_{};
    std::unique_ptr<provider::float_provider> horizontal_radius_multiplier_{};
    std::unique_ptr<provider::float_provider> vertical_radius_multiplier_{};
    std::unique_ptr<provider::float_provider>
        start_vertical_radius_multiplier_{};
    std::unique_ptr<provider::float_provider> floor_level_{};
};

struct canyon_shape {
    std::unique_ptr<provider::float_provider> distance_factor{};
    std::unique_ptr<provider::float_provider> thickness{};
    jint width_smoothness{1};
    std::unique_ptr<provider::float_provider> horizontal_radius_factor{};
    float vertical_radius_default_factor{0.0F};
    float vertical_radius_center_factor{0.0F};
    std::unique_ptr<provider::float_provider> y_scale{};
};

class canyon_carver final : public world_carver {
  public:
    canyon_carver(float probability,
                  std::unique_ptr<provider::height_provider> y,
                  std::unique_ptr<provider::float_provider> vertical_rotation,
                  canyon_shape shape);

    [[nodiscard]] auto is_start_chunk(worldgen_random &random) const
        -> bool override;
    [[nodiscard]] auto carve(const provider::generation_context &context,
                             worldgen_random &random,
                             const chunk_pos &chunk_pos,
                             const ::refrain::worldgen::chunk_pos &source_pos,
                             carver_output &output) const -> bool override;

  private:
    void do_carve(const provider::generation_context &context,
                  const chunk_pos &chunk_pos, jlong tunnel_seed, double x,
                  double y, double z, float thickness,
                  float horizontal_rotation, float vertical_rotation, jint step,
                  jint distance, double y_scale, carver_output &output) const;
    [[nodiscard]] auto
    init_width_factors(const provider::generation_context &context,
                       worldgen_random &random) const -> std::vector<float>;
    [[nodiscard]] static auto
    update_vertical_radius(worldgen_random &random, double vertical_radius,
                           float distance, float current_step,
                           const canyon_shape &shape) -> double;
    [[nodiscard]] static auto
    should_skip(const provider::generation_context &context,
                const std::vector<float> &width_factor_per_height, double xd,
                double yd, double zd, jint y) -> bool;

    float probability_{0.0F};
    std::unique_ptr<provider::height_provider> y_{};
    std::unique_ptr<provider::float_provider> vertical_rotation_{};
    canyon_shape shape_{};
};

/// Decodes one `worldgen/carver` document. The returned carver owns its value
/// providers.
[[nodiscard]] auto decode_carver(const data::json::value &document)
    -> result<std::unique_ptr<world_carver>>;

} // namespace refrain::worldgen::carver
