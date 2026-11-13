// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Shared lattice arithmetic of the Perlin variants. The scalar bulk path and
// the AVX2 kernel both read these definitions, so the corner terms and the
// fudged-Y coordinate cannot drift apart between the two.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/mth.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

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

/// The same table in the float form the sampling expressions multiply by, so
/// a corner does not convert the integer components again for every sample.
struct float_gradient {
    float x;
    float y;
    float z;
};

inline constexpr std::array<float_gradient, 16> float_gradients{{
    {1.0F, 1.0F, 0.0F},
    {-1.0F, 1.0F, 0.0F},
    {1.0F, -1.0F, 0.0F},
    {-1.0F, -1.0F, 0.0F},
    {1.0F, 0.0F, 1.0F},
    {-1.0F, 0.0F, 1.0F},
    {1.0F, 0.0F, -1.0F},
    {-1.0F, 0.0F, -1.0F},
    {0.0F, 1.0F, 1.0F},
    {0.0F, -1.0F, 1.0F},
    {0.0F, 1.0F, -1.0F},
    {0.0F, -1.0F, -1.0F},
    {1.0F, 1.0F, 0.0F},
    {0.0F, -1.0F, 1.0F},
    {-1.0F, 1.0F, 0.0F},
    {0.0F, -1.0F, -1.0F},
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

/// The smeared variant's Y fudge divisor, prepared once per bulk pass.
///
/// The expression divides the limit by the scale. When the scale is a power of
/// two, multiplying by its reciprocal rounds to the same double: both forms
/// round the exact quotient, and the reciprocal itself is exact. The multiply
/// avoids a division per sample.
struct smear_scale {
    double value{0.0};
    double reciprocal{0.0};
    bool exact_reciprocal{false};
};

[[nodiscard]] inline auto make_smear_scale(double divisor) -> smear_scale {
    smear_scale scale{.value = divisor};
    int exponent = 0;
    if (divisor > 0.0 && std::isfinite(divisor) &&
        std::frexp(divisor, &exponent) == 0.5 && exponent > -1000 &&
        exponent < 1000) {
        scale.reciprocal = 1.0 / divisor;
        scale.exact_reciprocal = true;
    }
    return scale;
}

/// The smeared variant's Y fudge. `smeared_perlin_noise::get` and both bulk
/// paths share this expression so they observe the same fudged coordinate.
[[nodiscard]] inline auto smeared_fudge_y(double original_y, double relative_y,
                                          const smear_scale &scale) -> double {
    const double limit = (original_y >= 0.0 && original_y < relative_y)
                             ? original_y
                             : relative_y;
    const double quotient =
        scale.exact_reciprocal ? limit * scale.reciprocal : limit / scale.value;
    return std::floor(quotient + 1.0E-7) * scale.value;
}

[[nodiscard]] inline auto smeared_fudge_y(double original_y, double relative_y,
                                          double fudge_y_scale) -> double {
    return smeared_fudge_y(original_y, relative_y,
                           make_smear_scale(fudge_y_scale));
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

    const auto &g000 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy00 + floor_z)) &
                                       15U];
    terms.d000xz = g000.x * relative_x + g000.z * relative_z;
    terms.g000y = g000.y;

    const auto &g100 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy10 + floor_z)) &
                                       15U];
    terms.d100xz = g100.x * (relative_x - 1.0F) + g100.z * relative_z;
    terms.g100y = g100.y;

    const auto &g010 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy01 + floor_z)) &
                                       15U];
    terms.d010xz = g010.x * relative_x + g010.z * relative_z;
    terms.g010y = g010.y;

    const auto &g110 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy11 + floor_z)) &
                                       15U];
    terms.d110xz = g110.x * (relative_x - 1.0F) + g110.z * relative_z;
    terms.g110y = g110.y;

    const auto &g001 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy00 + floor_z + 1)) &
                                       15U];
    terms.d001xz = g001.x * relative_x + g001.z * (relative_z - 1.0F);
    terms.g001y = g001.y;

    const auto &g101 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy10 + floor_z + 1)) &
                                       15U];
    terms.d101xz = g101.x * (relative_x - 1.0F) + g101.z * (relative_z - 1.0F);
    terms.g101y = g101.y;

    const auto &g011 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy01 + floor_z + 1)) &
                                       15U];
    terms.d011xz = g011.x * relative_x + g011.z * (relative_z - 1.0F);
    terms.g011y = g011.y;

    const auto &g111 = float_gradients[static_cast<std::size_t>(
                                           permute(perms, xy11 + floor_z + 1)) &
                                       15U];
    terms.d111xz = g111.x * (relative_x - 1.0F) + g111.z * (relative_z - 1.0F);
    terms.g111y = g111.y;
}

