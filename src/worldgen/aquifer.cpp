// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/aquifer.hpp>

namespace refrain::worldgen::terrain {

auto aquifer_grid::center(positional_random_factory &factory, jint grid_x_coord,
                          jint grid_y_coord, jint grid_z_coord) -> cell_center {
    auto random = factory.at(grid_x_coord, grid_y_coord, grid_z_coord);
    const jint x_offset = random->next_int(10);
    const jint y_offset = random->next_int(9);
    const jint z_offset = random->next_int(10);
    return cell_center{
        .x = from_grid_x(grid_x_coord, x_offset),
        .y = from_grid_y(grid_y_coord, y_offset),
        .z = from_grid_z(grid_z_coord, z_offset),
    };
}

} // namespace refrain::worldgen::terrain
