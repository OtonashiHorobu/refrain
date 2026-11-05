// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/schedule.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace sched = refrain::core::schedule;

/// Minimal provider context: counts invocations, remembers the tick, appends a
/// tag to an optional order log, and can return a chosen failure.
struct fixture {
    int calls{0};
    std::int64_t seen_tick{-1};
    const void *seen_vtable{nullptr};
    bool fail{false};
    std::string message{"scheduled task failed"};
    std::string *order{nullptr};
    char tag{'?'};
};

auto probe_invoker(void *self, const void *vtable, std::int64_t tick) noexcept
    -> refrain::status {
    auto &state{*static_cast<fixture *>(self)};
    ++state.calls;
    state.seen_tick = tick;
    state.seen_vtable = vtable;
    if (state.order != nullptr) {
        state.order->push_back(state.tag);
    }
    if (state.fail) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::unknown, std::string_view{state.message})};
    }
    return {};
}

auto make_task(std::string_view service, std::string_view method,
               std::int64_t interval, sched::phase when, sched::invoker invoke)
    -> sched::task_descriptor {
    return sched::task_descriptor{
        .service = service,
        .method = method,
        .interval_ticks = interval,
        .phase_of = when,
        .invoke = invoke,
    };
}

auto add_one(sched::scheduler &owner, const sched::task_descriptor &task,
             void *self, const void *vtable = nullptr) -> refrain::status {
    return owner.add(std::span<const sched::task_descriptor>{&task, 1}, self,
                     vtable);
}

} // namespace

