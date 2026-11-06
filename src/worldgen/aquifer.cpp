// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/aquifer.hpp>

#include <array>
#include <limits>

namespace refrain::worldgen::terrain {

namespace {

constexpr std::array<std::array<jint, 2>, 13> surface_sampling_offsets{{
    {0, 0},
    {-2, -1},
    {-1, -1},
    {0, -1},
    {1, -1},
    {-3, 0},
    {-2, 0},
    {-1, 0},
    {1, 0},
    {-2, 1},
    {-1, 1},
    {0, 1},
    {1, 1},
}};

constexpr jint way_below_min_y_value = way_below_min_y;

auto is_air(std::uint16_t state, const block_ids &blocks) -> bool {
    return state == blocks.air;
}
auto is_lava(std::uint16_t state, const block_ids &blocks) -> bool {
    return state == blocks.lava;
}
auto is_water(std::uint16_t state, const block_ids &blocks) -> bool {
    return state == blocks.default_fluid;
}

auto java_clamp(double value, double min, double max) -> double {
    return value < min ? min : (value < max ? value : max);
}

auto pack_column(jint x, jint z) -> std::int64_t {
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uz = static_cast<std::uint32_t>(z);
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(ux) |
                                     (static_cast<std::uint64_t>(uz) << 32U));
}

} // namespace

noise_aquifer::noise_aquifer(const aquifer_config &config,
                             positional_random_factory &factory,
                             const volume &vol, fluid_picker picker,
                             block_ids blocks,
                             const density::sampler_context &context)
    : config_{config}, factory_{&factory}, picker_{picker}, blocks_{blocks},
      context_{context} {
    min_grid_x_ = aquifer_grid::grid_x(vol.min_block_x() - 5);
    min_grid_y_ = aquifer_grid::grid_y(vol.min_block_y() + 1) - 1;
    min_grid_z_ = aquifer_grid::grid_z(vol.min_block_z() - 5);
    const jint max_grid_x = aquifer_grid::grid_x(vol.max_block_x() - 5) + 1;
    const jint max_grid_y = aquifer_grid::grid_y(vol.max_block_y() + 1) + 1;
    const jint max_grid_z = aquifer_grid::grid_z(vol.max_block_z() - 5) + 1;
    grid_size_x_ = max_grid_x - min_grid_x_ + 1;
    grid_size_y_ = max_grid_y - min_grid_y_ + 1;
    grid_size_z_ = max_grid_z - min_grid_z_ + 1;
    const auto total = static_cast<std::size_t>(grid_size_x_) *
                       static_cast<std::size_t>(grid_size_y_) *
                       static_cast<std::size_t>(grid_size_z_);
    status_cache_.resize(total);
    centers_.resize(total);
    center_ready_.assign(total, false);

    const jint max_adjusted_surface_level = [&] {
        const jint min_block_x = aquifer_grid::from_grid_x(min_grid_x_, 0);
        const jint min_block_z = aquifer_grid::from_grid_z(min_grid_z_, 0);
        const jint max_block_x = aquifer_grid::from_grid_x(max_grid_x, 9);
        const jint max_block_z = aquifer_grid::from_grid_z(max_grid_z, 9);
        return max_surface_level(min_block_x, min_block_z, max_block_x,
                                 max_block_z) +
               8;
    }();
    const jint skip_grid_y =
        aquifer_grid::grid_y(max_adjusted_surface_level + 12) - -1;
    skip_sampling_above_y_ = aquifer_grid::from_grid_y(skip_grid_y, 11) - 1;
}

auto noise_aquifer::get_index(jint grid_x, jint grid_y, jint grid_z) const
    -> std::size_t {
    const jint x = grid_x - min_grid_x_;
    const jint y = grid_y - min_grid_y_;
    const jint z = grid_z - min_grid_z_;
    return static_cast<std::size_t>((y * grid_size_z_ + z) * grid_size_x_ + x);
}

auto noise_aquifer::surface_level(jint block_x, jint block_z) -> jint {
    const jint quantized_x = (block_x >> 2) << 2;
    const jint quantized_z = (block_z >> 2) << 2;
    const auto key = pack_column(quantized_x, quantized_z);
    const auto found = surface_level_cache_.find(key);
    if (found != surface_level_cache_.end()) {
        return found->second;
    }
    const jint value =
        mth::floor(static_cast<double>(config_.surface_level->sample_value(
            context_, quantized_x, 0, quantized_z)));
    surface_level_cache_.emplace(key, value);
    return value;
}

