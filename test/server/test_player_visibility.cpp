// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/player_visibility.hpp>

#include <boost/ut.hpp>

using namespace boost::ut;
using namespace refrain::server;

namespace {

[[nodiscard]] auto viewer(std::string_view world, double x, double z,
                          std::int32_t view_distance = 10) -> visibility_query {
    return visibility_query{
        .world = world, .x = x, .z = z, .view_distance = view_distance};
}

} // namespace

suite<"server/player-visibility"> player_visibility_tests = [] {
    "the view distance clamps the tracking range"_test = [] {
        // Ten chunks of view distance is 160 blocks; the vanilla player range
        // is 32 chunks, so the view distance wins.
        expect(player_visible(viewer("world", 0.0, 0.0), "world", 160.0, 0.0,
                              player_tracking_chunks));
        expect(!player_visible(viewer("world", 0.0, 0.0), "world", 161.0, 0.0,
                               player_tracking_chunks));
    };

    "the distance is horizontal only"_test = [] {
        expect(player_visible(viewer("world", 0.0, 0.0), "world", 0.0, 150.0,
                              player_tracking_chunks));
    };

    "a shorter tracking range still applies"_test = [] {
        expect(
            player_visible(viewer("world", 0.0, 0.0), "world", 32.0, 0.0, 2));
        expect(
            !player_visible(viewer("world", 0.0, 0.0), "world", 33.0, 0.0, 2));
    };

    "another world is never visible"_test = [] {
        expect(!player_visible(viewer("world", 0.0, 0.0), "nether", 1.0, 1.0,
                               player_tracking_chunks));
    };

    "the entity chunk must stay inside the viewport"_test = [] {
        // 150 blocks away is inside the distance for a ten chunk view, and the
        // chunk is the tenth one from the viewer.
        expect(player_visible(viewer("world", 8.0, 8.0), "world", 150.0, 8.0,
                              player_tracking_chunks));
    };
};
