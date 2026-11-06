// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The subset of `net.minecraft.util.Mth` that world generation evaluates on
// every block or cell. Each function mirrors the Java expression, including
// its float/double types and evaluation order; see
// `doc/spec/worldgen/noise.md` for the source review.

#pragma once

#include <refrain/worldgen/java_compat.hpp>

#include <cmath>

namespace refrain::worldgen::mth {

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

[[nodiscard]] inline auto square(double value) -> double {
    return value * value;
}

} // namespace refrain::worldgen::mth
