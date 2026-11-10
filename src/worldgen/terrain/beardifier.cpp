// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/terrain/beardifier.hpp>

#include <refrain/worldgen/core/mth.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace refrain::worldgen {

namespace {

constexpr jint kernel_radius = 12;
constexpr jint kernel_size = 24;
constexpr float max_bury_distance = 6.0F;

auto compute_beard_contribution(jint dx, double dy, jint dz) -> double {
    const double distance_squar = mth::length_squared(
        static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
    return std::exp(-distance_squar / 16.0);
}

auto compute_beard_contribution(jint dx, jint dy, jint dz) -> double {
    return compute_beard_contribution(dx, static_cast<double>(dy) + 0.5, dz);
}

auto kernel() -> const std::array<float, 13824> & {
    static const std::array<float, 13824> table = [] {
        std::array<float, 13824> values{};
        for (jint zi = 0; zi < kernel_size; ++zi) {
            for (jint xi = 0; xi < kernel_size; ++xi) {
                for (jint yi = 0; yi < kernel_size; ++yi) {
                    values[static_cast<std::size_t>(zi * kernel_size *
                                                        kernel_size +
                                                    xi * kernel_size + yi)] =
                        static_cast<float>(compute_beard_contribution(
                            xi - kernel_radius, yi - kernel_radius,
                            zi - kernel_radius));
                }
            }
        }
        return values;
    }();
    return table;
}

auto get_bury_contribution(float dx, float dy, float dz) -> float {
    const float distance_squar = mth::length_squared(dx, dy, dz);
    return distance_squar >= 36.0F
               ? 0.0F
               : 1.0F - mth::sqrt(distance_squar) / max_bury_distance;
}

auto is_in_kernel_range(jint value) -> bool {
    return value >= 0 && value < kernel_size;
}

auto get_beard_contribution(jint dx, jint dy, jint dz, jint y_to_ground)
    -> float {
    const jint xi = dx + kernel_radius;
    const jint yi = dy + kernel_radius;
    const jint zi = dz + kernel_radius;
    if (!is_in_kernel_range(xi) || !is_in_kernel_range(yi) ||
        !is_in_kernel_range(zi)) {
        return 0.0F;
    }
    const float dy_with_offset = static_cast<float>(y_to_ground) + 0.5F;
    const float distance_squar = mth::length_squared(
        static_cast<float>(dx), dy_with_offset, static_cast<float>(dz));
    const float value = -dy_with_offset *
                        static_cast<float>(mth::fast_inv_sqrt(
                            static_cast<double>(distance_squar / 2.0F))) /
                        2.0F;
    return value * kernel()[static_cast<std::size_t>(
                       zi * kernel_size * kernel_size + xi * kernel_size + yi)];
}

} // namespace

auto bounding_box::is_inside(jint x, jint y, jint z) const noexcept -> bool {
    return x >= min_x && x <= max_x && z >= min_z && z <= max_z && y >= min_y &&
           y <= max_y;
}

auto bounding_box::intersects(jint other_min_x, jint other_min_y,
                              jint other_min_z, jint other_max_x,
                              jint other_max_y, jint other_max_z) const noexcept
    -> bool {
    return max_x >= other_min_x && min_x <= other_max_x &&
           max_z >= other_min_z && min_z <= other_max_z &&
           max_y >= other_min_y && min_y <= other_max_y;
}

auto bounding_box::inflated_by(jint amount) const noexcept -> bounding_box {
    return bounding_box{
        .min_x = min_x - amount,
        .min_y = min_y - amount,
        .min_z = min_z - amount,
        .max_x = max_x + amount,
        .max_y = max_y + amount,
        .max_z = max_z + amount,
    };
}

auto bounding_box::encapsulating(const bounding_box &first,
                                 const bounding_box &second) noexcept
    -> bounding_box {
    return bounding_box{
        .min_x = std::min(first.min_x, second.min_x),
        .min_y = std::min(first.min_y, second.min_y),
        .min_z = std::min(first.min_z, second.min_z),
        .max_x = std::max(first.max_x, second.max_x),
        .max_y = std::max(first.max_y, second.max_y),
        .max_z = std::max(first.max_z, second.max_z),
    };
}

beardifier::beardifier(std::vector<beardifier_rigid> pieces,
                       std::vector<jigsaw_junction> junctions,
                       std::optional<bounding_box> affected_box)
    : pieces_{std::move(pieces)}, junctions_{std::move(junctions)},
      affected_box_{affected_box} {}

auto beardifier::empty() -> std::unique_ptr<beardifier> {
    return std::make_unique<beardifier>(std::vector<beardifier_rigid>{},
                                        std::vector<jigsaw_junction>{},
                                        std::nullopt);
}

auto beardifier::kernel_entry(jint xi, jint yi, jint zi) const noexcept
    -> float {
    return kernel()[static_cast<std::size_t>(zi * kernel_size * kernel_size +
                                             xi * kernel_size + yi)];
}

auto beardifier::range() const -> interval { return interval::infinite(); }

auto beardifier::sample_value(const density::sampler_context & /*context*/,
                              jint block_x, jint block_y, jint block_z) const
    -> float {
    if (affected_box_.has_value() &&
        affected_box_->is_inside(block_x, block_y, block_z)) {
        return sample_unchecked(block_x, block_y, block_z);
    }
    return 0.0F;
}

void beardifier::sample_volume(const density::sampler_context & /*context*/,
                               buffer &output, const volume &vol) const {
    output.fill(0.0F);
    if (!affected_box_.has_value() ||
        !affected_box_->intersects(vol.min_block_x(), vol.min_block_y(),
                                   vol.min_block_z(), vol.max_block_x(),
                                   vol.max_block_y(), vol.max_block_z())) {
        return;
    }
    const auto &box = *affected_box_;
    const jint min_x = floor_div(std::max(0, box.min_x - vol.min_block_x()),
                                 vol.step_block_x());
    const jint min_y = floor_div(std::max(0, box.min_y - vol.min_block_y()),
                                 vol.step_block_y());
    const jint min_z = floor_div(std::max(0, box.min_z - vol.min_block_z()),
                                 vol.step_block_z());
    const jint max_x =
        std::min(vol.size_x() - 1,
                 floor_div(box.max_x - vol.min_block_x(), vol.step_block_x()));
    const jint max_y =
        std::min(vol.size_y() - 1,
                 floor_div(box.max_y - vol.min_block_y(), vol.step_block_y()));
    const jint max_z =
        std::min(vol.size_z() - 1,
                 floor_div(box.max_z - vol.min_block_z(), vol.step_block_z()));
    for (jint z = min_z; z <= max_z; ++z) {
        const jint block_z = vol.block_z(z);
        for (jint x = min_x; x <= max_x; ++x) {
            const jint block_x = vol.block_x(x);
            for (jint y = min_y; y <= max_y; ++y) {
                output.set(vol.index_unchecked(x, y, z),
                           sample_unchecked(block_x, vol.block_y(y), block_z));
            }
        }
    }
}

auto beardifier::sample_unchecked(jint block_x, jint block_y,
                                  jint block_z) const -> float {
    float noise_value = 0.0F;
    for (const auto &rigid : pieces_) {
        const auto &box = rigid.box;
        const jint dx =
            std::max(0, std::max(box.min_x - block_x, block_x - box.max_x));
        const jint dz =
            std::max(0, std::max(box.min_z - block_z, block_z - box.max_z));
        const jint ground_y = box.min_y + rigid.ground_level_delta;
        const jint dy_to_ground = block_y - ground_y;

        jint dy = 0;
        switch (rigid.adjustment) {
        case terrain_adjustment::none:
            dy = 0;
            break;
        case terrain_adjustment::bury:
        case terrain_adjustment::beard_thin:
            dy = dy_to_ground;
            break;
        case terrain_adjustment::beard_box:
            dy = std::max(0, std::max(ground_y - block_y, block_y - box.max_y));
            break;
        case terrain_adjustment::encapsulate:
            dy =
                std::max(0, std::max(box.min_y - block_y, block_y - box.max_y));
            break;
        }

        switch (rigid.adjustment) {
        case terrain_adjustment::none:
            break;
        case terrain_adjustment::bury:
            noise_value += get_bury_contribution(static_cast<float>(dx),
                                                 static_cast<float>(dy) / 2.0F,
                                                 static_cast<float>(dz));
            break;
        case terrain_adjustment::beard_thin:
        case terrain_adjustment::beard_box:
            noise_value +=
                get_beard_contribution(dx, dy, dz, dy_to_ground) * 0.8F;
            break;
        case terrain_adjustment::encapsulate:
            noise_value +=
                get_bury_contribution(static_cast<float>(dx) / 2.0F,
                                      static_cast<float>(dy) / 2.0F,
                                      static_cast<float>(dz) / 2.0F) *
                0.8F;
            break;
        }
    }
    for (const auto &junction : junctions_) {
        const jint dx = block_x - junction.source_x;
        const jint dy = block_y - junction.source_ground_y;
        const jint dz = block_z - junction.source_z;
        noise_value += get_beard_contribution(dx, dy, dz, dy) * 0.4F;
    }
    return noise_value;
}

} // namespace refrain::worldgen
