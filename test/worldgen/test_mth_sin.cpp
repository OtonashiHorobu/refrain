// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/core/mth.hpp>

#include <boost/ut.hpp>

#include <array>
#include <bit>
#include <cstdint>

namespace {

namespace mth = refrain::worldgen::mth;

auto bits(float value) -> std::uint32_t {
    return std::bit_cast<std::uint32_t>(value);
}

} // namespace

using namespace boost::ut;

suite<"worldgen/mth-sin"> mth_sin_tests = [] {
    "sin and cos match the Java table lookups"_test = [] {
        expect(bits(mth::sin(0.0)) == 0U);
        expect(bits(mth::cos(0.0)) == 1065353216U);
        expect(bits(mth::sin(1.0)) == 1062693212U);
        expect(bits(mth::cos(1.0)) == 1057641281U);
        expect(bits(mth::sin(0.5)) == 1056273710U);
        expect(bits(mth::cos(0.5)) == 1063299538U);
        expect(bits(mth::sin(-1.5)) == 3212794772U);
        expect(bits(mth::cos(-1.5)) == 1032898811U);
        expect(bits(mth::sin(12345.678)) == 3207872781U);
        expect(bits(mth::cos(12345.678)) == 1060489222U);
        expect(bits(mth::sin(-98765.4321)) == 3173825666U);
        expect(bits(mth::cos(-98765.4321)) == 1065338290U);
        expect(bits(mth::sin(10430.378350470453)) == 1049804519U);
        expect(bits(mth::cos(10430.378350470453)) == 1064649353U);
        expect(bits(mth::sin(65535.0)) == 1065039883U);
        expect(bits(mth::cos(65535.0)) == 1044708023U);
    };

    "the table fingerprint matches the Java table"_test = [] {
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        for (const auto value : refrain::worldgen::mth::detail::sin_table()) {
            hash ^= bits(value);
            hash *= 0x100000001b3ULL;
        }
        expect(hash == 0x21cb003ccaa34307ULL);
    };
};
