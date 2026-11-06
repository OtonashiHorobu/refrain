// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Coordinate math shared by 26.3's noise aquifer: grid conversions, the
// similarity metric between cell distances, and the random cell center.

#pragma once

#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/java_compat.hpp>
#include <refrain/worldgen/mth.hpp>
#include <refrain/worldgen/rng.hpp>
#include <refrain/worldgen/terrain.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::terrain {

struct cell_center {
    jint x{0};
    jint y{0};
    jint z{0};
};

class aquifer_grid {
  public:
    static constexpr jint cell_size = 16;   // X/Z grid anchor
    static constexpr jint cell_height = 12; // Y grid anchor

    /// `blockCoord >> 4`.
    [[nodiscard]] static auto grid_x(jint block_coord) noexcept -> jint {
        return block_coord >> 4;
    }
    /// `Math.floorDiv(blockCoord, 12)`.
    [[nodiscard]] static auto grid_y(jint block_coord) noexcept -> jint {
        return floor_div(block_coord, cell_height);
    }
    [[nodiscard]] static auto grid_z(jint block_coord) noexcept -> jint {
        return block_coord >> 4;
    }

    [[nodiscard]] static auto from_grid_x(jint grid_coord,
                                          jint block_offset) noexcept -> jint {
        return (grid_coord << 4) + block_offset;
    }
    [[nodiscard]] static auto from_grid_y(jint grid_coord,
                                          jint block_offset) noexcept -> jint {
        return grid_coord * cell_height + block_offset;
    }
    [[nodiscard]] static auto from_grid_z(jint grid_coord,
                                          jint block_offset) noexcept -> jint {
        return (grid_coord << 4) + block_offset;
    }

    /// `1 - (distanceSqr2 - distanceSqr1) / 25`.
    [[nodiscard]] static auto similarity(jint distance_sqr1,
                                         jint distance_sqr2) noexcept
        -> double {
        return 1.0 - static_cast<double>(distance_sqr2 - distance_sqr1) / 25.0;
    }

    /// The reference draws three bounded ints from the positional stream at
    /// the grid coordinate and offsets the anchor.
    [[nodiscard]] static auto center(positional_random_factory &factory,
                                     jint grid_x, jint grid_y, jint grid_z)
        -> cell_center;
};

/// The six density functions of the `noise_settings.aquifers` object.
struct aquifer_config {
    density::sampler_ptr barrier{};
    density::sampler_ptr fluid_level_floodedness{};
    density::sampler_ptr fluid_level_spread{};
    density::sampler_ptr lava{};
    density::sampler_ptr exclusion{};
    density::sampler_ptr surface_level{};
};

/// `Aquifer.NoiseBasedAquifer`: the overworld aquifer. Cell cache and surface
/// level cache follow the reference; sampler caching is omitted because it does
/// not change values.
class noise_aquifer final : public aquifer {
  public:
    noise_aquifer(aquifer_config config, positional_random_factory &factory,
                  const volume &volume, fluid_picker picker, block_ids blocks,
                  const density::sampler_context &context);

    [[nodiscard]] auto compute_substance(jint block_x, jint block_y,
                                         jint block_z, double density)
        -> std::optional<std::uint16_t> override;
    [[nodiscard]] auto should_schedule_fluid_update() const -> bool override {
        return should_schedule_fluid_update_;
    }

  private:
    [[nodiscard]] auto surface_level(jint block_x, jint block_z) -> jint;
    [[nodiscard]] auto max_surface_level(jint min_block_x, jint min_block_z,
                                         jint max_block_x, jint max_block_z)
        -> jint;
    [[nodiscard]] auto get_index(jint grid_x, jint grid_y, jint grid_z) const
        -> std::size_t;
    [[nodiscard]] auto get_aquifer_status(std::size_t index) -> fluid_status;
    [[nodiscard]] auto compute_fluid(jint x, jint y, jint z) -> fluid_status;
    [[nodiscard]] auto compute_surface_level(jint x, jint y, jint z,
                                             const fluid_status &global_fluid,
                                             jint lowest_surface_level,
                                             bool surface_at_center_under_fluid)
        -> jint;
    [[nodiscard]] auto
    compute_randomized_fluid_surface_level(jint x, jint y, jint z,
                                           jint lowest_surface_level) -> jint;
    [[nodiscard]] auto compute_fluid_type(jint x, jint y, jint z,
                                          const fluid_status &global_fluid,
                                          jint fluid_surface_level)
        -> std::uint16_t;
    [[nodiscard]] auto
    calculate_pressure(jint x, jint y, jint z, bool &noise_sampled,
                       double &noise_value, const fluid_status &closest1,
                       const fluid_status &closest2) -> double;

    aquifer_config config_;
    positional_random_factory *factory_;
    fluid_picker picker_;
    block_ids blocks_;
    density::sampler_context context_;
    jint min_grid_x_{0};
    jint min_grid_y_{0};
    jint min_grid_z_{0};
    jint grid_size_x_{0};
    jint grid_size_y_{0};
    jint grid_size_z_{0};
    jint skip_sampling_above_y_{0};
    std::vector<std::optional<fluid_status>> status_cache_;
    std::vector<cell_center> centers_;
    std::vector<bool> center_ready_;
    std::unordered_map<std::int64_t, jint> surface_level_cache_;
    bool should_schedule_fluid_update_{false};
};

/// `DimensionType.WAY_BELOW_MIN_Y` (MIN_Y << 4).
inline constexpr jint way_below_min_y = -1024;

/// `computeRandomizedFluidSurfaceLevel`: `middleY` is `floorDiv(y, 40) * 40 +
/// 20`; the spread noise is sampled at `(floorDiv(x,16), floorDiv(y,40),
/// floorDiv(z,16))` and multiplied by the float constant 10.0F.
[[nodiscard]] inline auto randomized_fluid_surface_level(jint middle_y,
                                                         float spread_noise,
                                                         jint lowest_surface)
    -> jint {
    const double spread = static_cast<double>(spread_noise * 10.0F);
    return std::min(lowest_surface, middle_y + mth::quantize(spread, 3));
}

/// `computeFluidType`: lava when the surface is at most -10, is not
/// `WAY_BELOW_MIN_Y`, the global fluid is not already lava, and the lava noise
/// magnitude exceeds 0.3.
[[nodiscard]] inline auto fluid_type_is_lava(jint fluid_surface_level,
                                             bool global_is_lava,
                                             double lava_noise) -> bool {
    return fluid_surface_level <= -10 &&
           fluid_surface_level != way_below_min_y && !global_is_lava &&
           std::abs(lava_noise) > 0.3;
}

} // namespace refrain::worldgen::terrain
