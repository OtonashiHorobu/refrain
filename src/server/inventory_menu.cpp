// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/inventory_menu.hpp>

#include <algorithm>
#include <cstddef>

namespace refrain::server {

namespace {

[[nodiscard]] auto empty(const sim::item_stack &stack) noexcept -> bool {
    return stack.count == 0 || stack.item.empty();
}

void clear(sim::item_stack &stack) noexcept {
    stack.item.clear();
    stack.count = 0;
}

[[nodiscard]] auto same_stack(const sim::item_stack &left,
                              const sim::item_stack &right) noexcept -> bool {
    return left.count == right.count && left.item == right.item;
}

} // namespace

auto inventory_index_of(std::int16_t menu_slot) -> std::size_t {
    if (menu_slot >= 36 && menu_slot <= 44) {
        return static_cast<std::size_t>(menu_slot - 36);
    }
    if (menu_slot >= 9 && menu_slot <= 35) {
        return static_cast<std::size_t>(menu_slot);
    }
    return no_inventory_index;
}

auto menu_slot_of(std::size_t inventory_index) -> std::int16_t {
    if (inventory_index < 9) {
        return static_cast<std::int16_t>(36 + inventory_index);
    }
    if (inventory_index < 36) {
        return static_cast<std::int16_t>(inventory_index);
    }
    return -1;
}

auto changed_menu_slots(std::span<const sim::item_stack> before,
                        std::span<const sim::item_stack> after)
    -> std::vector<std::int16_t> {
    std::vector<std::int16_t> changed{};
    for (std::int16_t slot{9}; slot <= 44; ++slot) {
        const auto index{inventory_index_of(slot)};
        const sim::item_stack empty{};
        const auto &old_stack{index < before.size() ? before[index] : empty};
        const auto &new_stack{index < after.size() ? after[index] : empty};
        if (!same_stack(old_stack, new_stack)) {
            changed.push_back(slot);
        }
    }
    return changed;
}

auto place_back(std::vector<sim::item_stack> &inventory, sim::item_stack stack)
    -> sim::item_stack {
    if (empty(stack)) {
        return sim::item_stack{};
    }
    for (std::size_t index{0}; index < 36; ++index) {
        if (inventory.size() <= index) {
            inventory.resize(index + 1);
        }
        auto &target{inventory[index]};
        if (empty(target) || target.item != stack.item) {
            continue;
        }
        const auto space{sim::max_stack_size - target.count};
        const auto moved{std::min(space, stack.count)};
        target.count += moved;
        stack.count -= moved;
        if (stack.count == 0) {
            return sim::item_stack{};
        }
    }
    for (std::size_t index{0}; index < 36; ++index) {
        if (inventory.size() <= index) {
            inventory.resize(index + 1);
        }
        auto &target{inventory[index]};
        if (!empty(target)) {
            continue;
        }
        target = stack;
        return sim::item_stack{};
    }
    return stack;
}

auto apply_player_click(std::vector<sim::item_stack> &inventory,
                        sim::item_stack &carried, std::int16_t menu_slot,
                        std::int8_t button) -> bool {
    const auto index{inventory_index_of(menu_slot)};
    if (index == no_inventory_index || (button != 0 && button != 1)) {
        return false;
    }
    if (inventory.size() <= index) {
        inventory.resize(index + 1);
    }
    auto &stack{inventory[index]};

    if (button == 0) {
        if (empty(carried)) {
            if (empty(stack)) {
                return false;
            }
            carried = stack;
            clear(stack);
            return true;
        }
        if (empty(stack)) {
            stack = carried;
            clear(carried);
            return true;
        }
        if (stack.item == carried.item) {
            const auto space{sim::max_stack_size - stack.count};
            const auto moved{std::min(space, carried.count)};
            if (moved == 0) {
                return false;
            }
            stack.count += moved;
            carried.count -= moved;
            if (carried.count == 0) {
                clear(carried);
            }
            return true;
        }
        std::swap(stack, carried);
        return true;
    }

    if (empty(carried)) {
        if (empty(stack)) {
            return false;
        }
        const auto taken{(stack.count + 1U) / 2U};
        carried = stack;
        carried.count = taken;
        stack.count -= taken;
        if (stack.count == 0) {
            clear(stack);
        }
        return true;
    }
    if (empty(stack) || stack.item == carried.item) {
        if (stack.count >= sim::max_stack_size) {
            return false;
        }
        if (empty(stack)) {
            stack.item = carried.item;
        }
        ++stack.count;
        --carried.count;
        if (carried.count == 0) {
            clear(carried);
        }
        return true;
    }
    return false;
}

} // namespace refrain::server
