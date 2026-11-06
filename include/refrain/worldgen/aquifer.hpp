// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Coordinate math shared by 26.3's noise aquifer: grid conversions, the
// similarity metric between cell distances, and the random cell center.

#pragma once

#include <refrain/worldgen/java_compat.hpp>
#include <refrain/worldgen/mth.hpp>
#include <refrain/worldgen/rng.hpp>

#include <algorithm>
#include <cmath>

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