inline void update_corner_terms(const std::array<std::uint8_t, 256> &perms,
                                jint x0, jint x1, jint floor_z, jint floor_y,
                                float relative_x, float relative_z,
                                perlin_corner_terms &terms) {
    update_corner_terms(perms.data(), x0, x1, floor_z, floor_y, relative_x,
                        relative_z, terms);
}

/// Inputs of both bulk kernels. `perms` points at the 256-entry permutation;
/// `fudge` is read only by the smeared variant.
struct perlin_volume_parameters {
    const std::uint8_t *perms{nullptr};
    double offset_x{0.0};
    double offset_y{0.0};
    double offset_z{0.0};
    smear_scale fudge{};
    bool smeared{false};
};

/// Baseline bulk path of the Perlin lattice: the exact arithmetic the
/// accelerated kernel has to reproduce. `add_to_volume` uses it when the CPU
/// has no vector target and the kernel test compares it against the vector
/// path.
template <bool smeared>
void perlin_volume_scalar(const perlin_volume_parameters &parameters,
                          buffer &output, const volume &vol, double xz_scale,
                          double y_scale, float amplitude) {
    std::size_t index = 0;

    for (jint index_z = 0; index_z < vol.size_z(); ++index_z) {
        const double z =
            wrap(vol.block_z(index_z) * xz_scale) + parameters.offset_z;
        const jint floor_z = mth::floor(z);
        const float relative_z = static_cast<float>(z - floor_z);
        const float alpha_z = mth::smoothstep(relative_z);

        for (jint index_x = 0; index_x < vol.size_x(); ++index_x) {
            const double x =
                wrap(vol.block_x(index_x) * xz_scale) + parameters.offset_x;
            const jint floor_x = mth::floor(x);
            const float relative_x = static_cast<float>(x - floor_x);
            const jint x0 = permute(parameters.perms, floor_x);
            const jint x1 = permute(parameters.perms, floor_x + 1);
            const float alpha_x = mth::smoothstep(relative_x);

            perlin_corner_terms terms;
            jint last_floor_y = std::numeric_limits<jint>::min();
            for (jint index_y = 0; index_y < vol.size_y(); ++index_y) {
                const double original_y = vol.block_y(index_y) * y_scale;
                const double y = wrap(original_y) + parameters.offset_y;
                const jint floor_y = mth::floor(y);
                const double relative_y_double =
                    y - static_cast<double>(floor_y);
                const float relative_y = static_cast<float>(relative_y_double);
                const float alpha_y = mth::smoothstep(relative_y);
                if (last_floor_y != floor_y) {
                    update_corner_terms(parameters.perms, x0, x1, floor_z,
                                        floor_y, relative_x, relative_z, terms);
                    last_floor_y = floor_y;
                }
                float fudged_relative_y = relative_y;
                if constexpr (smeared) {
                    fudged_relative_y = static_cast<float>(
                        relative_y_double - smeared_fudge_y(original_y,
                                                            relative_y_double,
                                                            parameters.fudge));
                }
                output.add_to(
                    index,
                    amplitude *
                        mth::lerp3(
                            alpha_x, alpha_y, alpha_z,
                            terms.d000xz + terms.g000y * fudged_relative_y,
                            terms.d100xz + terms.g100y * fudged_relative_y,
                            terms.d010xz +
                                terms.g010y * (fudged_relative_y - 1.0F),
                            terms.d110xz +
                                terms.g110y * (fudged_relative_y - 1.0F),
                            terms.d001xz + terms.g001y * fudged_relative_y,
                            terms.d101xz + terms.g101y * fudged_relative_y,
                            terms.d011xz +
                                terms.g011y * (fudged_relative_y - 1.0F),
                            terms.d111xz +
                                terms.g111y * (fudged_relative_y - 1.0F)));
                ++index;
            }
        }
    }
}

/// Selects the baseline kernel for the variant the parameters describe.
inline void perlin_volume_scalar(const perlin_volume_parameters &parameters,
                                 buffer &output, const volume &vol,
                                 double xz_scale, double y_scale,
                                 float amplitude) {
    if (parameters.smeared) {
        perlin_volume_scalar<true>(parameters, output, vol, xz_scale, y_scale,
                                   amplitude);
    } else {
        perlin_volume_scalar<false>(parameters, output, vol, xz_scale, y_scale,
                                    amplitude);
    }
}

} // namespace refrain::worldgen::noise_detail

namespace refrain::worldgen {

/// AVX2 version of the Perlin bulk path. The caller must have verified
/// `__builtin_cpu_supports("avx2")`; the function is compiled for that target
/// and is not safe to call elsewhere.
void perlin_volume_avx2(
    const noise_detail::perlin_volume_parameters &parameters, buffer &output,
    const volume &volume, double xz_scale, double y_scale, float amplitude);

} // namespace refrain::worldgen
