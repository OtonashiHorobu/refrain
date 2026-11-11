// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcast.hpp>

#include <boost/ut.hpp>

using namespace boost::ut;
using namespace refrain::server;

namespace {

[[nodiscard]] auto at(double x, double y, double z, float yaw = 0.0F,
                      float pitch = 0.0F) -> refrain::net::movement_event {
    return refrain::net::movement_event{
        .connection = 1,
        .x = x,
        .y = y,
        .z = z,
        .yaw = yaw,
        .pitch = pitch,
        .on_ground = true,
    };
}

} // namespace

suite<"server/entity-broadcast"> entity_broadcast_tests = [] {
    "a vertical jump beyond the delta range is a teleport"_test = [] {
        const auto move{
            choose_entity_move(at(0.0, 64.0, 0.0), at(0.0, 200.0, 0.0))};
        expect(move.kind == entity_move_kind::teleport);
    };

    "a small step becomes a delta packet"_test = [] {
        const auto move{
            choose_entity_move(at(0.0, 64.0, 0.0), at(0.25, 64.0, -0.25))};
        expect(move.kind == entity_move_kind::position);
        expect(move.dx == 1024) << "0.25 blocks in 1/4096 units";
        expect(move.dy == 0);
        expect(move.dz == -1024);
    };

    "a step that turns as well carries the rotation"_test = [] {
        const auto move{choose_entity_move(at(0.0, 64.0, 0.0),
                                           at(0.0, 64.0, 1.0, 90.0F, 0.0F))};
        expect(move.kind == entity_move_kind::position_rotation);
        expect(move.dz == 4096);
    };

    "a turn in place becomes a rotation packet"_test = [] {
        const auto move{choose_entity_move(at(1.0, 64.0, 2.0),
                                           at(1.0, 64.0, 2.0, 0.0F, 45.0F))};
        expect(move.kind == entity_move_kind::rotation);
        expect(move.dx == 0 && move.dy == 0 && move.dz == 0);
    };

    "an unchanged position produces no packet"_test = [] {
        const auto move{choose_entity_move(at(1.0, 64.0, 2.0, 10.0F, 5.0F),
                                           at(1.0, 64.0, 2.0, 10.0F, 5.0F))};
        expect(move.kind == entity_move_kind::none);
    };

    "a step beyond the delta range falls back to a teleport"_test = [] {
        const auto move{
            choose_entity_move(at(0.0, 64.0, 0.0), at(16.0, 64.0, 0.0))};
        expect(move.kind == entity_move_kind::teleport);
    };

    "the tracking range includes the boundary"_test = [] {
        const auto viewer{at(0.0, 64.0, 0.0)};
        expect(within_tracking_range(viewer, at(64.0, 64.0, 0.0), 64.0));
        expect(!within_tracking_range(viewer, at(64.5, 64.0, 0.0), 64.0));
        expect(within_tracking_range(viewer, at(0.0, 128.0, 0.0), 64.0));
        expect(!within_tracking_range(viewer, at(0.0, 129.0, 0.0), 64.0));
    };
};
