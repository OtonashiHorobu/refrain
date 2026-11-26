// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcast.hpp>

#include <boost/ut.hpp>

#include <limits>

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

    "a non-finite reported position produces no entity packet"_test = [] {
        const auto nan{std::numeric_limits<double>::quiet_NaN()};
        const auto nan_float{std::numeric_limits<float>::quiet_NaN()};
        const auto inf{std::numeric_limits<double>::infinity()};
        expect(
            choose_entity_move(at(0.0, 64.0, 0.0), at(nan, 64.0, 0.0)).kind ==
            entity_move_kind::none);
        expect(
            choose_entity_move(at(0.0, 64.0, 0.0), at(inf, 64.0, 0.0)).kind ==
            entity_move_kind::none);
        expect(choose_entity_move(at(0.0, 64.0, 0.0),
                                  at(0.0, 64.0, 0.0, inf, 0.0F))
                   .kind == entity_move_kind::none);
        expect(choose_entity_move(at(0.0, 64.0, 0.0),
                                  at(0.0, 64.0, 0.0, 0.0F, nan_float))
                   .kind == entity_move_kind::none);
    };

    "a non-finite previous position falls back to a teleport"_test = [] {
        const auto nan{std::numeric_limits<double>::quiet_NaN()};
        expect(
            choose_entity_move(at(nan, 64.0, 0.0), at(0.25, 64.0, 0.0)).kind ==
            entity_move_kind::teleport);
        expect(choose_entity_move(at(0.0, 64.0, 0.0), at(1.0e300, 64.0, 0.0))
                   .kind == entity_move_kind::teleport);
    };

    "a finite position predicate rejects a non-finite component"_test = [] {
        const auto nan{std::numeric_limits<double>::quiet_NaN()};
        const auto nan_float{std::numeric_limits<float>::quiet_NaN()};
        const auto inf{std::numeric_limits<double>::infinity()};
        expect(finite_position(0.5, 64.0, 0.5, 0.0F, 0.0F));
        expect(!finite_position(nan, 64.0, 0.5, 0.0F, 0.0F));
        expect(!finite_position(0.5, inf, 0.5, 0.0F, 0.0F));
        expect(!finite_position(0.5, 64.0, nan, 0.0F, 0.0F));
        expect(!finite_position(0.5, 64.0, 0.5, inf, 0.0F));
        expect(!finite_position(0.5, 64.0, 0.5, 0.0F, nan_float));
        expect(addressable_position(0.5, 64.0, 0.5));
        expect(addressable_position(world_horizontal_limit, 64.0,
                                    -world_horizontal_limit));
        expect(addressable_position(0.5, world_vertical_limit, 0.5));
        expect(!addressable_position(world_horizontal_limit + 1.0, 64.0, 0.5));
        expect(!addressable_position(0.5, 64.0, world_horizontal_limit + 1.0));
        expect(!addressable_position(0.5, world_vertical_limit + 1.0, 0.5));
        expect(!addressable_position(nan, 64.0, 0.5));
    };

    "the tracking range includes the boundary"_test = [] {
        const auto viewer{at(0.0, 64.0, 0.0)};
        expect(within_tracking_range(viewer, at(64.0, 64.0, 0.0), 64.0));
        expect(!within_tracking_range(viewer, at(64.5, 64.0, 0.0), 64.0));
        expect(within_tracking_range(viewer, at(0.0, 128.0, 0.0), 64.0));
        expect(!within_tracking_range(viewer, at(0.0, 129.0, 0.0), 64.0));
    };
};
