// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/inventory_menu.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

using refrain::server::apply_player_click;
using refrain::server::changed_menu_slots;
using refrain::server::inventory_index_of;
using refrain::server::menu_slot_of;
using refrain::server::no_inventory_index;
using refrain::server::place_back;
using refrain::sim::item_stack;

suite inventory_menu_tests = [] {
    "server/inventory left click picks up and places a stack"_test = [] {
        std::vector<item_stack> inventory(9);
        inventory[0] = item_stack{.item = "minecraft:stone", .count = 5};
        item_stack carried{};
        expect(apply_player_click(inventory, carried, 36, 0));
        expect(carried.item == "minecraft:stone" && carried.count == 5);
        expect(inventory[0].count == 0);
        expect(apply_player_click(inventory, carried, 37, 0));
        expect(inventory[1].item == "minecraft:stone");
        expect(inventory[1].count == 5);
        expect(carried.count == 0);
    };

    "server/inventory right click takes half and places one"_test = [] {
        std::vector<item_stack> inventory(9);
        inventory[0] = item_stack{.item = "minecraft:apple", .count = 5};
        item_stack carried{};
        expect(apply_player_click(inventory, carried, 36, 1));
        expect(carried.count == 3);
        expect(inventory[0].count == 2);
        expect(apply_player_click(inventory, carried, 37, 1));
        expect(inventory[1].count == 1);
        expect(carried.count == 2);
    };

    "server/inventory click merges and swaps stacks"_test = [] {
        std::vector<item_stack> inventory(9);
        inventory[0] = item_stack{.item = "minecraft:stone", .count = 60};
        inventory[1] = item_stack{.item = "minecraft:dirt", .count = 2};
        item_stack carried{.item = "minecraft:stone", .count = 10};
        expect(apply_player_click(inventory, carried, 36, 0));
        expect(inventory[0].count == 64);
        expect(carried.count == 6);
        expect(apply_player_click(inventory, carried, 37, 0));
        expect(inventory[1].item == "minecraft:stone");
        expect(inventory[1].count == 6);
        expect(carried.item == "minecraft:dirt");
        expect(carried.count == 2);
    };

    "server/inventory menu slots round-trip through the inventory index"_test =
        [] {
            expect(menu_slot_of(0) == 36);
            expect(menu_slot_of(8) == 44);
            expect(menu_slot_of(9) == 9);
            expect(menu_slot_of(35) == 35);
            expect(menu_slot_of(36) == -1);
            expect(inventory_index_of(36) == 0U);
            expect(inventory_index_of(44) == 8U);
            expect(inventory_index_of(9) == 9U);
            expect(inventory_index_of(35) == 35U);
            expect(inventory_index_of(0) == no_inventory_index);
            expect(inventory_index_of(45) == no_inventory_index);
            for (std::size_t index{0}; index < 36; ++index) {
                expect(inventory_index_of(menu_slot_of(index)) == index);
            }
        };

    "server/inventory reports only the slots that changed"_test = [] {
        std::vector<item_stack> before(36);
        before[0] = item_stack{.item = "minecraft:stone", .count = 5};
        before[20] = item_stack{.item = "minecraft:dirt", .count = 1};
        std::vector<item_stack> after{before};
        after[0] = item_stack{};
        after[20] = item_stack{};
        after[21] = item_stack{.item = "minecraft:apple", .count = 2};
        const auto changed{changed_menu_slots(before, after)};
        expect(changed.size() == 3U);
        expect(changed[0] == 20);
        expect(changed[1] == 21);
        expect(changed[2] == 36);
        expect(changed_menu_slots(before, before).empty());
    };

    "server/inventory diff treats a missing index as empty"_test = [] {
        std::vector<item_stack> before(9);
        before[0] = item_stack{.item = "minecraft:stone", .count = 1};
        const std::vector<item_stack> after{};
        const auto changed{changed_menu_slots(before, after)};
        expect(changed.size() == 1U);
        expect(changed[0] == 36);
    };

    "server/inventory places the cursor stack back by merging first"_test = [] {
        std::vector<item_stack> inventory(36);
        inventory[9] = item_stack{.item = "minecraft:stone", .count = 60};
        const auto remainder{place_back(
            inventory, item_stack{.item = "minecraft:stone", .count = 10})};
        expect(remainder.count == 0U);
        expect(inventory[9].count == 64U);
        expect(inventory[0].item == "minecraft:stone");
        expect(inventory[0].count == 6U);
    };

    "server/inventory keeps what does not fit on the cursor"_test = [] {
        std::vector<item_stack> inventory(36);
        for (auto &slot : inventory) {
            slot = item_stack{.item = "minecraft:dirt", .count = 64};
        }
        const auto remainder{place_back(
            inventory, item_stack{.item = "minecraft:stone", .count = 3})};
        expect(remainder.item == "minecraft:stone");
        expect(remainder.count == 3U);
    };

    "server/inventory ignores unsupported slots and buttons"_test = [] {
        std::vector<item_stack> inventory(9);
        item_stack carried{.item = "minecraft:stone", .count = 1};
        expect(!apply_player_click(inventory, carried, 0, 0));
        expect(!apply_player_click(inventory, carried, 45, 0));
        expect(!apply_player_click(inventory, carried, 36, 2));
    };
};

} // namespace
