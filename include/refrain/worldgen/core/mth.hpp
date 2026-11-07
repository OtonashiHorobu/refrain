// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The subset of `net.minecraft.util.Mth` that world generation evaluates on
// every block or cell. Each function mirrors the Java expression, including
// its float/double types and evaluation order; see
// `doc/spec/worldgen/noise.md` for the source review.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace refrain::worldgen::mth {

namespace detail {

/// Java `Mth.SIN`: 65536 samples of `Math.sin(i / 10430.378350470453)`.
inline auto sin_table() -> const std::array<float, 65536> & {
    static const auto table = [] {
        std::array<float, 65536> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<float>(
                std::sin(static_cast<double>(i) / 10430.378350470453));
        }
        return values;
    }();
    return table;
}

} // namespace detail

/// `Mth.sin`: table lookup, not `std::sin`.
[[nodiscard]] inline auto sin(double value) -> float {
    const auto index = static_cast<std::size_t>(
        static_cast<jlong>(value * 10430.378350470453) & 65535LL);
    return detail::sin_table()[index];
}

/// `Mth.sin(float)`: float arithmetic before the table index.
[[nodiscard]] inline auto sin(float value) -> float {
    const auto index = static_cast<std::size_t>(
        static_cast<jlong>(value * 10430.378F) & 65535LL);
    return detail::sin_table()[index];
}

/// `Mth.cos(float)`: float arithmetic before the table index.
[[nodiscard]] inline auto cos(float value) -> float {
    const auto index = static_cast<std::size_t>(
        static_cast<jlong>(value * 10430.378F + 16384.0F) & 65535LL);
    return detail::sin_table()[index];
}

/// `Mth.cos`: the sine table shifted by a quarter turn.
[[nodiscard]] inline auto cos(double value) -> float {
    const auto index = static_cast<std::size_t>(
        static_cast<jlong>(value * 10430.378350470453 + 16384.0) & 65535LL);
    return detail::sin_table()[index];
}

/// `Mth.floor`: truncation of `Math.floor`, i.e. the mathematical floor.
[[nodiscard]] inline auto floor(float value) -> jint {
    return static_cast<jint>(std::floor(value));
}

[[nodiscard]] inline auto floor(double value) -> jint {
    return static_cast<jint>(std::floor(value));
}

/// `Mth.lerp(float, float, float)`: `p0 + alpha * (p1 - p0)` in float.
[[nodiscard]] inline auto lerp(float alpha, float p0, float p1) -> float {
    return p0 + alpha * (p1 - p0);
}

/// `Mth.lerp2`: bilinear interpolation in float.
[[nodiscard]] inline auto lerp2(float alpha1, float alpha2, float x00,
                                float x10, float x01, float x11) -> float {
    return lerp(alpha2, lerp(alpha1, x00, x10), lerp(alpha1, x01, x11));
}

/// `Mth.lerp3`: trilinear interpolation in float.
[[nodiscard]] inline auto lerp3(float alpha1, float alpha2, float alpha3,
                                float x000, float x100, float x010, float x110,
                                float x001, float x101, float x011, float x111)
    -> float {
    return lerp(alpha3, lerp2(alpha1, alpha2, x000, x100, x010, x110),
                lerp2(alpha1, alpha2, x001, x101, x011, x111));
}

/// `Mth.smoothstep`: `x^3 * (x * (6x - 15) + 10)` in float.
[[nodiscard]] inline auto smoothstep(float x) -> float {
    return x * x * x * (x * (x * 6.0F - 15.0F) + 10.0F);
}

/// `Mth.smoothstepDerivative`: `30x^2 (x - 1)^2` in float.
[[nodiscard]] inline auto smoothstep_derivative(float x) -> float {
    return 30.0F * x * x * (x - 1.0F) * (x - 1.0F);
}

/// `Mth.lerp(double, double, double)`.
[[nodiscard]] inline auto lerp(double alpha, double p0, double p1) -> double {
    return p0 + alpha * (p1 - p0);
}

/// `Mth.inverseLerp`: `(value - min) / (max - min)`.
[[nodiscard]] inline auto inverse_lerp(double value, double min, double max)
    -> double {
    return (value - min) / (max - min);
}

/// `Mth.clampedLerp`: clamps the factor to `[0, 1]` before interpolating.
[[nodiscard]] inline auto clamped_lerp(double factor, double min, double max)
    -> double {
    if (factor < 0.0) {
        return min;
    }
    return factor > 1.0 ? max : lerp(factor, min, max);
}

/// `Mth.map`: linear remap without clamping.
/// `Mth.lengthSquared(float, float, float)`.
[[nodiscard]] inline auto length_squared(float x, float y, float z) -> float {
    return x * x + y * y + z * z;
}

/// `Mth.sqrt(float)`: the double square root narrowed back to float.
[[nodiscard]] inline auto sqrt(float value) -> float {
    return static_cast<float>(std::sqrt(static_cast<double>(value)));
}

/// `Mth.fastInvSqrt(double)`: the bit-hack reciprocal square root.
[[nodiscard]] inline auto fast_inv_sqrt(double value) -> double {
    const double half = 0.5 * value;
    std::int64_t bits = std::bit_cast<std::int64_t>(value);
    bits = 6910469410427058090LL - (bits >> 1);
    value = std::bit_cast<double>(bits);
    return value * (1.5 - half * value * value);
}

[[nodiscard]] inline auto map(double value, double from_min, double from_max,
                              double to_min, double to_max) -> double {
    return lerp(inverse_lerp(value, from_min, from_max), to_min, to_max);
}

/// `Mth.clampedMap`: linear remap with the factor clamped to `[0, 1]`.
[[nodiscard]] inline auto clamped_map(double value, double from_min,
                                      double from_max, double to_min,
                                      double to_max) -> double {
    return clamped_lerp(inverse_lerp(value, from_min, from_max), to_min,
                        to_max);
}

/// `Mth.quantize`: `floor(value / resolution) * resolution` as an int.
[[nodiscard]] inline auto quantize(double value, jint resolution) -> jint {
    return floor(value / static_cast<double>(resolution)) * resolution;
}

[[nodiscard]] inline auto square(double value) -> double {
    return value * value;
}

} // namespace refrain::worldgen::mth
