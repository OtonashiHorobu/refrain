// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/inventory_menu.hpp>

#include <algorithm>
#include <cstddef>

namespace refrain::server {

namespace {

[[nodiscard]] auto inventory_index(std::int16_t menu_slot) -> std::size_t {
    if (menu_slot >= 36 && menu_slot <= 44) {
        return static_cast<std::size_t>(menu_slot - 36);
    }
    if (menu_slot >= 9 && menu_slot <= 35) {
        return static_cast<std::size_t>(menu_slot);
    }
    return static_cast<std::size_t>(-1);
}

[[nodiscard]] auto empty(const sim::item_stack &stack) noexcept -> bool {
    return stack.count == 0 || stack.item.empty();
}

void clear(sim::item_stack &stack) noexcept {
    stack.item.clear();
    stack.count = 0;
}

} // namespace

auto apply_player_click(std::vector<sim::item_stack> &inventory,
                        sim::item_stack &carried, std::int16_t menu_slot,
                        std::int8_t button) -> bool {
    const auto index{inventory_index(menu_slot)};
    if (index == static_cast<std::size_t>(-1) || (button != 0 && button != 1)) {
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
