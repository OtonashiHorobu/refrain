// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/chunk_tracker.hpp>

#include <boost/ut.hpp>

#include <cstdint>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace session = refrain::session;

/// Timestamps are explicit so the estimate does not depend on how long the
/// test binary takes to reach the next statement.
constexpr std::uint64_t second = 1'000'000'000;

suite chunk_tracker_tests = [] {
    "session/chunk tracker emits viewport changes only"_test = [] {
        session::chunk_tracker tracker{};
        expect(tracker.update(1, 4, 6, 3));
        auto requests{tracker.take_requests()};
        expect(requests.size() == 1U);
        if (!requests.empty()) {
            expect(requests.front().connection == 1U);
            expect(requests.front().radius == 3);
        }
        expect(!tracker.update(1, 4, 6, 3));
        expect(tracker.take_requests().empty());
        expect(tracker.update(1, 5, 6, 3));
        requests = tracker.take_requests();
        expect(requests.size() == 1U);
        expect(tracker.remove(1));
        expect(!tracker.update(0, 0, 0, 3));
    };
};

suite chunk_tracker_prediction_tests = [] {
    "session/chunk tracker activates a look-ahead after two same-direction changes"_test =
        [] {
            session::chunk_tracker tracker{};
            expect(tracker.update(1, 0, 0, 3, 0));
            expect(tracker.update(1, 1, 0, 3, 2 * second));
            // One change is a direction hint, not a heading.
            const auto first{tracker.lookahead(1, 2 * second)};
            expect(!first.active);
            expect(first.offset_x == 0);
            expect(first.heading_streak == 1U);

            expect(tracker.update(1, 2, 0, 3, 4 * second));
            const auto predicted{tracker.lookahead(1, 4 * second)};
            expect(predicted.active);
            expect(predicted.heading_streak == 2U);
            expect(predicted.offset_x == 1);
            expect(predicted.offset_z == 0);
            expect(predicted.chunk_x == 3);
            expect(predicted.chunk_z == 0);
            expect(predicted.velocity_x > 0.4 && predicted.velocity_x < 0.6);
        };

    "session/chunk tracker collapses the look-ahead on a reversal"_test = [] {
        session::chunk_tracker tracker{};
        static_cast<void>(tracker.update(1, 0, 0, 3, 0));
        static_cast<void>(tracker.update(1, 1, 0, 3, 2 * second));
        static_cast<void>(tracker.update(1, 2, 0, 3, 4 * second));
        expect(tracker.lookahead(1, 4 * second).active);

        // One step against the heading: no further look-ahead in the old
        // direction, on the same update.
        static_cast<void>(tracker.update(1, 1, 0, 3, 6 * second));
        const auto reversed{tracker.lookahead(1, 6 * second)};
        expect(!reversed.active);
        expect(reversed.offset_x == 0);
        expect(reversed.offset_z == 0);
    };

    "session/chunk tracker needs one change to return to a new heading"_test =
        [] {
            session::chunk_tracker tracker{};
            static_cast<void>(tracker.update(1, 0, 0, 3, 0));
            static_cast<void>(tracker.update(1, 1, 0, 3, 2 * second));
            static_cast<void>(tracker.update(1, 2, 0, 3, 4 * second));
            static_cast<void>(tracker.update(1, 1, 0, 3, 6 * second));
            // Two steps against the old heading are where the new one starts
            // counting, so a single reversal is not a new heading either.
            expect(!tracker.lookahead(1, 6 * second).active);
            static_cast<void>(tracker.update(1, 0, 0, 3, 8 * second));
            expect(!tracker.lookahead(1, 8 * second).active);
            static_cast<void>(tracker.update(1, -1, 0, 3, 10 * second));
            const auto back{tracker.lookahead(1, 10 * second)};
            expect(back.active);
            expect(back.offset_x == -1);
            expect(back.chunk_x == -2);
        };

    "session/chunk tracker expires a look-ahead that stopped moving"_test = [] {
        session::chunk_tracker tracker{};
        static_cast<void>(tracker.update(1, 0, 0, 3, 0));
        static_cast<void>(tracker.update(1, 1, 0, 3, 2 * second));
        static_cast<void>(tracker.update(1, 2, 0, 3, 4 * second));
        expect(tracker.lookahead(1, 4 * second).active);
        // A player who stopped inside a chunk does not pin generation: the
        // last accepted change ages out instead of predicting forever.
        expect(!tracker
                    .lookahead(1, 4 * second +
                                      session::chunk_tracker::stale_after_ns +
                                      1)
                    .active);
    };

    "session/chunk tracker ignores updates that do not move the centre"_test =
        [] {
            session::chunk_tracker tracker{};
            static_cast<void>(tracker.update(1, 0, 0, 3, 0));
            static_cast<void>(tracker.update(1, 1, 0, 3, 2 * second));
            for (std::uint64_t at{2}; at < 10; ++at) {
                // Movement inside the chunk: same accepted centre, no new
                // sample, and no velocity from the polling rate.
                expect(!tracker.update(1, 1, 0, 3, at * second));
            }
            expect(tracker.lookahead(1, 9 * second).heading_streak == 1U);
            expect(!tracker.lookahead(1, 9 * second).active);
        };

    "session/chunk tracker clamps a correction jump to the tile ahead"_test =
        [] {
            session::chunk_tracker tracker{};
            static_cast<void>(tracker.update(1, 0, 0, 3, 0));
            static_cast<void>(tracker.update(1, 1, 0, 3, 2 * second));
            // A rejected-then-corrected position can jump many chunks at
            // once. The look-ahead must stay one tile ahead instead of
            // projecting the jump.
            static_cast<void>(tracker.update(1, 40, 0, 3, 3 * second));
            const auto jump{tracker.lookahead(1, 3 * second)};
            expect(jump.active);
            expect(jump.offset_x <=
                   session::chunk_tracker::max_lookahead_chunks);
            expect(jump.chunk_x <=
                   40 + session::chunk_tracker::max_lookahead_chunks);
        };

    "session/chunk tracker treats a diagonal stair step as one heading"_test =
        [] {
            session::chunk_tracker tracker{};
            static_cast<void>(tracker.update(1, 0, 0, 3, 0));
            static_cast<void>(tracker.update(1, 1, 0, 3, 2 * second));
            static_cast<void>(tracker.update(1, 1, 1, 3, 4 * second));
            static_cast<void>(tracker.update(1, 2, 1, 3, 6 * second));
            static_cast<void>(tracker.update(1, 2, 2, 3, 8 * second));
            const auto diagonal{tracker.lookahead(1, 8 * second)};
            expect(diagonal.active);
            expect(diagonal.offset_x >= 1);
            expect(diagonal.offset_z >= 1);
            expect(diagonal.chunk_x == 3);
            expect(diagonal.chunk_z == 3);
        };

    "session/chunk tracker reports no look-ahead for an unknown connection"_test =
        [] {
            session::chunk_tracker tracker{};
            const auto unknown{tracker.lookahead(9, 0)};
            expect(!unknown.active);
            expect(unknown.offset_x == 0);
            expect(unknown.offset_z == 0);
        };

    "session/chunk tracker drops the estimate when the connection leaves"_test =
        [] {
            session::chunk_tracker tracker{};
            static_cast<void>(tracker.update(1, 0, 0, 3, 0));
            static_cast<void>(tracker.update(1, 1, 0, 3, 2 * second));
            static_cast<void>(tracker.update(1, 2, 0, 3, 4 * second));
            expect(tracker.lookahead(1, 4 * second).active);
            expect(tracker.remove(1));
            expect(!tracker.lookahead(1, 4 * second).active);
            // A rejoin starts a fresh history instead of inheriting the old
            // heading.
            static_cast<void>(tracker.update(1, 2, 0, 3, 5 * second));
            expect(!tracker.lookahead(1, 5 * second).active);
        };
};

} // namespace