auto noise_aquifer::max_surface_level(jint min_block_x, jint min_block_z,
                                      jint max_block_x, jint max_block_z)
    -> jint {
    const jint min_quart_x = min_block_x >> 2;
    const jint max_quart_x = max_block_x >> 2;
    const jint min_quart_z = min_block_z >> 2;
    const jint max_quart_z = max_block_z >> 2;
    const volume vol{max_quart_x - min_quart_x + 1,
                     1,
                     max_quart_z - min_quart_z + 1,
                     min_quart_x * 4,
                     0,
                     min_quart_z * 4,
                     4,
                     1,
                     4};
    buffer values{vol.size()};
    config_.surface_level->sample_volume(context_, values, vol);
    jint max_y = std::numeric_limits<jint>::min();
    for (jint z = 0; z < vol.size_z(); ++z) {
        for (jint x = 0; x < vol.size_x(); ++x) {
            const jint level = mth::floor(
                static_cast<double>(values.get(vol.index_unchecked(x, 0, z))));
            surface_level_cache_[pack_column(vol.block_x(x), vol.block_z(z))] =
                level;
            max_y = std::max(max_y, level);
        }
    }
    return max_y;
}

auto noise_aquifer::get_aquifer_status(std::size_t index) -> fluid_status {
    if (status_cache_[index].has_value()) {
        return *status_cache_[index];
    }
    if (!center_ready_[index]) {
        const jint y = static_cast<jint>(
            index / static_cast<std::size_t>(grid_size_x_ * grid_size_z_));
        const jint remainder = static_cast<jint>(
            index % static_cast<std::size_t>(grid_size_x_ * grid_size_z_));
        const jint z = remainder / grid_size_x_;
        const jint x = remainder % grid_size_x_;
        centers_[index] = aquifer_grid::center(
            *factory_, min_grid_x_ + x, min_grid_y_ + y, min_grid_z_ + z);
        center_ready_[index] = true;
    }
    const auto &center = centers_[index];
    const auto status = compute_fluid(center.x, center.y, center.z);
    status_cache_[index] = status;
    return status;
}

auto noise_aquifer::compute_fluid(jint x, jint y, jint z) -> fluid_status {
    const auto global_fluid = picker_.compute_fluid(x, y, z);
    jint lowest_preliminary_surface = std::numeric_limits<jint>::max();
    const jint top_of_cell = y + 12;
    const jint bottom_of_cell = y - 12;
    bool surface_at_center_under_global_fluid = false;

    for (const auto &offset : surface_sampling_offsets) {
        const jint sample_x = x + offset[0] * 16;
        const jint sample_z = z + offset[1] * 16;
        const jint level = surface_level(sample_x, sample_z);
        const jint adjusted = level + 8;
        const bool start = offset[0] == 0 && offset[1] == 0;
        if (start && bottom_of_cell > adjusted) {
            return global_fluid;
        }
        const bool pokes_above = top_of_cell > adjusted;
        if (pokes_above || start) {
            const auto global_at_surface =
                picker_.compute_fluid(sample_x, adjusted, sample_z);
            if (!is_air(global_at_surface.at(adjusted, blocks_.air), blocks_)) {
                if (start) {
                    surface_at_center_under_global_fluid = true;
                }
                if (pokes_above) {
                    return global_at_surface;
                }
            }
        }
        lowest_preliminary_surface =
            std::min(lowest_preliminary_surface, level);
    }

    const jint fluid_surface_level =
        compute_surface_level(x, y, z, global_fluid, lowest_preliminary_surface,
                              surface_at_center_under_global_fluid);
    return fluid_status{
        .fluid_level = fluid_surface_level,
        .fluid_type =
            compute_fluid_type(x, y, z, global_fluid, fluid_surface_level),
    };
}

auto noise_aquifer::compute_surface_level(jint x, jint y, jint z,
                                          const fluid_status &global_fluid,
                                          jint lowest_surface_level,
                                          bool surface_at_center_under_fluid)
    -> jint {
    double partially_floodedness = -1.0;
    double fully_floodedness = -1.0;
    if (!(config_.exclusion->sample_value(context_, x, y, z) > 0.0)) {
        const jint distance_below_surface = (lowest_surface_level + 8) - y;
        const double floodedness_factor =
            surface_at_center_under_fluid
                ? mth::clamped_map(static_cast<double>(distance_below_surface),
                                   0.0, 64.0, 1.0, 0.0)
                : 0.0;
        const double noise = java_clamp(
            static_cast<double>(config_.fluid_level_floodedness->sample_value(
                context_, x, y, z)),
            -1.0, 1.0);
        const double fully_threshold =
            mth::map(floodedness_factor, 1.0, 0.0, -0.3, 0.8);
        const double partially_threshold =
            mth::map(floodedness_factor, 1.0, 0.0, -0.8, 0.4);
        partially_floodedness = noise - partially_threshold;
        fully_floodedness = noise - fully_threshold;
    }
    if (fully_floodedness > 0.0) {
        return global_fluid.fluid_level;
    }
    if (partially_floodedness > 0.0) {
        return compute_randomized_fluid_surface_level(x, y, z,
                                                      lowest_surface_level);
    }
    return way_below_min_y_value;
}

