// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The runtime clamps a client-requested view distance against the configured
// one. The configuration loader already rejects an out-of-range configured
// distance, so this suite pins the defensive branch: an upper bound below the
// lower bound must not form an invalid clamp range.

#include <boost/ut.hpp>

#include <cstdint>

namespace refrain::server {

/// Defined in the server runtime translation unit; declared here so the
/// regression test can reach the defensive branch directly.
[[nodiscard]] auto clamp_view_distance(std::int32_t requested,
                                       std::int32_t configured) noexcept
    -> std::int32_t;

} // namespace refrain::server

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

suite<"server/view-distance"> view_distance_tests = [] {
    "a client view distance is clamped into the configured range"_test = [] {
        namespace server = refrain::server;
        expect(server::clamp_view_distance(0, 10) == 2);
        expect(server::clamp_view_distance(1, 10) == 2);
        expect(server::clamp_view_distance(5, 10) == 5);
        expect(server::clamp_view_distance(10, 10) == 10);
        expect(server::clamp_view_distance(200, 10) == 10);
        expect(server::clamp_view_distance(-1, 32) == 2);
        expect(server::clamp_view_distance(40, 32) == 32);
    };

    "an invalid configured distance collapses to the minimum instead of an "
    "invalid clamp range"_test = [] {
        namespace server = refrain::server;
        // The loader rejects these values; the clamp must still be defined for
        // a caller that bypasses it, with the lower bound as the upper bound.
        expect(server::clamp_view_distance(0, 0) == 2);
        expect(server::clamp_view_distance(10, 0) == 2);
        expect(server::clamp_view_distance(0, -5) == 2);
        expect(server::clamp_view_distance(40, 1) == 2);
        expect(server::clamp_view_distance(2, 1) == 2);
    };
};

} // namespace
