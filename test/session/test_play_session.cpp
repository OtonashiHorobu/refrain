// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/play_session.hpp>

#include <boost/ut.hpp>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace session = refrain::session;
namespace sim = refrain::sim;

suite play_session_tests = [] {
    "session/player table assigns entity ids and visibility"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        const auto first{
            players.add_player(1, {}, "Alice", "overworld", 0.0, 0.0, 0.0, 1)};
        expect(first.has_value());
        const auto second{
            players.add_player(2, {}, "Bob", "overworld", 4.0, 0.0, 0.0, 1)};
        expect(second.has_value());
        expect(*first != *second);
        expect(players.size() == 2U);
        expect(players.player_of(1) != nullptr);
        expect(players.player_of(1)->entity_id == *first);

        const auto visible{players.visible_to(1, 8.0)};
        expect(visible.size() == 1U);
        if (!visible.empty()) {
            expect(visible.front().connection == 2U);
            expect(visible.front().entity_id == *second);
        }
        expect(players.visible_to(1, 1.0).empty());

        expect(players.update_position(2, 100.0, 0.0, 0.0, 0.0F, 0.0F));
        expect(players.visible_to(1, 8.0).empty());
        expect(players.set_health(2, 7.0F));
        expect(players.player_of(2)->health == 7.0F);
    };

    "session/inventories are owned per player"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        expect(players.add_player(1, {}, "Alice", "overworld", 0.0, 0.0, 0.0, 1)
                   .has_value());
        expect(players.add_player(2, {}, "Bob", "overworld", 0.0, 0.0, 0.0, 1)
                   .has_value());
        auto *alice{players.inventory_of(1)};
        auto *bob{players.inventory_of(2)};
        expect(alice != nullptr);
        expect(bob != nullptr);
        if (alice == nullptr || bob == nullptr) {
            return;
        }
        alice->push_back({.item = "minecraft:stone", .count = 4});
        alice->push_back({.item = "minecraft:dirt", .count = 2});
        expect(std::all_of(bob->begin(), bob->end(),
                           [](const auto &stack) { return stack.count == 0; }));
        expect(players.player_of(1)->inventory.size() == 38U);
        expect(players.player_of(2)->inventory.size() == 36U);
        expect(players.inventory_of(99) == nullptr);
    };

    "session/the synthetic player stays out of connection views"_test = [] {
        sim::player_table table{};
        expect(table
                   .add(sim::player_table::registration{
                       .connection = sim::synthetic_connection,
                       .name = "@console",
                       .world = "overworld",
                   })
                   .has_value());
        expect(table
                   .add(sim::player_table::registration{
                       .connection = 1,
                       .name = "Alice",
                       .world = "overworld",
                   })
                   .has_value());
        session::play_session players{table};
        expect(players.size() == 1U);
        expect(players.player_of(sim::synthetic_connection) == nullptr);
        expect(players.player_by_entity(1) == nullptr);
        expect(players.player_by_entity(2) != nullptr);
        expect(players.inventory_of(sim::synthetic_connection) == nullptr);
        const auto visible{players.all()};
        expect(visible.size() == 1U);
        if (!visible.empty()) {
            expect(visible.front().name == "Alice");
        }
        expect(players.update_position(sim::synthetic_connection, 1.0, 2.0, 3.0,
                                       0.0F, 0.0F) == false);
    };

    "session/player removal releases the record"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        expect(players.add_player(3, {}, "Carol", "overworld", 0.0, 0.0, 0.0, 1)
                   .has_value());
        expect(players.remove_player(3));
        expect(!players.remove_player(3));
        expect(players.player_of(3) == nullptr);
        expect(players.size() == 0U);
    };
};

} // namespace
