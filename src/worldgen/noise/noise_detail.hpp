// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Shared lattice arithmetic of the Perlin variants. The scalar bulk path and
// the AVX2 kernel both read these definitions, so the corner terms and the
// fudged-Y coordinate cannot drift apart between the two.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace refrain::worldgen::noise_detail {

struct gradient {
    int x;
    int y;
    int z;
};

// The 16-entry gradient table, including the four duplicates at positions
// 12..15.
inline constexpr std::array<gradient, 16> gradients{{
    {1, 1, 0},
    {-1, 1, 0},
    {1, -1, 0},
    {-1, -1, 0},
    {1, 0, 1},
    {-1, 0, 1},
    {1, 0, -1},
    {-1, 0, -1},
    {0, 1, 1},
    {0, -1, 1},
    {0, 1, -1},
    {0, -1, -1},
    {1, 1, 0},
    {0, -1, 1},
    {-1, 1, 0},
    {0, -1, -1},
}};

inline constexpr double wrap_period = 33554432.0; // Java 3.3554432E7
inline constexpr double half_wrap = 16777216.0;   // Java 1.6777216E7

/// Keeps coordinates inside the stable range so the integer floor does not
/// overflow far from spawn. Far lands are never enabled.
[[nodiscard]] inline auto wrap(double value) -> double {
    static const double half_round_off = std::nextafter(half_wrap, 0.0);
    if (value >= -half_round_off && value < half_round_off) {
        return value;
    }
    return value - std::floor(value / wrap_period + 0.5) * wrap_period;
}

[[nodiscard]] inline auto permute(const std::uint8_t *perms, jint value)
    -> jint {
    return static_cast<jint>(perms[static_cast<std::size_t>(value) & 0xFFU]);
}

[[nodiscard]] inline auto permute(const std::array<std::uint8_t, 256> &perms,
                                  jint value) -> jint {
    return permute(perms.data(), value);
}

/// The smeared variant's Y fudge. `smeared_perlin_noise::get` and both bulk
/// paths share this expression so they observe the same fudged coordinate.
[[nodiscard]] inline auto smeared_fudge_y(double original_y, double relative_y,
                                          double fudge_y_scale) -> double {
    const double limit = (original_y >= 0.0 && original_y < relative_y)
                             ? original_y
                             : relative_y;
    return std::floor(limit / fudge_y_scale + 1.0E-7) * fudge_y_scale;
}

/// The eight corner terms of one lattice column. The XZ product is constant
/// while the floored Y does not change, so a column only recomputes the terms
/// when the floor moves.
struct perlin_corner_terms {
    float d000xz{0.0F};
    float d100xz{0.0F};
    float d010xz{0.0F};
    float d110xz{0.0F};
    float d001xz{0.0F};
    float d101xz{0.0F};
    float d011xz{0.0F};
    float d111xz{0.0F};
    float g000y{0.0F};
    float g100y{0.0F};
    float g010y{0.0F};
    float g110y{0.0F};
    float g001y{0.0F};
    float g101y{0.0F};
    float g011y{0.0F};
    float g111y{0.0F};
};

/// Recomputes the eight corner terms for one floored Y. The expression order
/// matches the reference bulk path exactly.
inline void update_corner_terms(const std::uint8_t *perms, jint x0, jint x1,
                                jint floor_z, jint floor_y, float relative_x,
                                float relative_z, perlin_corner_terms &terms) {
    const jint xy00 = permute(perms, x0 + floor_y);
    const jint xy01 = permute(perms, x0 + floor_y + 1);
    const jint xy10 = permute(perms, x1 + floor_y);
    const jint xy11 = permute(perms, x1 + floor_y + 1);

    const auto &g000 =
        gradients[static_cast<std::size_t>(permute(perms, xy00 + floor_z)) &
                  15U];
    terms.d000xz = static_cast<float>(g000.x) * relative_x +
                   static_cast<float>(g000.z) * relative_z;
    terms.g000y = static_cast<float>(g000.y);

    const auto &g100 =
        gradients[static_cast<std::size_t>(permute(perms, xy10 + floor_z)) &
                  15U];
    terms.d100xz = static_cast<float>(g100.x) * (relative_x - 1.0F) +
                   static_cast<float>(g100.z) * relative_z;
    terms.g100y = static_cast<float>(g100.y);

    const auto &g010 =
        gradients[static_cast<std::size_t>(permute(perms, xy01 + floor_z)) &
                  15U];
    terms.d010xz = static_cast<float>(g010.x) * relative_x +
                   static_cast<float>(g010.z) * relative_z;
    terms.g010y = static_cast<float>(g010.y);

    const auto &g110 =
        gradients[static_cast<std::size_t>(permute(perms, xy11 + floor_z)) &
                  15U];
    terms.d110xz = static_cast<float>(g110.x) * (relative_x - 1.0F) +
                   static_cast<float>(g110.z) * relative_z;
    terms.g110y = static_cast<float>(g110.y);

    const auto &g001 =
        gradients[static_cast<std::size_t>(permute(perms, xy00 + floor_z + 1)) &
                  15U];
    terms.d001xz = static_cast<float>(g001.x) * relative_x +
                   static_cast<float>(g001.z) * (relative_z - 1.0F);
    terms.g001y = static_cast<float>(g001.y);

    const auto &g101 =
        gradients[static_cast<std::size_t>(permute(perms, xy10 + floor_z + 1)) &
                  15U];
    terms.d101xz = static_cast<float>(g101.x) * (relative_x - 1.0F) +
                   static_cast<float>(g101.z) * (relative_z - 1.0F);
    terms.g101y = static_cast<float>(g101.y);

    const auto &g011 =
        gradients[static_cast<std::size_t>(permute(perms, xy01 + floor_z + 1)) &
                  15U];
    terms.d011xz = static_cast<float>(g011.x) * relative_x +
                   static_cast<float>(g011.z) * (relative_z - 1.0F);
    terms.g011y = static_cast<float>(g011.y);

    const auto &g111 =
        gradients[static_cast<std::size_t>(permute(perms, xy11 + floor_z + 1)) &
                  15U];
    terms.d111xz = static_cast<float>(g111.x) * (relative_x - 1.0F) +
                   static_cast<float>(g111.z) * (relative_z - 1.0F);
    terms.g111y = static_cast<float>(g111.y);
}

inline void update_corner_terms(const std::array<std::uint8_t, 256> &perms,
                                jint x0, jint x1, jint floor_z, jint floor_y,
                                float relative_x, float relative_z,
                                perlin_corner_terms &terms) {
    update_corner_terms(perms.data(), x0, x1, floor_z, floor_y, relative_x,
                        relative_z, terms);
}

/// Inputs of the AVX2 bulk kernel. `perms` points at the 256-entry
/// permutation, `fudge_y_scale` is read only by the smeared variant.
struct perlin_volume_parameters {
    const std::uint8_t *perms{nullptr};
    double offset_x{0.0};
    double offset_y{0.0};
    double offset_z{0.0};
    double fudge_y_scale{0.0};
    bool smeared{false};
};

} // namespace refrain::worldgen::noise_detail

namespace refrain::worldgen {

/// AVX2 version of the Perlin bulk path. The caller must have verified
/// `__builtin_cpu_supports("avx2")`; the function is compiled for that target
/// and is not safe to call elsewhere.
void perlin_volume_avx2(
    const noise_detail::perlin_volume_parameters &parameters, buffer &output,
    const volume &volume, double xz_scale, double y_scale, float amplitude);

} // namespace refrain::worldgen
