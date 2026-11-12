// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_generation.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

using refrain::server::admission_controller;
using refrain::server::admission_limits;
using refrain::server::admission_reason;
using refrain::server::admission_sample;
using refrain::server::farthest_column;
using refrain::server::rank_columns_by_distance;
using refrain::server::to_string;
using refrain::world::chunk_pos;

[[nodiscard]] auto sample(std::size_t queue_depth, double tick_p99_ms,
                          std::uint64_t resident_bytes = 0)
    -> admission_sample {
    return admission_sample{.queue_depth = queue_depth,
                            .tick_p99_ms = tick_p99_ms,
                            .resident_bytes = resident_bytes};
}

/// Defaults: 1..8 credits starting at 2, a 50 ms budget with a 40 ms raise
/// headroom, and a raise after three consecutive good seconds.
[[nodiscard]] auto default_limits() -> admission_limits {
    return admission_limits{};
}

suite<"server/chunk-admission"> admission_controller_tests = [] {
    "server/admission starts at the initial credit level"_test = [] {
        const admission_controller controller{
            admission_limits{.initial_credits = 3}};
        expect(controller.credits() == 3U);
    };

    "server/admission clamps the initial level into the bounds"_test = [] {
        expect(admission_controller{admission_limits{.min_credits = 2,
                                                     .max_credits = 4,
                                                     .initial_credits = 9}}
                   .credits() == 4U);
        expect(admission_controller{admission_limits{.min_credits = 2,
                                                     .max_credits = 4,
                                                     .initial_credits = 0}}
                   .credits() == 2U);
    };

    "server/admission raises one credit after three good samples"_test = [] {
        admission_controller controller{default_limits()};
        expect(!controller.update(sample(5, 10.0)).has_value());
        expect(!controller.update(sample(5, 10.0)).has_value());
        const auto change = controller.update(sample(5, 10.0));
        expect(change.has_value());
        expect(change->credits == 3U);
        expect(change->reason == admission_reason::queue_depth);
        expect(controller.credits() == 3U);
    };

    "server/admission resets the raise window on a weak sample"_test = [] {
        admission_controller controller{default_limits()};
        expect(!controller.update(sample(9, 10.0)).has_value());
        expect(!controller.update(sample(9, 10.0)).has_value());
        // Inside the budget, above the raise headroom: it neither raises nor
        // counts as one of the consecutive good samples.
        expect(!controller.update(sample(9, 45.0)).has_value());
        expect(!controller.update(sample(9, 10.0)).has_value());
        expect(!controller.update(sample(9, 10.0)).has_value());
        expect(controller.credits() == 2U);
        const auto change = controller.update(sample(9, 10.0));
        expect(change.has_value());
        expect(change->credits == 3U);
    };

    "server/admission raises only while demand exceeds the credits"_test = [] {
        admission_controller controller{default_limits()};
        for (int second{0}; second < 10; ++second) {
            // Queue depth equals the credit level: the workers can already
            // cover the demand, so more credits would buy nothing.
            expect(!controller.update(sample(2, 10.0)).has_value());
        }
        expect(controller.credits() == 2U);
        expect(!controller.update(sample(3, 10.0)).has_value());
        expect(!controller.update(sample(3, 10.0)).has_value());
        const auto change = controller.update(sample(3, 10.0));
        expect(change.has_value());
        expect(controller.credits() == 3U);
    };

    "server/admission cuts at once when the tick p99 misses the budget"_test =
        [] {
            admission_controller controller{
                admission_limits{.initial_credits = 6}};
            const auto change = controller.update(sample(20, 80.0));
            expect(change.has_value());
            expect(change->credits == 3U);
            expect(change->reason == admission_reason::tick_p99);
            expect(controller.credits() == 3U);
        };

    "server/admission reports no cut that the minimum already covers"_test =
        [] {
            admission_controller controller{
                admission_limits{.initial_credits = 1}};
            expect(!controller.update(sample(20, 80.0)).has_value());
            expect(controller.credits() == 1U);
        };

    "server/admission treats a p99 at the budget as inside"_test = [] {
        admission_controller controller{
            admission_limits{.initial_credits = 4, .budget_ms = 50.0}};
        expect(!controller.update(sample(9, 50.0)).has_value());
        expect(controller.credits() == 4U);
    };

    "server/admission cuts at once when resident memory crosses its bound"_test =
        [] {
            admission_controller controller{admission_limits{
                .initial_credits = 4, .memory_bound_bytes = 1024}};
            const auto change = controller.update(sample(9, 10.0, 2048));
            expect(change.has_value());
            expect(change->credits == 2U);
            expect(change->reason == admission_reason::memory);
        };

    "server/admission ignores an unreported memory sample"_test = [] {
        admission_controller controller{
            admission_limits{.initial_credits = 4, .memory_bound_bytes = 1024}};
        for (int second{0}; second < 4; ++second) {
            // No demand beyond the current credits, so the only input that
            // could move the level is memory; an unreported zero is ignored.
            expect(!controller.update(sample(4, 10.0, 0)).has_value());
        }
        expect(controller.credits() == 4U);
    };

    "server/admission follows the expected trajectory over a synthetic run"_test =
        [] {
            admission_controller controller{default_limits()};
            std::vector<std::size_t> raised{};
            for (int second{0}; second < 18; ++second) {
                if (const auto change = controller.update(sample(32, 10.0))) {
                    raised.push_back(change->credits);
                    expect(change->reason == admission_reason::queue_depth);
                }
            }
            expect(raised == std::vector<std::size_t>{3, 4, 5, 6, 7, 8});
            expect(controller.credits() == 8U);

            // A single missed second halves the level instead of waiting for
            // the hysteresis window; the next good samples ramp it back.
            const auto cut = controller.update(sample(32, 60.0));
            expect(cut.has_value());
            expect(cut->credits == 4U);
            expect(cut->reason == admission_reason::tick_p99);
            raised.clear();
            for (int second{0}; second < 12; ++second) {
                if (const auto change = controller.update(sample(32, 10.0))) {
                    raised.push_back(change->credits);
                }
            }
            expect(raised == std::vector<std::size_t>{5, 6, 7, 8});
            expect(controller.credits() == 8U);
            // At the maximum, further good samples change nothing.
            expect(!controller.update(sample(32, 10.0)).has_value());
            expect(controller.credits() == 8U);
        };

    "server/admission names the reason for the log line"_test = [] {
        expect(to_string(admission_reason::tick_p99) ==
               std::string_view{"tick-p99"});
        expect(to_string(admission_reason::memory) ==
               std::string_view{"memory"});
        expect(to_string(admission_reason::queue_depth) ==
               std::string_view{"queue-depth"});
    };
};

