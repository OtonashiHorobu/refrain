// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/inventory_sync.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/vanilla.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/fake_play_gateway.hpp"

using namespace boost::ut;
using namespace refrain::server;

namespace {

namespace data = refrain::data;
namespace net = refrain::net;
namespace session = refrain::session;
namespace sim = refrain::sim;
namespace support = refrain::test_support;

[[nodiscard]] auto item_protocol_id(const data::vanilla_tables &tables,
                                    std::string_view name) -> std::uint32_t {
    const auto registry{tables.find_registry("minecraft:item")};
    if (!registry) {
        return 0;
    }
    const auto count{tables.registry_entry_count(*registry)};
    for (std::uint32_t index{0}; index < count; ++index) {
        const auto entry{tables.registry_entry(*registry, index)};
        if (entry.name == name && entry.protocol_id >= 0) {
            return static_cast<std::uint32_t>(entry.protocol_id);
        }
    }
    return 0;
}

void join(session::play_session &players) {
    static_cast<void>(players.add_player(1, net::uuid{}, "player", "world", 0.0,
                                         64.0, 0.0, 1));
}

} // namespace

suite<"server/inventory-sync"> inventory_sync_tests = [] {
    "the first tick sends the full menu for the active connection"_test = [] {
        auto bytes{
            refrain::core::io::read_file(std::string{REFRAIN_SOURCE_DIR} +
                                         "/content/vanilla/26.3/tables.rvtb")};
        if (!bytes) {
            expect(false) << "tables.rvtb is missing";
            return;
        }
        auto opened{data::vanilla_tables::open(*bytes)};
        if (!opened) {
            expect(false) << "tables.rvtb is invalid";
            return;
        }
        const auto stone{item_protocol_id(*opened, "minecraft:stone")};
        expect(stone != 0U) << "stone must have a protocol id";

        sim::player_table table{};
        session::play_session players{table};
        join(players);
        players.inventory_of(1)->at(0) = sim::item_stack{
            .item = "minecraft:stone",
            .count = 5,
        };
        support::fake_play_gateway gateway{};
        gateway.active = {1};
        inventory_sync sync{gateway, &*opened};

        sync.tick(players);

        expect(gateway.container_content.size() == 1U);
        expect(gateway.container_content[0].connection == 1U);
        expect(gateway.container_content[0].container_id == 0);
        expect(gateway.container_content[0].items.size() == 46U);
        expect(gateway.container_content[0].items[36].item_id == stone);
        expect(gateway.container_content[0].items[36].count == 5U);
        expect(gateway.container_content[0].state_id == 1);
    };

    "a changed stack sends one menu slot update"_test = [] {
        auto bytes{
            refrain::core::io::read_file(std::string{REFRAIN_SOURCE_DIR} +
                                         "/content/vanilla/26.3/tables.rvtb")};
        if (!bytes) {
            expect(false) << "tables.rvtb is missing";
            return;
        }
        auto opened{data::vanilla_tables::open(*bytes)};
        if (!opened) {
            expect(false) << "tables.rvtb is invalid";
            return;
        }
        const auto stone{item_protocol_id(*opened, "minecraft:stone")};
        expect(stone != 0U);

        sim::player_table table{};
        session::play_session players{table};
        join(players);
        auto *inventory{players.inventory_of(1)};
        inventory->at(0) = sim::item_stack{
            .item = "minecraft:stone",
            .count = 5,
        };
        support::fake_play_gateway gateway{};
        gateway.active = {1};
        inventory_sync sync{gateway, &*opened};
        sync.tick(players);

        gateway.container_content.clear();
        inventory->at(0).count = 3;
        sync.tick(players);

        expect(gateway.container_slots.size() == 1U);
        expect(gateway.container_slots[0].slot == 36);
        expect(gateway.container_slots[0].item.item_id == stone);
        expect(gateway.container_slots[0].item.count == 3U);
        expect(gateway.container_slots[0].state_id == 2);
    };

    "a left click moves the stack to the cursor and empties the slot"_test =
        [] {
            auto bytes{refrain::core::io::read_file(
                std::string{REFRAIN_SOURCE_DIR} +
                "/content/vanilla/26.3/tables.rvtb")};
            if (!bytes) {
                expect(false) << "tables.rvtb is missing";
                return;
            }
            auto opened{data::vanilla_tables::open(*bytes)};
            if (!opened) {
                expect(false) << "tables.rvtb is invalid";
                return;
            }
            const auto stone{item_protocol_id(*opened, "minecraft:stone")};
            expect(stone != 0U);

            sim::player_table table{};
            session::play_session players{table};
            join(players);
            auto *inventory{players.inventory_of(1)};
            inventory->at(0) = sim::item_stack{
                .item = "minecraft:stone",
                .count = 5,
            };
            support::fake_play_gateway gateway{};
            gateway.active = {1};
            inventory_sync sync{gateway, &*opened};
            sync.tick(players);

            gateway.container_slots.clear();
            gateway.cursor_items.clear();
            gateway.container_clicks.push_back(net::container_click_event{
                .connection = 1,
                .container_id = 0,
                .state_id = 1,
                .slot = 36,
                .button = 0,
                .mode = 0,
            });
            sync.tick(players);

            expect(inventory->at(0).count == 0U);
            expect(gateway.container_slots.size() == 1U);
            expect(gateway.container_slots[0].slot == 36);
            expect(gateway.container_slots[0].item.count == 0U);
            expect(gateway.cursor_items.size() == 1U);
            expect(gateway.cursor_items[0].item.item_id == stone);
            expect(gateway.cursor_items[0].item.count == 5U);
        };

    "a creative slot overwrites the inventory and resends the menu"_test = [] {
        auto bytes{
            refrain::core::io::read_file(std::string{REFRAIN_SOURCE_DIR} +
                                         "/content/vanilla/26.3/tables.rvtb")};
        if (!bytes) {
            expect(false) << "tables.rvtb is missing";
            return;
        }
        auto opened{data::vanilla_tables::open(*bytes)};
        if (!opened) {
            expect(false) << "tables.rvtb is invalid";
            return;
        }
        const auto stone{item_protocol_id(*opened, "minecraft:stone")};
        expect(stone != 0U);

        sim::player_table table{};
        session::play_session players{table};
        join(players);
        support::fake_play_gateway gateway{};
        gateway.active = {1};
        inventory_sync sync{gateway, &*opened};
        sync.tick(players);

        gateway.container_content.clear();
        gateway.creative_slots.push_back(net::creative_slot_event{
            .connection = 1,
            .slot = 0,
            .item_id = stone,
            .count = 7,
        });
        sync.tick(players);

        expect(players.inventory_of(1)->at(0).item == "minecraft:stone");
        expect(players.inventory_of(1)->at(0).count == 7U);
        expect(gateway.container_content.size() == 1U);
        expect(gateway.container_content[0].items[36].count == 7U);
    };

    "an unavailable gateway sends nothing and keeps no state"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        join(players);
        support::fake_play_gateway gateway{false};
        gateway.active = {1};
        inventory_sync sync{gateway, nullptr};

        sync.tick(players);

        expect(gateway.container_content.empty());
        expect(gateway.container_slots.empty());
        expect(gateway.cursor_items.empty());
    };
};
