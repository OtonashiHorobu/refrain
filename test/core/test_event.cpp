// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/event.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

namespace {

namespace ev = refrain::core::events;

struct ping {
    int value{};
};

struct pong {
    int value{};
};

struct message {
    std::string text{};
};

// Same unqualified name in two scopes: the canonical type name keeps them
// apart, so the tag must differ even though both are called "event".
namespace alpha {
struct event {
    int value{};
};
} // namespace alpha

namespace beta {
struct event {
    int value{};
};
} // namespace beta

} // namespace

// Registered at static-initialization time; test/main.cpp runs the suites. This
// is Boost.UT's namespace-scope entry point: a `suite` variable initialized
// with a lambda, so the `"name"_test = ...` statements live in a function body.
boost::ut::suite event_suite = [] {
    using namespace boost::ut;

    "event/type_of is a stable compile-time label"_test = [] {
        // The tag is a compile-time hash of the compiler-canonical type name
        // (not the address of a per-type static), so it is identical in every
        // translation unit / dynamic library built from the same declaration
        // rather than being unique to one link image.
        static_assert(ev::type_of<ping>() != 0);
        static_assert(ev::type_of<ping>() == ev::type_of<ping>());
        static_assert(ev::type_of<ping>() != ev::type_of<pong>());
        // Aliases of one type share the tag: the compiler prints the canonical
        // type, so the tag does not depend on how a call site spells the type.
        using ping_alias = ping;
        static_assert(ev::type_of<ping_alias>() == ev::type_of<ping>());
        // Different types that share an unqualified name are still distinct.
        static_assert(ev::type_of<alpha::event>() !=
                      ev::type_of<beta::event>());
        static_assert(ev::type_of<alpha::event>() != ev::type_of<ping>());

        constexpr auto baked_in{ev::type_of<ping>()};
        expect(baked_in == ev::type_of<ping>());
        expect(ev::type_of<ping>() != ev::type_of<pong>());
    };

    "event/bus is neither copyable nor movable"_test = [] {
        // Deferred tasks capture `this`; a copied/moved bus would leave
        // closures pointing at the source object (dangling listeners).
        static_assert(!std::is_copy_constructible_v<ev::bus>);
        static_assert(!std::is_copy_assignable_v<ev::bus>);
        static_assert(!std::is_move_constructible_v<ev::bus>);
        static_assert(!std::is_move_assignable_v<ev::bus>);
        expect(true);
    };

    "event/self-unsubscribe during publish keeps later listeners"_test = [] {
        ev::bus bus{};
        auto order{std::string{}};
        ev::subscription_id first_id{};
        first_id = bus.subscribe<ping>([&bus, &order, &first_id](const ping &) {
            order += 'A';
            // Erasing itself must not shift the vector under the cursor
            // and skip the listener at the next index.
            bus.unsubscribe(first_id);
        });
        (void)bus.subscribe<ping>([&order](const ping &) { order += 'B'; });
        (void)bus.subscribe<ping>([&order](const ping &) { order += 'C'; });

        bus.publish(ping{.value = 0});
        expect(order == "ABC");
        expect(bus.subscribers() == std::size_t{2});
        expect(bus.listener_count(ev::type_of<ping>()) == std::size_t{2});
    };

    "event/unsubscribe of a later listener skips only that listener"_test = [] {
        ev::bus bus{};
        auto order{std::string{}};
        ev::subscription_id second_id{};
        (void)bus.subscribe<ping>([&bus, &order, &second_id](const ping &) {
            order += 'A';
            bus.unsubscribe(second_id); // tombstone B mid-publish
        });
        second_id =
            bus.subscribe<ping>([&order](const ping &) { order += 'B'; });
        (void)bus.subscribe<ping>([&order](const ping &) { order += 'C'; });

        bus.publish(ping{.value = 0});
        expect(order == "AC");
        expect(bus.subscribers() == std::size_t{2});
    };

    "event/unsubscribe of an earlier listener keeps later ones"_test = [] {
        ev::bus bus{};
        auto order{std::string{}};
        ev::subscription_id first_id{};
        first_id =
            bus.subscribe<ping>([&order](const ping &) { order += 'A'; });
        (void)bus.subscribe<ping>([&bus, &order, &first_id](const ping &) {
            order += 'B';
            bus.unsubscribe(first_id); // erase before the cursor
        });
        (void)bus.subscribe<ping>([&order](const ping &) { order += 'C'; });

        bus.publish(ping{.value = 0});
        expect(order == "ABC");
        expect(bus.subscribers() == std::size_t{2});
    };

    "event/subscribe during publish does not receive the in-flight event"_test =
        [] {
            ev::bus bus{};
            auto order{std::string{}};
            auto added{false};
            (void)bus.subscribe<ping>([&bus, &order, &added](const ping &) {
                order += 'A';
                if (!added) {
                    added = true;
                    (void)bus.subscribe<ping>(
                        [&order](const ping &) { order += 'D'; });
                }
            });

            bus.publish(ping{.value = 0});
            expect(order == "A");
            expect(added);
            expect(bus.subscribers() == std::size_t{2});

            // The next publish does reach the listener added by the callback.
            order.clear();
            bus.publish(ping{.value = 1});
            expect(order == "AD" || order == "DA");
            expect(order.size() == std::size_t{2});
        };

    "event/clear from a listener is safe and stops delivery"_test = [] {
        ev::bus bus{};
        auto calls{std::size_t{}};
        (void)bus.subscribe<ping>([&bus, &calls](const ping &) {
            ++calls;
            bus.clear();
        });
        (void)bus.subscribe<ping>([&calls](const ping &) { ++calls; });
        (void)bus.subscribe<ping>([&calls](const ping &) { ++calls; });

        bus.publish(ping{.value = 0});
        expect(calls == std::size_t{1});
        expect(bus.subscribers() == std::size_t{0});
        expect(bus.pending() == std::size_t{0});
    };

    "event/removals are purged after a throwing listener"_test = [] {
        ev::bus bus{};
        auto removed_calls{std::size_t{}};
        ev::subscription_id victim{};
        // The throwing listener runs first and tombstones the listener that
        // follows it; the pending exception must still reset the publish depth
        // and purge the tombstone.
        (void)bus.subscribe<ping>([&bus, &victim](const ping &) {
            bus.unsubscribe(victim);
            throw 42;
        });
        victim = bus.subscribe<ping>(
            [&removed_calls](const ping &) { ++removed_calls; });

        bool caught{false};
        try {
            bus.publish(ping{.value = 0});
        } catch (const int &) {
            caught = true;
        }
        expect(caught);
        // The publish depth was reset and the tombstone purged, so the bus is
        // usable again and the removed listener was never invoked.
        expect(bus.subscribers() == std::size_t{1});
        expect(removed_calls == std::size_t{0});

        bus.clear();
        expect(bus.subscribers() == std::size_t{0});
        auto calls{std::size_t{}};
        (void)bus.subscribe<ping>([&calls](const ping &) { ++calls; });
        bus.publish(ping{.value = 1});
        expect(calls == std::size_t{1});
    };

    "event/subscribe returns unique increasing ids"_test = [] {
        ev::bus bus{};
        const auto first{bus.subscribe<ping>([](const ping &) {})};
        const auto second{bus.subscribe<ping>([](const ping &) {})};
        expect(first != second);
        expect(second > first);
        expect(bus.subscribers() == std::size_t{2});
    };

    "event/publish invokes listeners in subscription order"_test = [] {
        ev::bus bus{};
        auto order{std::string{}};
        (void)bus.subscribe<ping>([&order](const ping &event) {
            order += static_cast<char>('1' + event.value);
        });
        (void)bus.subscribe<ping>([&order](const ping &event) {
            order += static_cast<char>('a' + event.value);
        });
        (void)bus.subscribe<ping>([&order](const ping &event) {
            order += static_cast<char>('A' + event.value);
        });
        bus.publish(ping{.value = 0});
        expect(order == "1aA");
    };

    "event/different types do not interfere"_test = [] {
        ev::bus bus{};
        auto pings{std::size_t{}};
        auto pongs{std::size_t{}};
        (void)bus.subscribe<ping>([&pings](const ping &) { ++pings; });
        (void)bus.subscribe<pong>([&pongs](const pong &) { ++pongs; });

        bus.publish(ping{.value = 1});
        expect(pings == std::size_t{1});
        expect(pongs == std::size_t{0});

        bus.publish(pong{.value = 2});
        expect(pings == std::size_t{1});
        expect(pongs == std::size_t{1});

        expect(bus.listener_count(ev::type_of<ping>()) == std::size_t{1});
        expect(bus.listener_count(ev::type_of<pong>()) == std::size_t{1});
        expect(bus.listener_count(ev::type_of<message>()) == std::size_t{0});
        expect(bus.subscribers() == std::size_t{2});
    };

    "event/unsubscribe removes one listener and ignores unknown ids"_test = [] {
        ev::bus bus{};
        auto pings{std::size_t{}};
        const auto kept{
            bus.subscribe<ping>([&pings](const ping &) { ++pings; })};
        const auto removed{
            bus.subscribe<ping>([&pings](const ping &) { pings += 10; })};

        bus.unsubscribe(removed);
        expect(bus.subscribers() == std::size_t{1});
        bus.publish(ping{.value = 0});
        expect(pings == std::size_t{1});

        bus.unsubscribe(removed);         // already removed: no-op
        bus.unsubscribe(removed + 1000U); // never subscribed: no-op
        expect(bus.subscribers() == std::size_t{1});
        expect(bus.listener_count(ev::type_of<ping>()) == std::size_t{1});

        bus.unsubscribe(kept);
        expect(bus.subscribers() == std::size_t{0});
        expect(bus.listener_count(ev::type_of<ping>()) == std::size_t{0});
    };

    "event/post defers delivery until dispatch"_test = [] {
        ev::bus bus{};
        auto calls{std::size_t{}};
        (void)bus.subscribe<ping>([&calls](const ping &) { ++calls; });

        bus.post(ping{.value = 7});
        expect(calls == std::size_t{0});
        expect(bus.pending() == std::size_t{1});

        expect(bus.dispatch() == std::size_t{1});
        expect(calls == std::size_t{1});
        expect(bus.pending() == std::size_t{0});
        expect(bus.dispatch() == std::size_t{0});
    };

    "event/dispatch drains in post order"_test = [] {
        ev::bus bus{};
        auto order{std::string{}};
        (void)bus.subscribe<ping>([&order](const ping &event) {
            order += static_cast<char>('0' + event.value);
        });

        bus.post(ping{.value = 1});
        bus.post(ping{.value = 2});
        bus.post(ping{.value = 3});
        expect(bus.pending() == std::size_t{3});

        expect(bus.dispatch() == std::size_t{3});
        expect(order == "123");
        expect(bus.pending() == std::size_t{0});
    };

    "event/post from a listener is dispatched in the same call"_test = [] {
        ev::bus bus{};
        auto order{std::string{}};
        (void)bus.subscribe<ping>([&bus, &order](const ping &event) {
            order += static_cast<char>('0' + event.value);
            if (event.value == 1) {
                bus.post(ping{.value = 2});
            }
        });

        bus.post(ping{.value = 1});
        expect(bus.dispatch() == std::size_t{2});
        expect(order == "12");
        expect(bus.pending() == std::size_t{0});
    };

    "event/post from a listener keeps FIFO order"_test = [] {
        ev::bus bus{};
        auto order{std::string{}};
        (void)bus.subscribe<ping>([&bus, &order](const ping &event) {
            order += static_cast<char>('0' + event.value);
            if (event.value == 1) {
                bus.post(ping{.value = 9});
            }
        });

        bus.post(ping{.value = 1});
        bus.post(ping{.value = 3});
        expect(bus.dispatch() == std::size_t{3});
        expect(order == "139");
    };

    // A listener that re-posts the same event must not keep dispatch() running
    // forever: one call publishes at most `max_events_per_dispatch` events and
    // leaves the rest queued for the following call.
    "event/dispatch respects the per-call event budget"_test = [] {
        ev::bus bus{};
        auto calls{std::size_t{}};
        (void)bus.subscribe<ping>([&bus, &calls](const ping &event) {
            ++calls;
            bus.post(ping{.value = event.value + 1});
        });
        bus.set_max_events_per_dispatch(3);
        bus.post(ping{.value = 0});

        expect(bus.dispatch() == std::size_t{3});
        expect(calls == std::size_t{3});
        expect(bus.pending() > std::size_t{0});

        // The remainder is drained by the next call, not dropped.
        expect(bus.dispatch() == std::size_t{3});
        expect(calls == std::size_t{6});

        // 0 means unlimited.
        bus.clear(); // removes the self-posting listener, drops pending events
        expect(bus.pending() == std::size_t{0});
        bus.set_max_events_per_dispatch(0);
        for (int value = 0; value < 5; ++value) {
            bus.post(ping{.value = value});
        }
        expect(bus.dispatch() == std::size_t{5});
        expect(bus.pending() == std::size_t{0});
    };

    "event/post stores the event by value"_test = [] {
        ev::bus bus{};
        auto seen{std::string{}};
        (void)bus.subscribe<message>(
            [&seen](const message &event) { seen = event.text; });

        {
            auto outgoing{message{}};
            outgoing.text = "hello";
            bus.post(outgoing);
        } // outgoing is destroyed here: the queued copy must stay valid

        expect(bus.dispatch() == std::size_t{1});
        expect(seen == "hello");
    };

    "event/clear drops listeners and pending events"_test = [] {
        ev::bus bus{};
        auto calls{std::size_t{}};
        (void)bus.subscribe<ping>([&calls](const ping &) { ++calls; });
        bus.post(ping{.value = 1});

        bus.clear();
        expect(bus.subscribers() == std::size_t{0});
        expect(bus.pending() == std::size_t{0});
        expect(bus.dispatch() == std::size_t{0});

        bus.publish(ping{.value = 2});
        expect(calls == std::size_t{0});
        expect(bus.listener_count(ev::type_of<ping>()) == std::size_t{0});
    };

    "event/raw subscribe takes an untyped listener"_test = [] {
        ev::bus bus{};
        auto seen{0};
        const auto id{
            bus.subscribe(ev::type_of<ping>(), [&seen](const void *payload) {
                seen = static_cast<const ping *>(payload)->value;
            })};
        expect(id != ev::subscription_id{0});
        expect(bus.listener_count(ev::type_of<ping>()) == std::size_t{1});

        bus.publish(ping{.value = 42});
        expect(seen == 42);

        bus.unsubscribe(id);
        expect(bus.subscribers() == std::size_t{0});
    };

    "event/priority orders high before normal before low"_test = [] {
        ev::bus bus{};
        std::vector<int> seen{};
        (void)bus.subscribe<ping>(ev::event_priority::low,
                                  [&seen](const ping &) { seen.push_back(1); });
        (void)bus.subscribe<ping>(ev::event_priority::high,
                                  [&seen](const ping &) { seen.push_back(2); });
        (void)bus.subscribe<ping>(ev::event_priority::normal,
                                  [&seen](const ping &) { seen.push_back(3); });
        (void)bus.subscribe<ping>(ev::event_priority::high,
                                  [&seen](const ping &) { seen.push_back(4); });

        bus.publish(ping{.value = 7});
        expect(seen == std::vector<int>{2, 4, 3, 1});
    };
};
