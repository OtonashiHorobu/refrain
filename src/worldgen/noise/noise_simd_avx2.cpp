// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// AVX2 bulk path of the Perlin volume kernels. Compiling this translation
// unit with Highway's AVX2 static target keeps the intrinsic target attribute
// on the kernel alone; `perlin_noise::add_to_volume` calls it only after
// `__builtin_cpu_supports("avx2")` succeeds, so the baseline build still runs
// on CPUs without AVX2.

#include "noise_detail.hpp"

#include <refrain/worldgen/core/mth.hpp>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

#define HWY_BASELINE_TARGETS HWY_AVX2
#include <hwy/highway.h>

#include <limits>

namespace refrain::worldgen {

namespace {

namespace hn = hwy::HWY_NAMESPACE;

using simd_f32 = hn::ScalableTag<float>;
using simd_f64_half = hn::Half<simd_f32>;
using simd_f64 = hn::Rebind<double, simd_f64_half>;

/// `wrap()` over one double-lane block. The branch split matches the scalar
/// helper and the reduction keeps the scalar expression order.
[[nodiscard]] HWY_ATTR auto wrap_vector(hn::Vec<simd_f64> value)
    -> hn::Vec<simd_f64> {
    const simd_f64 d;
    static const double half_round_off =
        std::nextafter(noise_detail::half_wrap, 0.0);
    const auto inside = hn::And(hn::Ge(value, hn::Set(d, -half_round_off)),
                                hn::Lt(value, hn::Set(d, half_round_off)));
    // Terrain coordinates are far inside the stable range, so the reduction
    // is dead for every block a chunk produces; skip it when every lane keeps
    // the scalar helper's fast branch.
    if (hn::AllTrue(d, inside)) {
        return value;
    }
    const auto period = hn::Set(d, noise_detail::wrap_period);
    const auto reduced = hn::Sub(
        value,
        hn::Mul(hn::Floor(hn::Add(hn::Div(value, period), hn::Set(d, 0.5))),
                period));
    return hn::IfThenElse(inside, value, reduced);
}

/// `mth::smoothstep` over one float block, same operation order as the scalar
/// helper.
[[nodiscard]] HWY_ATTR auto smoothstep_vector(hn::Vec<simd_f32> x)
    -> hn::Vec<simd_f32> {
    const simd_f32 d;
    const auto inner = hn::Add(
        hn::Mul(hn::Sub(hn::Mul(x, hn::Set(d, 6.0F)), hn::Set(d, 15.0F)), x),
        hn::Set(d, 10.0F));
    return hn::Mul(hn::Mul(hn::Mul(x, x), x), inner);
}

/// `mth::lerp` with a scalar alpha.
[[nodiscard]] HWY_ATTR auto lerp_vector(float alpha, hn::Vec<simd_f32> p0,
                                        hn::Vec<simd_f32> p1)
    -> hn::Vec<simd_f32> {
    return hn::Add(p0, hn::Mul(hn::Set(simd_f32{}, alpha), hn::Sub(p1, p0)));
}

/// `mth::lerp` with a per-lane alpha.
[[nodiscard]] HWY_ATTR auto
lerp_vector(hn::Vec<simd_f32> alpha, hn::Vec<simd_f32> p0, hn::Vec<simd_f32> p1)
    -> hn::Vec<simd_f32> {
    return hn::Add(p0, hn::Mul(alpha, hn::Sub(p1, p0)));
}

} // namespace

HWY_ATTR void
perlin_volume_avx2(const noise_detail::perlin_volume_parameters &parameters,
                   buffer &output, const volume &vol, double xz_scale,
                   double y_scale, float amplitude) {
    const simd_f32 d32;
    const simd_f64_half d32_half;
    const simd_f64 d64;
    const auto lanes = static_cast<jint>(hn::Lanes(d32));
    const auto half_lanes = static_cast<jint>(hn::Lanes(d64));
    const auto y_step = hn::Set(d64, static_cast<double>(vol.step_block_y()));
    const auto y_base = hn::Set(d64, static_cast<double>(vol.min_block_y()));
    const auto y_scale_v = hn::Set(d64, y_scale);
    const auto offset_y_v = hn::Set(d64, parameters.offset_y);
    const auto fudge_scale_v = hn::Set(d64, parameters.fudge.value);
    const auto fudge_reciprocal_v = hn::Set(d64, parameters.fudge.reciprocal);
    const auto fudge_rounding_v = hn::Set(d64, 1.0E-7);
    const bool exact_fudge_reciprocal = parameters.fudge.exact_reciprocal;

    std::size_t index = 0;
    for (jint index_z = 0; index_z < vol.size_z(); ++index_z) {
        const double z = noise_detail::wrap(vol.block_z(index_z) * xz_scale) +
                         parameters.offset_z;
        const jint floor_z = mth::floor(z);
        const float relative_z = static_cast<float>(z - floor_z);
        const float alpha_z = mth::smoothstep(relative_z);

        for (jint index_x = 0; index_x < vol.size_x(); ++index_x) {
            const double x =
                noise_detail::wrap(vol.block_x(index_x) * xz_scale) +
                parameters.offset_x;
            const jint floor_x = mth::floor(x);
            const float relative_x = static_cast<float>(x - floor_x);
            const jint x0 = noise_detail::permute(parameters.perms, floor_x);
            const jint x1 =
                noise_detail::permute(parameters.perms, floor_x + 1);
            const float alpha_x = mth::smoothstep(relative_x);

            noise_detail::perlin_corner_terms terms;
            jint last_floor_y = std::numeric_limits<jint>::min();
            const auto refresh = [&](jint floor_y) {
                noise_detail::update_corner_terms(parameters.perms, x0, x1,
                                                  floor_z, floor_y, relative_x,
                                                  relative_z, terms);
                last_floor_y = floor_y;
            };
            const auto sample_scalar = [&](jint index_y) {
                const double original_y = vol.block_y(index_y) * y_scale;
                const double y =
                    noise_detail::wrap(original_y) + parameters.offset_y;
                const jint floor_y = mth::floor(y);
                const double relative_y_double =
                    y - static_cast<double>(floor_y);
                const float relative_y = static_cast<float>(relative_y_double);
                const float alpha_y = mth::smoothstep(relative_y);
                if (last_floor_y != floor_y) {
                    refresh(floor_y);
                }
                float fudged_relative_y = relative_y;
                if (parameters.smeared) {
                    fudged_relative_y = static_cast<float>(
                        relative_y_double -
                        noise_detail::smeared_fudge_y(
                            original_y, relative_y_double, parameters.fudge));
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
            };

            jint index_y = 0;
            while (index_y + lanes <= vol.size_y()) {
                const auto lane_lo =
                    hn::Iota(d64, static_cast<double>(index_y));
                const auto lane_hi =
                    hn::Iota(d64, static_cast<double>(index_y + half_lanes));
                const auto block_y_lo =
                    hn::Add(hn::Mul(lane_lo, y_step), y_base);
                const auto block_y_hi =
                    hn::Add(hn::Mul(lane_hi, y_step), y_base);
                const auto original_lo = hn::Mul(block_y_lo, y_scale_v);
                const auto original_hi = hn::Mul(block_y_hi, y_scale_v);
                const auto y_lo = hn::Add(wrap_vector(original_lo), offset_y_v);
                const auto y_hi = hn::Add(wrap_vector(original_hi), offset_y_v);
                const auto floored_lo = hn::Floor(y_lo);
                const auto floored_hi = hn::Floor(y_hi);

                // The floor of the first lane names the shared floor for the
                // whole block; a block that straddles a floor change falls
                // back to the scalar path.
                const jint first_floor =
                    static_cast<jint>(hn::GetLane(floored_lo));
                const auto floor_target =
                    hn::Set(d64, static_cast<double>(first_floor));
                if (!hn::AllTrue(d64, hn::Eq(floored_lo, floor_target)) ||
                    !hn::AllTrue(d64, hn::Eq(floored_hi, floor_target))) {
                    for (jint lane = 0; lane < lanes; ++lane) {
                        sample_scalar(index_y + lane);
                    }
                    index_y += lanes;
                    continue;
                }
                if (last_floor_y != first_floor) {
                    refresh(first_floor);
                }

                const auto relative_lo_double = hn::Sub(y_lo, floored_lo);
                const auto relative_hi_double = hn::Sub(y_hi, floored_hi);
                const auto relative_lo =
                    hn::DemoteTo(d32_half, relative_lo_double);
                const auto relative_hi =
                    hn::DemoteTo(d32_half, relative_hi_double);
                const auto relative_y =
                    hn::Combine(d32, relative_hi, relative_lo);
                const auto alpha_y = smoothstep_vector(relative_y);

                hn::Vec<simd_f32> fudged = relative_y;
                if (parameters.smeared) {
                    const auto in_range_lo =
                        hn::And(hn::Ge(original_lo, hn::Zero(d64)),
                                hn::Lt(original_lo, relative_lo_double));
                    const auto in_range_hi =
                        hn::And(hn::Ge(original_hi, hn::Zero(d64)),
                                hn::Lt(original_hi, relative_hi_double));
                    const auto limit_lo = hn::IfThenElse(
                        in_range_lo, original_lo, relative_lo_double);
                    const auto limit_hi = hn::IfThenElse(
                        in_range_hi, original_hi, relative_hi_double);
                    // A power-of-two scale rounds the same through a multiply
                    // by its exact reciprocal as through the division.
                    const auto quotient_lo =
                        exact_fudge_reciprocal
                            ? hn::Mul(limit_lo, fudge_reciprocal_v)
                            : hn::Div(limit_lo, fudge_scale_v);
                    const auto quotient_hi =
                        exact_fudge_reciprocal
                            ? hn::Mul(limit_hi, fudge_reciprocal_v)
                            : hn::Div(limit_hi, fudge_scale_v);
                    const auto fudge_lo = hn::Mul(
                        hn::Floor(hn::Add(quotient_lo, fudge_rounding_v)),
                        fudge_scale_v);
                    const auto fudge_hi = hn::Mul(
                        hn::Floor(hn::Add(quotient_hi, fudge_rounding_v)),
                        fudge_scale_v);
                    fudged = hn::Combine(
                        d32,
                        hn::DemoteTo(d32_half,
                                     hn::Sub(relative_hi_double, fudge_hi)),
                        hn::DemoteTo(d32_half,
                                     hn::Sub(relative_lo_double, fudge_lo)));
                }

                const auto p000 =
                    hn::Add(hn::Set(d32, terms.d000xz),
                            hn::Mul(hn::Set(d32, terms.g000y), fudged));
                const auto p100 =
                    hn::Add(hn::Set(d32, terms.d100xz),
                            hn::Mul(hn::Set(d32, terms.g100y), fudged));
                const auto p010 =
                    hn::Add(hn::Set(d32, terms.d010xz),
                            hn::Mul(hn::Set(d32, terms.g010y),
                                    hn::Sub(fudged, hn::Set(d32, 1.0F))));
                const auto p110 =
                    hn::Add(hn::Set(d32, terms.d110xz),
                            hn::Mul(hn::Set(d32, terms.g110y),
                                    hn::Sub(fudged, hn::Set(d32, 1.0F))));
                const auto p001 =
                    hn::Add(hn::Set(d32, terms.d001xz),
                            hn::Mul(hn::Set(d32, terms.g001y), fudged));
                const auto p101 =
                    hn::Add(hn::Set(d32, terms.d101xz),
                            hn::Mul(hn::Set(d32, terms.g101y), fudged));
                const auto p011 =
                    hn::Add(hn::Set(d32, terms.d011xz),
                            hn::Mul(hn::Set(d32, terms.g011y),
                                    hn::Sub(fudged, hn::Set(d32, 1.0F))));
                const auto p111 =
                    hn::Add(hn::Set(d32, terms.d111xz),
                            hn::Mul(hn::Set(d32, terms.g111y),
                                    hn::Sub(fudged, hn::Set(d32, 1.0F))));
                const auto top =
                    lerp_vector(alpha_y, lerp_vector(alpha_x, p000, p100),
                                lerp_vector(alpha_x, p010, p110));
                const auto bottom =
                    lerp_vector(alpha_y, lerp_vector(alpha_x, p001, p101),
                                lerp_vector(alpha_x, p011, p111));
                const auto value = hn::Mul(hn::Set(d32, amplitude),
                                           lerp_vector(alpha_z, top, bottom));
                auto *slot = output.data() + index;
                hn::StoreU(hn::Add(hn::LoadU(d32, slot), value), d32, slot);
                index += static_cast<std::size_t>(lanes);
                index_y += lanes;
            }
            for (; index_y < vol.size_y(); ++index_y) {
                sample_scalar(index_y);
            }
        }
    }
}

} // namespace refrain::worldgen

#endif // x86_64
