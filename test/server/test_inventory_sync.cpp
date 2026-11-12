// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/inventory_sync.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/sim/item.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
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

[[nodiscard]] auto component_protocol_id(const data::vanilla_tables &tables,
                                         std::string_view name)
    -> std::uint32_t {
    const auto registry{tables.find_registry("minecraft:data_component_type")};
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

[[nodiscard]] auto round_trip(const net::item_stack &value) -> net::item_stack {
    std::vector<std::byte> bytes;
    net::writer out{bytes};
    expect(static_cast<bool>(net::encode_item_stack(out, value)));
    net::reader in{bytes};
    net::item_stack decoded{};
    expect(static_cast<bool>(net::decode_item_stack(in, decoded)));
    return decoded;
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

    "a damaged enchanted tool survives a menu click and the wire"_test = [] {
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
        auto catalog{net::registry_catalog::load(
            std::string{REFRAIN_SOURCE_DIR} +
            "/content/vanilla/26.3/protocol_registry.json")};
        if (!catalog) {
            expect(false) << "protocol_registry.json is invalid";
            return;
        }
        const auto sword{item_protocol_id(*opened, "minecraft:diamond_sword")};
        const auto damage_id{
            component_protocol_id(*opened, "minecraft:damage")};
        const auto enchantments_id{
            component_protocol_id(*opened, "minecraft:enchantments")};
        expect(sword != 0U);
        expect(damage_id != 0U);
        expect(enchantments_id != 0U);

        sim::player_table table{};
        session::play_session players{table};
        join(players);
        auto *inventory{players.inventory_of(1)};
        sim::item_stack tool{.item = "minecraft:diamond_sword", .count = 1};
        sim::set_stack_damage(tool, 42);
        expect(sim::set_stack_enchantments(
                   tool,
                   {sim::item_enchantment{.enchantment = "minecraft:sharpness",
                                          .level = 5},
                    sim::item_enchantment{.enchantment = "minecraft:unbreaking",
                                          .level = 3}})
                   .has_value());
        inventory->at(0) = tool;

        support::fake_play_gateway gateway{};
        gateway.active = {1};
        inventory_sync sync{gateway, &*opened, &*catalog};
        sync.tick(players);

        expect(gateway.container_content.size() == 1U);
        if (gateway.container_content.size() != 1U) {
            return;
        }
        const auto published{gateway.container_content[0].items[36]};
        expect(published.item_id == sword);
        expect(published.count == 1U);
        expect(published.components.contains(damage_id));
        expect(published.components.contains(enchantments_id));
        const auto *damage{std::get_if<net::item_component_damage>(
            &published.components.at(damage_id))};
        expect(damage != nullptr && damage->amount == 42U);
        const auto *enchantments{std::get_if<net::item_component_enchantments>(
            &published.components.at(enchantments_id))};
        expect(enchantments != nullptr && enchantments->entries.size() == 2U);
        const auto decoded{round_trip(published)};
        const bool wire_same{decoded == published};
        expect(wire_same);

        // Pick the tool up, then put it into the next hotbar slot: the menu
        // click must carry the components through to the new stack and to the
        // slot update the client receives.
        gateway.container_content.clear();
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
        expect(gateway.cursor_items.size() == 1U);
        if (gateway.cursor_items.size() != 1U) {
            return;
        }
        expect(gateway.cursor_items[0].item == published);

        const auto cursor_state{gateway.container_slots.empty()
                                    ? 2
                                    : gateway.container_slots[0].state_id};
        gateway.container_slots.clear();
        gateway.cursor_items.clear();
        gateway.container_clicks.push_back(net::container_click_event{
            .connection = 1,
            .container_id = 0,
            .state_id = cursor_state,
            .slot = 37,
            .button = 0,
            .mode = 0,
        });
        sync.tick(players);

        expect(inventory->at(1).item == "minecraft:diamond_sword");
        expect(sim::stack_damage(inventory->at(1)) == 42U);
        const auto *placed{sim::stack_enchantments(inventory->at(1))};
        expect(placed != nullptr && placed->size() == 2U);
        expect(gateway.container_slots.size() == 1U);
        if (gateway.container_slots.size() != 1U) {
            return;
        }
        expect(gateway.container_slots[0].slot == 37);
        const auto placed_wire{round_trip(gateway.container_slots[0].item)};
        expect(placed_wire.components.contains(damage_id));
        expect(placed_wire.components.contains(enchantments_id));
        const auto *placed_damage{std::get_if<net::item_component_damage>(
            &placed_wire.components.at(damage_id))};
        expect(placed_damage != nullptr && placed_damage->amount == 42U);
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