auto noise_aquifer::compute_randomized_fluid_surface_level(
    jint x, jint y, jint z, jint lowest_surface_level) -> jint {
    const jint cell_x = floor_div(x, 16);
    const jint cell_y = floor_div(y, 40);
    const jint cell_z = floor_div(z, 16);
    const jint middle_y = cell_y * 40 + 20;
    const float spread = config_.fluid_level_spread->sample_value(
        context_, cell_x, cell_y, cell_z);
    return randomized_fluid_surface_level(middle_y, spread,
                                          lowest_surface_level);
}

auto noise_aquifer::compute_fluid_type(jint x, jint y, jint z,
                                       const fluid_status &global_fluid,
                                       jint fluid_surface_level)
    -> std::uint16_t {
    auto type = global_fluid.fluid_type;
    if (fluid_surface_level <= -10 &&
        fluid_surface_level != way_below_min_y_value &&
        !is_lava(type, blocks_)) {
        const jint cell_x = floor_div(x, 64);
        const jint cell_y = floor_div(y, 40);
        const jint cell_z = floor_div(z, 64);
        const double lava_noise = static_cast<double>(
            config_.lava->sample_value(context_, cell_x, cell_y, cell_z));
        if (std::abs(lava_noise) > 0.3) {
            type = blocks_.lava;
        }
    }
    return type;
}

auto noise_aquifer::calculate_pressure(jint x, jint y, jint z,
                                       bool &noise_sampled, double &noise_value,
                                       const fluid_status &closest1,
                                       const fluid_status &closest2) -> double {
    const auto type1 = closest1.at(y, blocks_.air);
    const auto type2 = closest2.at(y, blocks_.air);
    if ((!is_lava(type1, blocks_) || !is_water(type2, blocks_)) &&
        (!is_water(type1, blocks_) || !is_lava(type2, blocks_))) {
        const jint fluid_y_diff =
            std::abs(closest1.fluid_level - closest2.fluid_level);
        if (fluid_y_diff == 0) {
            return 0.0;
        }
        const double average_fluid_y =
            0.5 * (static_cast<double>(closest1.fluid_level) +
                   static_cast<double>(closest2.fluid_level));
        const double how_far_above =
            static_cast<double>(y) + 0.5 - average_fluid_y;
        const double base_value = static_cast<double>(fluid_y_diff) / 2.0;
        const double distance_from_edge = base_value - std::abs(how_far_above);
        double gradient = 0.0;
        if (how_far_above > 0.0) {
            const double center = 0.0 + distance_from_edge;
            gradient = center > 0.0 ? center / 1.5 : center / 2.5;
        } else {
            const double center = 3.0 + distance_from_edge;
            gradient = center > 0.0 ? center / 3.0 : center / 10.0;
        }
        double noise = 0.0;
        if (!(gradient < -2.0) && !(gradient > 2.0)) {
            if (!noise_sampled) {
                noise_value = static_cast<double>(
                    config_.barrier->sample_value(context_, x, y, z));
                noise_sampled = true;
            }
            noise = noise_value;
        }
        return 2.0 * (noise + gradient);
    }
    return 2.0;
}

