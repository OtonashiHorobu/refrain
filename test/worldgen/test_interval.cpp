// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Interval vectors generated from the real 26.3 class with
// `tools/worldgen/vectors/run.sh <server.jar> IntervalVectors`.

#include <refrain/worldgen/density/interval.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

auto bits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr float nan_value = std::numeric_limits<float>::quiet_NaN();

struct expected {
    std::string_view name;
    std::uint32_t min;
    std::uint32_t max;
};

} // namespace

suite<"worldgen/interval"> interval_tests = [] {
    "interval operations match the reference bits"_test = [] {
        const interval a = interval::of(-2.0F, 3.0F);
        const interval b = interval::of(0.5F, 4.0F);
        const interval c = interval::of(-10.0F, -1.0F);
        const interval pos = interval::of(0.25F, 2.0F);
        const interval zero = interval::of_exact(0.0F);
        const interval inf = interval::infinite();
        const interval nai = interval::na_i();

        const auto check = [](std::string_view name, interval value,
                              std::uint32_t want_min, std::uint32_t want_max) {
            expect(bits(value.min()) == want_min) << name << " min";
            expect(bits(value.max()) == want_max) << name << " max";
        };

        check("of.exact", interval::of_exact(2.5F), 0x40200000U, 0x40200000U);
        check("of.symmetric", interval::of_symmetric(2.0F), 0xc0000000U,
              0x40000000U);
        check("add", interval::add(a, b), 0xbfc00000U, 0x40e00000U);
        check("add.nai", interval::add(a, nai), 0x7fc00000U, 0x7fc00000U);
        check("sub", interval::sub(a, b), 0xc0c00000U, 0x40200000U);
        check("mul.cross", interval::mul(c, b), 0xc2200000U, 0xbf000000U);
        check("mul.zero", interval::mul(zero, inf), 0x00000000U, 0x00000000U);
        check("div", interval::div(a, interval::of(1.0F, 2.0F)), 0xc0000000U,
              0x40400000U);
        check("div.through.zero", interval::div(a, interval::of(-1.0F, 1.0F)),
              0xff800000U, 0x7f800000U);
        check("min", interval::min(a, b), 0xc0000000U, 0x40400000U);
        check("max", interval::max(a, b), 0x3f000000U, 0x40800000U);
        check("clamp.inner", interval::clamp(a, -1.0F, 1.0F), 0xbf800000U,
              0x3f800000U);
        check("clamp.above", interval::clamp(a, 10.0F, 20.0F), 0x41200000U,
              0x41200000U);
        check("abs.cross", interval::abs(c), 0x3f800000U, 0x41200000U);
        check("abs.positive", interval::abs(pos), 0x3e800000U, 0x40000000U);
        check("square.cross", interval::square(a), 0x00000000U, 0x41100000U);
        check("square.negative", interval::square(c), 0x3f800000U, 0x42c80000U);
        check("pow.square", interval::pow(a, interval::of_exact(2.0F)),
              0x00000000U, 0x41100000U);
        check("pow.negative.base",
              interval::pow(interval::of(-2.0F, -1.0F),
                            interval::of(-3.0F, 3.0F)),
              0xc1000000U, 0x40800000U);
        check("pow.zero.base", interval::pow(zero, interval::of(-1.0F, 2.0F)),
              0x00000000U, 0x7f800000U);
        check("pow.positive.base", interval::pow(pos, interval::of(0.5F, 3.0F)),
              0x3c800000U, 0x41000000U);
        check("pow.infinite.exp", interval::pow(interval::of_exact(0.5F), inf),
              0x00000000U, 0x7f800000U);
        check("log", interval::log(interval::of(-1.0F, 4.0F)), 0xff800000U,
              0x3fb17218U);
        check("sign.zero.edge", interval::sign(interval::of(-3.0F, 0.0F)),
              0xbf800000U, 0x00000000U);
        check("sign.span", interval::sign(a), 0xbf800000U, 0x3f800000U);
        check("map.cube",
              interval::map_monotonic(c, [](float x) { return x * x * x; }),
              0xc47a0000U, 0xbf800000U);
        check("lerp.finite",
              interval::lerp(interval::of(0.25F, 0.75F), -2.0F, 5.0F),
              0xbe800000U, 0x40500000U);
        check("lerp.infinite",
              interval::lerp(interval::of(0.0F, 1.0F), -infinity, 3.0F),
              0xff800000U, 0x40400000U);
        check("lerp.intervals", interval::lerp(interval::of(0.0F, 1.0F), c, b),
              0xc1200000U, 0x40800000U);
        check("encap.intervals", interval::encapsulating(a, c), 0xc1200000U,
              0x40400000U);
        check("nai", nai, 0x7fc00000U, 0x7fc00000U);
    };
};
