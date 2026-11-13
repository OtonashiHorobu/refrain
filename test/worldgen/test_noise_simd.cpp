// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The accelerated Perlin volume kernel must reproduce the baseline kernel bit
// for bit: it is only usable because the bulk path it replaces rounds each
// sample identically. This suite runs both implementations over volumes that
// exercise the cases the vector path splits on (shared floors, floors changing
// between lanes, the smeared fudge with and without an exact reciprocal, and
// coordinates that need the wrap reduction) and compares every float.
//
// The shared header is the contract between the two kernels, so the test reads
// it directly instead of going through the virtual `add_to_volume`, which only
// runs whichever implementation the CPU happens to select.

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>
#include <refrain/worldgen/noise/noise.hpp>

#include "../../src/worldgen/noise/noise_detail.hpp"

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

auto bits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

struct kernel_case {
    const char *name;
    volume vol;
    double xz_scale;
    double y_scale;
    float amplitude;
    double fudge_y_scale;
    bool smeared;
};

auto parameter_block(const perlin_noise &noise, const kernel_case &value)
    -> noise_detail::perlin_volume_parameters {
    return noise_detail::perlin_volume_parameters{
        .perms = nullptr,
        .offset_x = noise.offset_x(),
        .offset_y = noise.offset_y(),
        .offset_z = noise.offset_z(),
        .fudge = noise_detail::make_smear_scale(value.fudge_y_scale),
        .smeared = value.smeared,
    };
}

auto permutation_of(const perlin_noise &noise)
    -> std::array<std::uint8_t, 256> {
    std::array<std::uint8_t, 256> perms{};
    for (std::size_t i = 0; i < perms.size(); ++i) {
        perms[i] = noise.perm(i);
    }
    return perms;
}

void check_case(const perlin_noise &noise, const kernel_case &value) {
    const auto perms = permutation_of(noise);
    auto parameters = parameter_block(noise, value);
    parameters.perms = perms.data();

    buffer baseline{value.vol.size()};
    buffer accelerated{value.vol.size()};
    baseline.fill(0.0F);
    accelerated.fill(0.0F);

    noise_detail::perlin_volume_scalar(parameters, baseline, value.vol,
                                       value.xz_scale, value.y_scale,
                                       value.amplitude);
    perlin_volume_avx2(parameters, accelerated, value.vol, value.xz_scale,
                       value.y_scale, value.amplitude);

    for (std::size_t i = 0; i < baseline.size(); ++i) {
        expect(bits(baseline.get(i)) == bits(accelerated.get(i)))
            << value.name << " sample " << i << ": scalar " << baseline.get(i)
            << ", vector " << accelerated.get(i);
    }
}

auto make_cases() -> std::vector<kernel_case> {
    std::vector<kernel_case> cases;
    // Floors change on every Y sample: the unit-step case the vector path can
    // only take as a scalar fallback.
    cases.push_back(kernel_case{
        .name = "unit step",
        .vol = volume{5, 33, 5, -3, -64, 7, 4, 8, 4},
        .xz_scale = 0.25,
        .y_scale = 0.125,
        .amplitude = 1.0F,
        .fudge_y_scale = 8.0,
        .smeared = false,
    });
    // Y steps below one lattice unit share a floor for several lanes.
    cases.push_back(kernel_case{
        .name = "shared floors",
        .vol = volume{3, 32, 3, 17, 0, -19, 8, 8, 8},
        .xz_scale = 0.5,
        .y_scale = 0.0009765625,
        .amplitude = -0.375F,
        .fudge_y_scale = 8.0,
        .smeared = false,
    });
    // Lanes that straddle a floor change.
    cases.push_back(kernel_case{
        .name = "straddling floors",
        .vol = volume{4, 40, 4, 101, 12, 55, 1, 8, 1},
        .xz_scale = 1.0,
        .y_scale = 0.0625,
        .amplitude = 2.5F,
        .fudge_y_scale = 8.0,
        .smeared = false,
    });
    // The smeared variant: the fudge divisor is a power of two here, so both
    // kernels take the reciprocal-multiply path.
    cases.push_back(kernel_case{
        .name = "smeared power-of-two fudge",
        .vol = volume{5, 33, 5, -3, -64, 7, 4, 8, 4},
        .xz_scale = 0.25,
        .y_scale = 0.125,
        .amplitude = 1.0F,
        .fudge_y_scale = 8.0,
        .smeared = true,
    });
    // A divisor that is not a power of two keeps the division.
    cases.push_back(kernel_case{
        .name = "smeared non-power-of-two fudge",
        .vol = volume{3, 24, 3, 5, 9, -7, 2, 4, 2},
        .xz_scale = 0.375,
        .y_scale = 0.07,
        .amplitude = 0.75F,
        .fudge_y_scale = 7.0,
        .smeared = true,
    });
    // Coordinates outside the stable range take the wrap reduction in both
    // kernels.
    cases.push_back(kernel_case{
        .name = "wrapped coordinates",
        .vol = volume{2, 9, 2, 67108800, 33554400, -67108832, 32, 16, 32},
        .xz_scale = 0.25,
        .y_scale = 0.5,
        .amplitude = 1.0F,
        .fudge_y_scale = 8.0,
        .smeared = false,
    });
    return cases;
}

} // namespace

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

suite<"worldgen/noise-simd"> noise_simd_tests = [] {
    "the accelerated volume kernel reproduces the baseline kernel"_test = [] {
        if (!__builtin_cpu_supports("avx2")) {
            return;
        }
        legacy_random_source random{12345};
        const perlin_noise noise{random};
        for (const auto &value : make_cases()) {
            check_case(noise, value);
        }
    };
};

#endif