auto noise_aquifer::compute_substance(jint block_x, jint block_y, jint block_z,
                                      double density)
    -> std::optional<std::uint16_t> {
    if (density > 0.0) {
        should_schedule_fluid_update_ = false;
        return std::nullopt;
    }
    const auto global_fluid = picker_.compute_fluid(block_x, block_y, block_z);
    if (block_y > skip_sampling_above_y_) {
        should_schedule_fluid_update_ = false;
        return global_fluid.at(block_y, blocks_.air);
    }
    if (is_lava(global_fluid.at(block_y, blocks_.air), blocks_)) {
        should_schedule_fluid_update_ = false;
        return blocks_.lava;
    }

    const jint x_anchor = aquifer_grid::grid_x(block_x + -5);
    const jint y_anchor = aquifer_grid::grid_y(block_y + 1);
    const jint z_anchor = aquifer_grid::grid_z(block_z + -5);
    jint distance_sqr1 = std::numeric_limits<jint>::max();
    jint distance_sqr2 = std::numeric_limits<jint>::max();
    jint distance_sqr3 = std::numeric_limits<jint>::max();
    jint distance_sqr4 = std::numeric_limits<jint>::max();
    std::size_t closest1 = 0;
    std::size_t closest2 = 0;
    std::size_t closest3 = 0;
    std::size_t closest4 = 0;

    for (jint x1 = 0; x1 <= 1; ++x1) {
        for (jint y1 = -1; y1 <= 1; ++y1) {
            for (jint z1 = 0; z1 <= 1; ++z1) {
                const jint grid_x = x_anchor + x1;
                const jint grid_y = y_anchor + y1;
                const jint grid_z = z_anchor + z1;
                const std::size_t index = get_index(grid_x, grid_y, grid_z);
                if (!center_ready_[index]) {
                    centers_[index] =
                        aquifer_grid::center(*factory_, grid_x, grid_y, grid_z);
                    center_ready_[index] = true;
                }
                const auto &center = centers_[index];
                const jint dx = center.x - block_x;
                const jint dy = center.y - block_y;
                const jint dz = center.z - block_z;
                const jint new_distance = dx * dx + dy * dy + dz * dz;
                if (distance_sqr1 >= new_distance) {
                    closest4 = closest3;
                    closest3 = closest2;
                    closest2 = closest1;
                    closest1 = index;
                    distance_sqr4 = distance_sqr3;
                    distance_sqr3 = distance_sqr2;
                    distance_sqr2 = distance_sqr1;
                    distance_sqr1 = new_distance;
                } else if (distance_sqr2 >= new_distance) {
                    closest4 = closest3;
                    closest3 = closest2;
                    closest2 = index;
                    distance_sqr4 = distance_sqr3;
                    distance_sqr3 = distance_sqr2;
                    distance_sqr2 = new_distance;
                } else if (distance_sqr3 >= new_distance) {
                    closest4 = closest3;
                    closest3 = index;
                    distance_sqr4 = distance_sqr3;
                    distance_sqr3 = new_distance;
                } else if (distance_sqr4 >= new_distance) {
                    closest4 = index;
                    distance_sqr4 = new_distance;
                }
            }
        }
    }

    const auto status1 = get_aquifer_status(closest1);
    const double similarity12 =
        aquifer_grid::similarity(distance_sqr1, distance_sqr2);
    const auto fluid_state = status1.at(block_y, blocks_.air);
    if (similarity12 <= 0.0) {
        should_schedule_fluid_update_ =
            similarity12 >= aquifer_grid::similarity(100, 144)
                ? !(status1 == get_aquifer_status(closest2))
                : false;
        return fluid_state;
    }
    if (is_water(fluid_state, blocks_) &&
        is_lava(picker_.compute_fluid(block_x, block_y - 1, block_z)
                    .at(block_y - 1, blocks_.air),
                blocks_)) {
        should_schedule_fluid_update_ = true;
        return fluid_state;
    }

    bool barrier_noise_sampled = false;
    double barrier_noise_value = 0.0;
    const auto status2 = get_aquifer_status(closest2);
    const double barrier12 =
        similarity12 *
        calculate_pressure(block_x, block_y, block_z, barrier_noise_sampled,
                           barrier_noise_value, status1, status2);
    if (density + barrier12 > 0.0) {
        should_schedule_fluid_update_ = false;
        return std::nullopt;
    }

    const auto status3 = get_aquifer_status(closest3);
    const double similarity13 =
        aquifer_grid::similarity(distance_sqr1, distance_sqr3);
    if (similarity13 > 0.0) {
        const double barrier13 =
            similarity12 * similarity13 *
            calculate_pressure(block_x, block_y, block_z, barrier_noise_sampled,
                               barrier_noise_value, status1, status3);
        if (density + barrier13 > 0.0) {
            should_schedule_fluid_update_ = false;
            return std::nullopt;
        }
    }

    const double similarity23 =
        aquifer_grid::similarity(distance_sqr2, distance_sqr3);
    if (similarity23 > 0.0) {
        const double barrier23 =
            similarity12 * similarity23 *
            calculate_pressure(block_x, block_y, block_z, barrier_noise_sampled,
                               barrier_noise_value, status2, status3);
        if (density + barrier23 > 0.0) {
            should_schedule_fluid_update_ = false;
            return std::nullopt;
        }
    }

    const bool may_flow12 = !(status1 == status2);
    const double flowing_similarity = aquifer_grid::similarity(100, 144);
    const bool may_flow23 =
        similarity23 >= flowing_similarity && !(status2 == status3);
    const bool may_flow13 =
        similarity13 >= flowing_similarity && !(status1 == status3);
    if (!may_flow12 && !may_flow23 && !may_flow13) {
        should_schedule_fluid_update_ =
            similarity13 >= flowing_similarity &&
            aquifer_grid::similarity(distance_sqr1, distance_sqr4) >=
                flowing_similarity &&
            !(status1 == get_aquifer_status(closest4));
    } else {
        should_schedule_fluid_update_ = true;
    }
    return fluid_state;
}

} // namespace refrain::worldgen::terrain

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
