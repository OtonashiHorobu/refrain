// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Coordinate math shared by 26.3's noise aquifer: grid conversions, the
// similarity metric between cell distances, and the random cell center.

#pragma once

#include <refrain/worldgen/java_compat.hpp>
#include <refrain/worldgen/rng.hpp>

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

} // namespace refrain::worldgen::terrain