suite<"server/chunk-admission"> admission_priority_tests = [] {
    "server/admission ranks columns by distance then position"_test = [] {
        const std::vector<chunk_pos> wanted{{5, 0}, {1, 0}, {3, 0}, {-2, 0}};
        const std::vector<chunk_pos> centres{{0, 0}};
        const auto ordered{rank_columns_by_distance(wanted, centres)};
        expect(ordered.size() == wanted.size());
        expect(ordered[0] == chunk_pos{1, 0});
        expect(ordered[1] == chunk_pos{-2, 0});
        expect(ordered[2] == chunk_pos{3, 0});
        expect(ordered[3] == chunk_pos{5, 0});
    };

    "server/admission measures to the nearest centre"_test = [] {
        const std::vector<chunk_pos> wanted{{200, 200}, {1, 0}};
        const std::vector<chunk_pos> centres{{100, 100}, {0, 0}};
        const auto ordered{rank_columns_by_distance(wanted, centres)};
        expect(ordered.size() == 2U);
        expect(ordered[0] == chunk_pos{1, 0});
        expect(ordered[1] == chunk_pos{200, 200});
    };

    "server/admission keeps the position order with no centre"_test = [] {
        const std::vector<chunk_pos> wanted{{2, 1}, {-1, 4}, {-1, -3}};
        const auto ordered{rank_columns_by_distance(wanted, {})};
        expect(ordered.size() == 3U);
        expect(ordered[0] == chunk_pos{-1, -3});
        expect(ordered[1] == chunk_pos{-1, 4});
        expect(ordered[2] == chunk_pos{2, 1});
    };

    "server/admission evicts the farthest column from a full queue"_test = [] {
        const std::vector<chunk_pos> wanted{{1, 0}, {-6, 0}, {2, 0}, {3, 0}};
        const std::vector<chunk_pos> centres{{0, 0}};
        expect(farthest_column(wanted, centres) == chunk_pos{-6, 0});
        // A tie on distance falls to the greatest position.
        const std::vector<chunk_pos> tied{{2, 0}, {-2, 0}, {0, 2}};
        expect(farthest_column(tied, centres) == chunk_pos{2, 0});
    };

    "server/admission evicts the greatest position with no centre"_test = [] {
        const std::vector<chunk_pos> wanted{{0, 0}, {1, 0}, {1, 1}};
        expect(farthest_column(wanted, {}) == chunk_pos{1, 1});
    };

    "server/admission handles an empty wanted set"_test = [] {
        const std::vector<chunk_pos> centres{{0, 0}};
        expect(rank_columns_by_distance({}, centres).empty());
        expect(rank_columns_by_distance({}, {}).empty());
    };

    "server/admission keeps every wanted column"_test = [] {
        const std::vector<chunk_pos> wanted{{-3, 2}, {7, -7}, {0, 0}, {1, 1}};
        const std::vector<chunk_pos> centres{{1, 1}, {-2, -2}};
        const auto ordered{rank_columns_by_distance(wanted, centres)};
        expect(ordered.size() == wanted.size());
        expect(ordered[0] == chunk_pos{1, 1});
        expect(ordered[1] == chunk_pos{0, 0});
        expect(ordered[2] == chunk_pos{-3, 2});
        expect(ordered[3] == chunk_pos{7, -7});
    };
};

} // namespace
