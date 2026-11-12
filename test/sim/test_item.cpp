// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/vanilla.hpp>
#include <refrain/sim/item.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

using refrain::sim::component_damage;
using refrain::sim::damage_result;
using refrain::sim::damage_stack;
using refrain::sim::default_stack_size;
using refrain::sim::item_component_kind;
using refrain::sim::item_definition;
using refrain::sim::item_enchantment;
using refrain::sim::item_stack;
using refrain::sim::item_table;
using refrain::sim::set_stack_custom_name;
using refrain::sim::set_stack_damage;
using refrain::sim::set_stack_enchantments;
using refrain::sim::stack_custom_name;
using refrain::sim::stack_damage;
using refrain::sim::stack_enchantments;
using refrain::sim::stack_max_damage;

/// A container with the two item records the model tests need.
auto item_tables() -> refrain::result<refrain::data::vanilla_tables> {
    refrain::data::vanilla_spec spec;
    spec.data_version = 5023;
    spec.items = {
        refrain::data::item_spec{
            .name = "test:block", .max_stack_size = 64, .max_damage = 0},
        refrain::data::item_spec{
            .name = "test:sword", .max_stack_size = 1, .max_damage = 3},
    };
    auto bytes{refrain::data::build_vanilla_tables(spec)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    return refrain::data::vanilla_tables::open(*bytes);
}

auto test_items() -> item_table {
    item_table items;
    items.insert("test:block", {.max_stack_size = 64, .max_damage = 0});
    items.insert("test:sword", {.max_stack_size = 1, .max_damage = 3});
    return items;
}

suite item_model_tests = [] {
    "sim/item table copies stack size and max damage from the registry"_test =
        [] {
            auto tables{item_tables()};
            expect(tables.has_value());
            if (!tables) {
                return;
            }
            const auto items{item_table::from_tables(*tables)};
            expect(items.size() == 2U);
            expect(items.stack_limit("test:block") == 64U);
            expect(items.stack_limit("test:sword") == 1U);
            expect(items.max_damage("test:sword") == 3U);
            expect(items.max_damage("test:block") == 0U);
            const auto *definition{items.find("test:sword")};
            expect(definition != nullptr);
            if (definition != nullptr) {
                expect(definition->max_stack_size == 1U);
            }
        };

    "sim/item table falls back for an unknown item"_test = [] {
        const auto items{test_items()};
        expect(items.find("test:unknown") == nullptr);
        expect(items.stack_limit("test:unknown") == default_stack_size);
        expect(items.max_damage("test:unknown") == 0U);
        expect(item_table{}.find("test:sword") == nullptr);
        expect(item_table{}.stack_limit("test:sword") == default_stack_size);
    };

    "sim/item damage component appears and clears"_test = [] {
        item_stack stack{.item = "test:sword", .count = 1};
        expect(stack_damage(stack) == 0U);
        expect(stack.components.empty());
        set_stack_damage(stack, 2);
        expect(stack_damage(stack) == 2U);
        expect(stack.components.contains(item_component_kind::damage));
        const auto *damage{std::get_if<component_damage>(
            &stack.components.at(item_component_kind::damage))};
        expect(damage != nullptr);
        if (damage != nullptr) {
            expect(damage->amount == 2U);
        }
        set_stack_damage(stack, 0);
        expect(stack_damage(stack) == 0U);
        expect(stack.components.empty());
    };

    "sim/item durability consumes one use at a time"_test = [] {
        const auto items{test_items()};
        item_stack stack{.item = "test:sword", .count = 1};
        expect(stack_max_damage(stack, items) == 3U);
        expect(damage_stack(stack, items) == damage_result::damaged);
        expect(stack_damage(stack) == 1U);
        expect(damage_stack(stack, items) == damage_result::damaged);
        expect(damage_stack(stack, items) == damage_result::broken);
        expect(stack.count == 0U);
        expect(stack.components.empty());
    };

    "sim/item damage ignores undamageable and empty stacks"_test = [] {
        const auto items{test_items()};
        item_stack block{.item = "test:block", .count = 5};
        expect(damage_stack(block, items) == damage_result::unchanged);
        expect(block.count == 5U);
        expect(block.components.empty());
        item_stack empty{};
        expect(damage_stack(empty, items) == damage_result::unchanged);
        item_stack sword{.item = "test:sword", .count = 1};
        expect(damage_stack(sword, items, 0) == damage_result::unchanged);
        expect(stack_damage(sword) == 0U);
    };

    "sim/item enchantments are sorted and deduplicated"_test = [] {
        item_stack stack{.item = "test:sword", .count = 1};
        expect(
            set_stack_enchantments(
                stack,
                {item_enchantment{.enchantment = "test:sharpness", .level = 5},
                 item_enchantment{.enchantment = "test:unbreaking", .level = 3},
                 item_enchantment{.enchantment = "test:sharpness", .level = 2}})
                .has_value());
        const auto *entries{stack_enchantments(stack)};
        expect(entries != nullptr);
        if (entries != nullptr) {
            expect(entries->size() == 2U);
            expect((*entries)[0].enchantment == "test:sharpness");
            expect((*entries)[0].level == 2U);
            expect((*entries)[1].enchantment == "test:unbreaking");
        }
        expect(set_stack_enchantments(stack, {}).has_value());
        expect(stack_enchantments(stack) == nullptr);
        expect(stack.components.empty());
    };

    "sim/item enchantment levels outside the protocol range are rejected"_test =
        [] {
            item_stack stack{.item = "test:sword", .count = 1};
            expect(
                !set_stack_enchantments(
                     stack, {item_enchantment{.enchantment = "test:sharpness",
                                              .level = 0}})
                     .has_value());
            expect(
                !set_stack_enchantments(
                     stack, {item_enchantment{.enchantment = "test:sharpness",
                                              .level = 256}})
                     .has_value());
            expect(
                !set_stack_enchantments(
                     stack, {item_enchantment{.enchantment = "", .level = 1}})
                     .has_value());
            expect(stack.components.empty());
        };

    "sim/item custom name appears and clears"_test = [] {
        item_stack stack{.item = "test:sword", .count = 1};
        set_stack_custom_name(stack, "Excalibur");
        const auto *name{stack_custom_name(stack)};
        expect(name != nullptr);
        if (name != nullptr) {
            expect(*name == "Excalibur");
        }
        expect(stack.components.contains(item_component_kind::custom_name));
        set_stack_custom_name(stack, "");
        expect(stack_custom_name(stack) == nullptr);
        expect(stack.components.empty());
    };

    "sim/item stacks with different components are not the same item"_test =
        [] {
            item_stack plain{.item = "test:sword", .count = 1};
            item_stack damaged{.item = "test:sword", .count = 1};
            set_stack_damage(damaged, 1);
            expect(!refrain::sim::same_item(plain, damaged));
            expect(refrain::sim::same_item(plain, plain));
            expect(plain.components != damaged.components);
        };
};

} // namespace
