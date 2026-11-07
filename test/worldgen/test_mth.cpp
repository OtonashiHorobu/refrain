// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Vectors generated from the real 26.3 class with
// `tools/worldgen/vectors/run.sh <server.jar> MthVectors`.

#include <refrain/worldgen/core/mth.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <cstring>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

auto dbits(double value) -> std::uint64_t {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

auto fbits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

} // namespace

suite<"worldgen/mth"> mth_tests = [] {
    "map and clampedMap match the reference bits"_test = [] {
        expect(dbits(mth::map(0.3, 0.0, 1.0, -0.8, 0.4)) ==
               0xbfdc28f5c28f5c29ULL);
        expect(dbits(mth::map(2.0, 0.0, 1.0, -0.8, 0.4)) ==
               0x3ff999999999999bULL);
        expect(dbits(mth::clamped_map(0.3, 0.0, 64.0, 1.0, 0.0)) ==
               0x3fefd9999999999aULL);
        expect(dbits(mth::clamped_map(-5.0, 0.0, 64.0, 1.0, 0.0)) ==
               0x3ff0000000000000ULL);
        expect(dbits(mth::clamped_map(70.0, 0.0, 64.0, 1.0, 0.0)) ==
               0x0000000000000000ULL);
        expect(dbits(mth::inverse_lerp(3.0, 1.0, 5.0)) ==
               0x3fe0000000000000ULL);
        expect(dbits(mth::clamped_lerp(-0.5, 2.0, 6.0)) ==
               0x4000000000000000ULL);
        expect(dbits(mth::clamped_lerp(1.5, 2.0, 6.0)) ==
               0x4018000000000000ULL);
        expect(dbits(mth::clamped_lerp(0.25, 2.0, 6.0)) ==
               0x4008000000000000ULL);
    };

    "quantize truncates toward negative infinity"_test = [] {
        expect(mth::quantize(-3.7, 3) == -6);
        expect(mth::quantize(4.9, 3) == 3);
        expect(mth::quantize(-0.1, 3) == -3);
    };

    "float variants keep float arithmetic"_test = [] {
        expect(fbits(mth::map(0.75F, 0.0F, 1.0F, -1.0F, 3.0F)) == 0x40000000U);
        expect(fbits(mth::clamped_map(1.5F, 0.0F, 1.0F, -1.0F, 3.0F)) ==
               0x40400000U);
    };
};
