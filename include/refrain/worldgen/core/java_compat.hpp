// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Java semantics used by the world generator.
//
// World generation is defined by the reference implementation's arithmetic, and
// Java's integer model differs from C++ in ways that silently change results:
// signed overflow wraps instead of being undefined, division truncates toward
// zero, `>>>` is a logical shift and `Integer.remainderUnsigned` has no C++
// counterpart. These helpers spell the Java behaviour out once, so algorithm
// code never repeats the casts.

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace refrain::worldgen {

/// Java `Math.min(float, float)`: NaN wins and negative zero sorts below
/// positive zero.
[[nodiscard]] inline auto java_min(float left, float right) -> float {
    if (std::isnan(left) || std::isnan(right)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (left == 0.0F && right == 0.0F) {
        return std::signbit(left) ? left : right;
    }
    return left < right ? left : right;
}

/// Java `Math.max(float, float)`.
[[nodiscard]] inline auto java_max(float left, float right) -> float {
    if (std::isnan(left) || std::isnan(right)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (left == 0.0F && right == 0.0F) {
        return std::signbit(left) ? right : left;
    }
    return left > right ? left : right;
}

using jint = std::int32_t;
using jlong = std::int64_t;

/// Java's `int` multiplication: wraps modulo 2^32. C++ signed overflow is
/// undefined, so the multiplication happens in `std::uint32_t`.
[[nodiscard]] constexpr auto wrap_mul(jint a, jint b) noexcept -> jint {
    return static_cast<jint>(static_cast<std::uint32_t>(a) *
                             static_cast<std::uint32_t>(b));
}

/// Java's `int` addition with wrap-around.
[[nodiscard]] constexpr auto wrap_add(jint a, jint b) noexcept -> jint {
    return static_cast<jint>(static_cast<std::uint32_t>(a) +
                             static_cast<std::uint32_t>(b));
}

/// Java's `int` subtraction with wrap-around.
[[nodiscard]] constexpr auto wrap_sub(jint a, jint b) noexcept -> jint {
    return static_cast<jint>(static_cast<std::uint32_t>(a) -
                             static_cast<std::uint32_t>(b));
}

/// Java's narrowing `(int)` conversion: NaN becomes zero, values outside the
/// int range saturate at the nearer bound, and the rest truncate toward zero.
/// C++ leaves out-of-range conversions undefined, so arithmetic that can
/// overflow a count must pass through this helper.
[[nodiscard]] inline auto java_int_cast(double value) noexcept -> jint {
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= static_cast<double>(std::numeric_limits<jint>::max())) {
        return std::numeric_limits<jint>::max();
    }
    if (value <= static_cast<double>(std::numeric_limits<jint>::min())) {
        return std::numeric_limits<jint>::min();
    }
    return static_cast<jint>(value);
}

/// `Math.floorDiv(int, int)`. `INT32_MIN / -1` returns `INT32_MIN`, as in Java.
[[nodiscard]] constexpr auto floor_div(jint a, jint b) noexcept -> jint {
    if (b == 0) {
        return 0; // Java throws; callers never divide by zero.
    }
    if (a == INT32_MIN && b == -1) {
        return a;
    }
    jint quotient = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --quotient;
    }
    return quotient;
}

/// `Math.floorMod(int, int)`.
[[nodiscard]] constexpr auto floor_mod(jint a, jint b) noexcept -> jint {
    return a - (floor_div(a, b) * b);
}

/// `Math.floorDiv(long, long)`. `LONG_MIN / -1` returns `LONG_MIN`, as in
/// Java.
[[nodiscard]] constexpr auto floor_div(jlong a, jlong b) noexcept -> jlong {
    if (b == 0) {
        return 0; // Java throws; callers never divide by zero.
    }
    if (a == std::numeric_limits<jlong>::min() && b == -1) {
        return a;
    }
    jlong quotient = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --quotient;
    }
    return quotient;
}

/// `Math.floorMod(long, long)`.
[[nodiscard]] constexpr auto floor_mod(jlong a, jlong b) noexcept -> jlong {
    return a - (floor_div(a, b) * b);
}

/// Java's unsigned right shift on a 64-bit value.
[[nodiscard]] constexpr auto ushr(jlong value, int bits) noexcept -> jlong {
    return static_cast<jlong>(static_cast<std::uint64_t>(value) >>
                              static_cast<unsigned>(bits));
}

/// `Long.rotateLeft(long, int)`.
[[nodiscard]] constexpr auto rotate_left(jlong value, int bits) noexcept
    -> jlong {
    return static_cast<jlong>(
        std::rotl(static_cast<std::uint64_t>(value), bits));
}

/// `Integer.remainderUnsigned(int, int)`, used by Lemire's bounded draw.
[[nodiscard]] constexpr auto remainder_unsigned(jint dividend,
                                                jint divisor) noexcept -> jint {
    return static_cast<jint>(static_cast<std::uint32_t>(dividend) %
                             static_cast<std::uint32_t>(divisor));
}

/// `String.hashCode()` for ASCII text: `h = 31*h + c` with 32-bit wrap.
///
/// Resource locations are ASCII. Non-ASCII names would need UTF-16 code units;
/// the legacy positional factory never sees them (it is selected only by the
/// `legacy_random_source` presets).
[[nodiscard]] constexpr auto java_string_hash(std::string_view text) noexcept
    -> jint {
    jint hash = 0;
    for (const char ch : text) {
        hash = static_cast<jint>(static_cast<std::uint32_t>(hash) * 31U +
                                 static_cast<std::uint8_t>(ch));
    }
    return hash;
}

/// Java's float-to-double widening is exact; this alias documents the intent at
/// call sites where a `float` participates in a `double` expression.
[[nodiscard]] constexpr auto widen(float value) noexcept -> double {
    return static_cast<double>(value);
}

} // namespace refrain::worldgen