boost::ut::suite schedule_suite = [] {
    using namespace boost::ut;

    "schedule/first run happens at the interval, not before"_test = [] {
        sched::scheduler owner{};
        fixture state{};
        const auto vtable_token{0};

        const auto task{make_task("svc", "periodic", 10,
                                  sched::phase::post_tick, &probe_invoker)};
        expect(add_one(owner, task, &state, &vtable_token).has_value());

        expect(!owner.due(sched::phase::post_tick, 9));
        expect(owner.run(sched::phase::post_tick, 9) == std::size_t{0});
        expect(state.calls == 0);

        expect(owner.due(sched::phase::post_tick, 10));
        expect(owner.run(sched::phase::post_tick, 10) == std::size_t{1});
        expect(state.calls == 1);
        expect(state.seen_tick == 10);
        expect(state.seen_vtable == &vtable_token);
        expect(!owner.due(sched::phase::post_tick, 10));
        expect(owner.last_tick() == 10);
    };

    "schedule/big tick jump coalesces onto the fixed grid"_test = [] {
        sched::scheduler owner{};
        fixture state{};

        const auto task{make_task("svc", "periodic", 10,
                                  sched::phase::post_tick, &probe_invoker)};
        expect(add_one(owner, task, &state).has_value());

        // next_due = 10; tick 95 passes slots 10..90 (9 slots), so one run.
        expect(owner.run(sched::phase::post_tick, 95) == std::size_t{1});
        expect(state.calls == 1);
        expect(state.seen_tick == 95);

        const auto snapshot{owner.tasks()};
        expect(snapshot.size() == std::size_t{1});
        expect(snapshot[0].stats.runs == 1);
        expect(snapshot[0].stats.coalesced == 8);
        expect(snapshot[0].stats.last_tick == 95);

        // The next slot is 100, not 105: the grid was restored, not shifted.
        expect(owner.run(sched::phase::post_tick, 99) == std::size_t{0});
        expect(state.calls == 1);
        expect(owner.run(sched::phase::post_tick, 100) == std::size_t{1});
        expect(state.calls == 2);
        expect(owner.tasks()[0].stats.coalesced == 8);

        // A jump after a normal run advances from the current grid position.
        expect(owner.run(sched::phase::post_tick, 135) == std::size_t{1});
        expect(state.calls == 3);
        expect(owner.tasks()[0].stats.coalesced == 10);
        expect(owner.tasks()[0].stats.last_tick == 135);
        expect(owner.run(sched::phase::post_tick, 140) == std::size_t{1});
    };

    "schedule/registration order wins across services and phases"_test = [] {
        sched::scheduler owner{};
        auto order{std::string{}};
        fixture late_state{.order = &order, .tag = '1'};
        fixture early_state{.order = &order, .tag = '2'};
        fixture pre_state{.order = &order, .tag = 'R'};

        const auto late{make_task("zeta", "late", 10, sched::phase::post_tick,
                                  &probe_invoker)};
        const auto early{make_task("alpha", "early", 10,
                                   sched::phase::post_tick, &probe_invoker)};
        const auto before{make_task("alpha", "before", 10,
                                    sched::phase::pre_tick, &probe_invoker)};
        expect(add_one(owner, late, &late_state).has_value());
        expect(add_one(owner, early, &early_state).has_value());
        expect(add_one(owner, before, &pre_state).has_value());

        // Phases are independent: pre_tick work never runs from post_tick.
        expect(owner.run(sched::phase::pre_tick, 10) == std::size_t{1});
        expect(order == "R");
        expect(owner.run(sched::phase::post_tick, 10) == std::size_t{2});
        expect(order == "R12");
        expect(owner.run(sched::phase::post_tick, 10) == std::size_t{0});
        expect(order == "R12");

        // tasks() mirrors the same registration order.
        const auto snapshot{owner.tasks()};
        expect(snapshot.size() == std::size_t{3});
        expect(snapshot[0].service == "zeta");
        expect(snapshot[1].service == "alpha");
        expect(snapshot[2].method == "before");
    };

    "schedule/add rejects duplicate (service, method) atomically"_test = [] {
        sched::scheduler owner{};
        fixture original{};
        fixture duplicate{};

        const auto task{make_task("svc", "tick", 10, sched::phase::post_tick,
                                  &probe_invoker)};
        expect(add_one(owner, task, &original).has_value());

        const auto conflicting{make_task(
            "svc", "tick", 99, sched::phase::pre_tick, &probe_invoker)};
        const auto rejected{add_one(owner, conflicting, &duplicate)};
        expect(!rejected.has_value());
        expect(rejected.error().code == refrain::error_code::invalid_argument);
        expect(owner.size() == std::size_t{1});

        const auto snapshot{owner.tasks()};
        expect(snapshot.size() == std::size_t{1});
        expect(snapshot[0].interval_ticks == 10);
        expect(snapshot[0].phase_of == sched::phase::post_tick);
        expect(duplicate.calls == 0);

        // A duplicate inside one batch is rejected before anything is added.
        const std::array<sched::task_descriptor, 2> batch{
            make_task("fresh", "one", 5, sched::phase::post_tick,
                      &probe_invoker),
            make_task("fresh", "one", 5, sched::phase::post_tick,
                      &probe_invoker),
        };
        const auto batch_rejected{owner.add(batch, &duplicate, nullptr)};
        expect(!batch_rejected.has_value());
        expect(batch_rejected.error().code ==
               refrain::error_code::invalid_argument);
        expect(owner.size() == std::size_t{1});
    };

    "schedule/add rejects null invoker, bad intervals and empty names"_test =
        [] {
            sched::scheduler owner{};
            fixture state{};

            const auto valid{make_task(
                "svc", "tick", 10, sched::phase::post_tick, &probe_invoker)};
            const auto no_invoker{
                make_task("svc", "null", 10, sched::phase::post_tick, nullptr)};
            const auto zero_interval{make_task(
                "svc", "zero", 0, sched::phase::post_tick, &probe_invoker)};
            const auto negative_interval{make_task("svc", "negative", -3,
                                                   sched::phase::post_tick,
                                                   &probe_invoker)};
            const auto too_large{
                make_task("svc", "large", sched::max_interval_ticks + 1,
                          sched::phase::post_tick, &probe_invoker)};
            const auto empty_service{make_task(
                "", "method", 10, sched::phase::post_tick, &probe_invoker)};
            const auto empty_method{make_task(
                "svc", "", 10, sched::phase::post_tick, &probe_invoker)};

            const auto null_invoker_result{add_one(owner, no_invoker, &state)};
            expect(!null_invoker_result.has_value());
            expect(null_invoker_result.error().code ==
                   refrain::error_code::invalid_argument);
            expect(!add_one(owner, zero_interval, &state).has_value());
            expect(!add_one(owner, negative_interval, &state).has_value());
            expect(!add_one(owner, too_large, &state).has_value());
            expect(!add_one(owner, empty_service, &state).has_value());
            expect(!add_one(owner, empty_method, &state).has_value());
            expect(owner.empty());

            // A null self has no provider to invoke.
            const auto null_self{
                owner.add(std::span<const sched::task_descriptor>{&valid, 1},
                          nullptr, nullptr)};
            expect(!null_self.has_value());
            expect(null_self.error().code ==
                   refrain::error_code::invalid_argument);
            expect(owner.empty());

            // A null vtable is legal: host-owned maintenance tasks pass a
            // plain context pointer with no provider vtable.
            expect(add_one(owner, valid, &state).has_value());
            expect(owner.size() == std::size_t{1});
            expect(owner.run(sched::phase::post_tick, 10) == std::size_t{1});
            expect(state.calls == 1);
            expect(state.seen_vtable == nullptr);
        };

    "schedule/add is all-or-nothing for a mixed batch"_test = [] {
        sched::scheduler owner{};
        fixture state{};

        const std::array<sched::task_descriptor, 2> batch{
            make_task("svc", "good", 10, sched::phase::post_tick,
                      &probe_invoker),
            make_task("svc", "bad", 10, sched::phase::post_tick, nullptr),
        };
        const auto rejected{owner.add(batch, &state, nullptr)};
        expect(!rejected.has_value());
        expect(rejected.error().code == refrain::error_code::invalid_argument);
        expect(owner.empty());
        expect(state.calls == 0);

        // The scheduler is still usable after a rejected batch.
        const auto task{make_task("svc", "good", 10, sched::phase::post_tick,
                                  &probe_invoker)};
        expect(add_one(owner, task, &state).has_value());
        expect(owner.run(sched::phase::post_tick, 10) == std::size_t{1});
        expect(state.calls == 1);
    };

    "schedule/failure is counted and later tasks still run"_test = [] {
        sched::scheduler owner{};
        fixture before{};
        fixture failing{};
        fixture after{};
        failing.fail = true;
        failing.message = std::string(400, 'x');

        const auto before_task{make_task(
            "svc", "before", 10, sched::phase::post_tick, &probe_invoker)};
        const auto failing_task{make_task(
            "svc", "failing", 10, sched::phase::post_tick, &probe_invoker)};
        const auto after_task{make_task(
            "svc", "after", 10, sched::phase::post_tick, &probe_invoker)};
        expect(add_one(owner, before_task, &before).has_value());
        expect(add_one(owner, failing_task, &failing).has_value());
        expect(add_one(owner, after_task, &after).has_value());

        expect(owner.run(sched::phase::post_tick, 10) == std::size_t{3});
        expect(before.calls == 1);
        expect(failing.calls == 1);
        expect(after.calls == 1);

        const auto snapshot{owner.tasks()};
        expect(snapshot.size() == std::size_t{3});
        expect(snapshot[0].stats.failures == 0);
        expect(snapshot[1].stats.failures == 1);
        expect(snapshot[1].stats.runs == 1);
        expect(snapshot[1].stats.last_error.size() == std::size_t{256});
        expect(snapshot[1].stats.last_error == std::string(256, 'x'));
        expect(snapshot[2].stats.failures == 0);

        // The failure is not sticky: the task runs (and fails) on the grid.
        expect(owner.run(sched::phase::post_tick, 20) == std::size_t{3});
        expect(owner.tasks()[1].stats.failures == 2);
        expect(owner.tasks()[1].stats.last_tick == 20);

        // Short messages are stored verbatim, without padding.
        sched::scheduler short_owner{};
        fixture short_failure{};
        short_failure.fail = true;
        short_failure.message = "boom";
        const auto short_task{make_task(
            "svc", "short", 1, sched::phase::post_tick, &probe_invoker)};
        expect(add_one(short_owner, short_task, &short_failure).has_value());
        expect(short_owner.run(sched::phase::post_tick, 1) == std::size_t{1});
        expect(short_owner.tasks()[0].stats.last_error == "boom");
    };

    "schedule/disabled tasks keep their grid and never replay"_test = [] {
        sched::scheduler owner{};
        fixture state{};

        const auto task{make_task("svc", "tick", 5, sched::phase::post_tick,
                                  &probe_invoker)};
        expect(add_one(owner, task, &state).has_value());
        expect(owner.run(sched::phase::post_tick, 5) == std::size_t{1});
        expect(state.calls == 1);

        expect(owner.set_enabled("svc", "tick", false));
        expect(!owner.due(sched::phase::post_tick, 10));
        expect(!owner.due(sched::phase::post_tick, 100));
        expect(owner.run(sched::phase::post_tick, 10) == std::size_t{0});
        expect(owner.run(sched::phase::post_tick, 15) == std::size_t{0});
        expect(state.calls == 1);

        expect(!owner.set_enabled("svc", "other", true));
        expect(!owner.set_enabled("other", "tick", true));

        expect(owner.set_enabled("svc", "tick", true));
        expect(owner.due(sched::phase::post_tick, 10));
        // Coalesce every slot skipped while disabled into one run.
        expect(owner.run(sched::phase::post_tick, 16) == std::size_t{1});
        expect(state.calls == 2);
        expect(!owner.due(sched::phase::post_tick, 16));
        expect(owner.run(sched::phase::post_tick, 20) == std::size_t{1});
        expect(state.calls == 3);

        const auto snapshot{owner.tasks()};
        expect(snapshot[0].enabled);
        expect(snapshot[0].stats.runs == 3);
        expect(snapshot[0].stats.coalesced == 1);
    };

    "schedule/remove_service removes exactly that service"_test = [] {
        sched::scheduler owner{};
        fixture alpha_a{};
        fixture alpha_b{};
        fixture beta{};

        expect(add_one(owner,
                       make_task("alpha", "a", 10, sched::phase::post_tick,
                                 &probe_invoker),
                       &alpha_a)
                   .has_value());
        expect(add_one(owner,
                       make_task("alpha", "b", 10, sched::phase::post_tick,
                                 &probe_invoker),
                       &alpha_b)
                   .has_value());
        expect(add_one(owner,
                       make_task("beta", "only", 10, sched::phase::post_tick,
                                 &probe_invoker),
                       &beta)
                   .has_value());
        expect(owner.size() == std::size_t{3});

        expect(owner.remove_service("alpha") == std::size_t{2});
        expect(owner.size() == std::size_t{1});
        const auto snapshot{owner.tasks()};
        expect(snapshot.size() == std::size_t{1});
        expect(snapshot[0].service == "beta");
        expect(snapshot[0].method == "only");

        expect(owner.run(sched::phase::post_tick, 10) == std::size_t{1});
        expect(beta.calls == 1);
        expect(alpha_a.calls == 0);
        expect(alpha_b.calls == 0);

        expect(owner.remove_service("alpha") == std::size_t{0});
        expect(owner.remove_service("beta") == std::size_t{1});
        expect(owner.empty());

        // A view borrowed from tasks() may point into the entry being removed;
        // the name is owned before erasing so the comparison stays valid.
        sched::scheduler borrowed_owner{};
        fixture borrowed_state{};
        expect(add_one(borrowed_owner,
                       make_task("borrowed", "tick", 10,
                                 sched::phase::post_tick, &probe_invoker),
                       &borrowed_state)
                   .has_value());
        const auto borrowed_snapshot{borrowed_owner.tasks()};
        expect(borrowed_owner.remove_service(borrowed_snapshot[0].service) ==
               std::size_t{1});
        expect(borrowed_owner.empty());
    };

    "schedule/due, size, last_tick and clear track scheduler state"_test = [] {
        sched::scheduler owner{};
        expect(owner.empty());
        expect(owner.size() == std::size_t{0});
        expect(owner.last_tick() == -1);
        expect(!owner.due(sched::phase::pre_tick, 0));

        fixture state{};
        const auto task{
            make_task("svc", "pre", 4, sched::phase::pre_tick, &probe_invoker)};
        expect(add_one(owner, task, &state).has_value());
        expect(!owner.empty());
        expect(owner.size() == std::size_t{1});
        expect(!owner.due(sched::phase::pre_tick, 3));
        expect(owner.due(sched::phase::pre_tick, 4));
        expect(!owner.due(sched::phase::post_tick, 4));

        // A run that invokes nothing still records the tick it handled.
        expect(owner.run(sched::phase::post_tick, 4) == std::size_t{0});
        expect(owner.last_tick() == 4);
        expect(owner.run(sched::phase::pre_tick, 4) == std::size_t{1});
        expect(state.calls == 1);
        expect(!owner.due(sched::phase::pre_tick, 4));
        expect(owner.due(sched::phase::pre_tick, 8));

        owner.clear();
        expect(owner.empty());
        expect(owner.size() == std::size_t{0});
        expect(owner.last_tick() == -1);
        expect(owner.tasks().empty());

        // Clearing restarts the grid at tick 0.
        expect(add_one(owner, task, &state).has_value());
        expect(owner.run(sched::phase::pre_tick, 4) == std::size_t{1});
        expect(state.calls == 2);
    };

    "schedule/negative ticks are ignored"_test = [] {
        sched::scheduler owner{};
        fixture state{};

        const auto task{make_task("svc", "tick", 1, sched::phase::post_tick,
                                  &probe_invoker)};
        expect(add_one(owner, task, &state).has_value());

        expect(owner.run(sched::phase::post_tick, -1) == std::size_t{0});
        expect(state.calls == 0);
        expect(owner.last_tick() == -1);

        expect(owner.run(sched::phase::post_tick, 1) == std::size_t{1});
        expect(owner.last_tick() == 1);
        expect(owner.run(sched::phase::post_tick, -5) == std::size_t{0});
        expect(state.calls == 1);
        expect(owner.last_tick() == 1);
        expect(owner.tasks()[0].stats.runs == 1);
        expect(owner.tasks()[0].stats.last_tick == 1);
    };

    "schedule/add accepts an empty task table (N = 0 generator output)"_test =
        [] {
            sched::scheduler owner{};
            fixture state{};

            expect(owner
                       .add(std::span<const sched::task_descriptor>{}, &state,
                            nullptr)
                       .has_value());
            expect(owner.empty());
            expect(owner.size() == std::size_t{0});
        };

    "schedule/phase_name labels both tick phases"_test = [] {
        expect(sched::phase_name(sched::phase::pre_tick) == "pre_tick");
        expect(sched::phase_name(sched::phase::post_tick) == "post_tick");
    };
};
