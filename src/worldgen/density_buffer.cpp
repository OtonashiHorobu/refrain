// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/density_buffer.hpp>

#include <refrain/worldgen/java_compat.hpp>

#include <stdexcept>

namespace refrain::worldgen {

volume::volume(jint size_x, jint size_y, jint size_z, jint min_block_x,
               jint min_block_y, jint min_block_z, jint step_block_x,
               jint step_block_y, jint step_block_z)
    : size_x_{size_x}, size_y_{size_y}, size_z_{size_z},
      min_block_x_{min_block_x}, min_block_y_{min_block_y},
      min_block_z_{min_block_z}, step_block_x_{step_block_x},
      step_block_y_{step_block_y}, step_block_z_{step_block_z} {
    if (size_x <= 0 || size_y <= 0 || size_z <= 0) {
        throw std::invalid_argument("volume size must be positive");
    }
    if (step_block_x <= 0 || step_block_y <= 0 || step_block_z <= 0) {
        throw std::invalid_argument("volume step must be positive");
    }
}

auto volume::index_of_block(jint block_x, jint block_y,
                            jint block_z) const noexcept -> std::size_t {
    const jint relative_x = block_x - min_block_x_;
    const jint relative_y = block_y - min_block_y_;
    const jint relative_z = block_z - min_block_z_;
    if (step_block_x_ == 1 && step_block_y_ == 1 && step_block_z_ == 1) {
        if (relative_x >= 0 && relative_y >= 0 && relative_z >= 0 &&
            relative_x < size_x_ && relative_y < size_y_ &&
            relative_z < size_z_) {
            return index_unchecked(relative_x, relative_y, relative_z);
        }
        return npos;
    }
    if (relative_x >= 0 && relative_y >= 0 && relative_z >= 0 &&
        relative_x < size_x_ * step_block_x_ &&
        relative_y < size_y_ * step_block_y_ &&
        relative_z < size_z_ * step_block_z_ &&
        floor_mod(relative_x, step_block_x_) == 0 &&
        floor_mod(relative_y, step_block_y_) == 0 &&
        floor_mod(relative_z, step_block_z_) == 0) {
        return index_unchecked(floor_div(relative_x, step_block_x_),
                               floor_div(relative_y, step_block_y_),
                               floor_div(relative_z, step_block_z_));
    }
    return npos;
}

} // namespace refrain::worldgen
