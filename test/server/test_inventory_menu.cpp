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

    "server/inventory ignores unsupported slots and buttons"_test = [] {
        std::vector<item_stack> inventory(9);
        item_stack carried{.item = "minecraft:stone", .count = 1};
        expect(!apply_player_click(inventory, carried, 0, 0));
        expect(!apply_player_click(inventory, carried, 45, 0));
        expect(!apply_player_click(inventory, carried, 36, 2));
    };
};

} // namespace
