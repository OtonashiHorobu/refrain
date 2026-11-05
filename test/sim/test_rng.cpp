// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/rng.hpp>

#include <boost/ut.hpp>

#include <cstdint>

namespace {
using namespace boost::ut;

[[maybe_unused]] const suite tests = [] {
    "sim/rng same seed produces the same stream"_test = [] {
        refrain::sim::rng left{0x123456789ABCDEF0ULL};
        refrain::sim::rng right{0x123456789ABCDEF0ULL};
        for (int index{0}; index < 64; ++index) {
            expect(left.next_u64() == right.next_u64());
        }
        expect(left.state0() == right.state0());
        expect(left.state1() == right.state1());
    };

    "sim/rng different seeds diverge"_test = [] {
        refrain::sim::rng left{1};
        refrain::sim::rng right{2};
        bool differs{false};
        for (int index{0}; index < 8; ++index) {
            differs = differs || left.next_u64() != right.next_u64();
        }
        expect(differs);
    };

    "sim/rng bounded and float ranges"_test = [] {
        refrain::sim::rng generator{42};
        for (int index{0}; index < 512; ++index) {
            expect(generator.next_bounded(6) < 6U);
            const auto value{generator.next_float()};
            expect(value >= 0.0F && value < 1.0F);
        }
        expect(generator.next_bounded(0) == 0U);
    };
};

} // namespace
