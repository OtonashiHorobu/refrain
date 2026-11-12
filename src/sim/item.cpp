// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/item.hpp>

#include <algorithm>
#include <utility>

namespace refrain::sim {

namespace {

template <typename T>
[[nodiscard]] auto component(const item_stack &stack, item_component_kind kind)
    -> const T * {
    const auto found{stack.components.find(kind)};
    if (found == stack.components.end()) {
        return nullptr;
    }
    return std::get_if<T>(&found->second);
}

} // namespace

auto item_table::from_tables(const data::vanilla_tables &tables) -> item_table {
    item_table table;
    const auto count{tables.item_count()};
    for (std::uint32_t index{0}; index < count; ++index) {
        const auto entry{tables.item_at(index)};
        table.items_.emplace(std::string{entry.name},
                             item_definition{
                                 .max_stack_size = entry.max_stack_size,
                                 .max_damage = entry.max_damage,
                             });
    }
    return table;
}

void item_table::insert(std::string name, item_definition definition) {
    items_.insert_or_assign(std::move(name), definition);
}

auto item_table::find(std::string_view item) const noexcept
    -> const item_definition * {
    const auto found{items_.find(item)};
    return found == items_.end() ? nullptr : &found->second;
}

auto item_table::stack_limit(std::string_view item) const noexcept
    -> std::uint32_t {
    const auto *definition{find(item)};
    return definition == nullptr ? default_stack_size
                                 : definition->max_stack_size;
}

auto item_table::max_damage(std::string_view item) const noexcept
    -> std::uint32_t {
    const auto *definition{find(item)};
    return definition == nullptr ? 0U : definition->max_damage;
}

auto stack_empty(const item_stack &stack) noexcept -> bool {
    return stack.count == 0 || stack.item.empty();
}

auto same_item(const item_stack &left, const item_stack &right) noexcept
    -> bool {
    return left.item == right.item && left.components == right.components;
}

auto stack_damage(const item_stack &stack) noexcept -> std::uint32_t {
    const auto *damage{
        component<component_damage>(stack, item_component_kind::damage)};
    return damage == nullptr ? 0U : damage->amount;
}

auto stack_enchantments(const item_stack &stack) noexcept
    -> const std::vector<item_enchantment> * {
    const auto *enchantments{component<component_enchantments>(
        stack, item_component_kind::enchantments)};
    return enchantments == nullptr ? nullptr : &enchantments->entries;
}

auto stack_custom_name(const item_stack &stack) noexcept
    -> const std::string * {
    const auto *name{component<component_custom_name>(
        stack, item_component_kind::custom_name)};
    return name == nullptr ? nullptr : &name->text;
}

auto stack_max_damage(const item_stack &stack, const item_table &table) noexcept
    -> std::uint32_t {
    return table.max_damage(stack.item);
}

void set_stack_damage(item_stack &stack, std::uint32_t amount) noexcept {
    if (amount == 0) {
        stack.components.erase(item_component_kind::damage);
        return;
    }
    stack.components.insert_or_assign(item_component_kind::damage,
                                      component_damage{.amount = amount});
}

auto set_stack_enchantments(item_stack &stack,
                            std::vector<item_enchantment> entries) -> status {
    // A later level for the same enchantment replaces the earlier one, the
    // same way a map assignment does, and the component keeps one sorted entry
    // per enchantment.
    std::map<std::string, std::uint32_t, std::less<>> unique;
    for (auto &entry : entries) {
        if (entry.enchantment.empty()) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "item enchantment name must not be empty")};
        }
        if (entry.level < 1U || entry.level > 255U) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "item enchantment level is out of range")};
        }
        unique.insert_or_assign(std::move(entry.enchantment), entry.level);
    }
    if (unique.empty()) {
        stack.components.erase(item_component_kind::enchantments);
        return status_ok();
    }
    std::vector<item_enchantment> sorted;
    sorted.reserve(unique.size());
    for (auto &[enchantment, level] : unique) {
        sorted.push_back(
            item_enchantment{.enchantment = enchantment, .level = level});
    }
    stack.components.insert_or_assign(
        item_component_kind::enchantments,
        component_enchantments{.entries = std::move(sorted)});
    return status_ok();
}

void set_stack_custom_name(item_stack &stack, std::string text) {
    if (text.empty()) {
        stack.components.erase(item_component_kind::custom_name);
        return;
    }
    stack.components.insert_or_assign(
        item_component_kind::custom_name,
        component_custom_name{.text = std::move(text)});
}

auto damage_stack(item_stack &stack, const item_table &table,
                  std::uint32_t amount) -> damage_result {
    const auto limit{stack_max_damage(stack, table)};
    if (limit == 0 || amount == 0 || stack_empty(stack)) {
        return damage_result::unchanged;
    }
    const auto current{stack_damage(stack)};
    const auto total{static_cast<std::uint64_t>(current) + amount};
    if (total >= limit) {
        stack.count = 0;
        stack.components.clear();
        return damage_result::broken;
    }
    set_stack_damage(stack, static_cast<std::uint32_t>(total));
    return damage_result::damaged;
}

} // namespace refrain::sim
