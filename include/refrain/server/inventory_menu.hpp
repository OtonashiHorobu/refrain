// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/sim/world.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace refrain::server {

/// Menu slot that mirrors an inventory index, or -1 when the index has no
/// menu slot in the modeled player menu.
[[nodiscard]] auto menu_slot_of(std::size_t inventory_index) -> std::int16_t;

/// Inventory index behind a menu slot, or `npos` when the slot is not part of
/// the modeled player menu.
[[nodiscard]] auto inventory_index_of(std::int16_t menu_slot) -> std::size_t;

inline constexpr std::size_t no_inventory_index = static_cast<std::size_t>(-1);

/// Menu slots whose stack differs between two inventory snapshots.
///
/// A missing index counts as an empty stack, so a snapshot that grew reports
/// the new slots. Slots without a menu slot are skipped and the result is
/// ordered by menu slot.
[[nodiscard]] auto changed_menu_slots(std::span<const sim::item_stack> before,
                                      std::span<const sim::item_stack> after)
    -> std::vector<std::int16_t>;

/// Put a stack back into the player inventory, merging into matching stacks
/// first and using empty slots afterwards. Stacks merge only when their item
/// and components match, and the item's own stack limit caps each slot.
/// Returns the remainder that did not fit; a remainder count of zero means the
/// stack was placed completely.
[[nodiscard]] auto place_back(std::vector<sim::item_stack> &inventory,
                              sim::item_stack stack,
                              const sim::item_table &items) -> sim::item_stack;

/// Apply one mouse click to the player inventory menu.
///
/// Menu slots: 36..44 are the hotbar (inventory 0..8), 9..35 are the main
/// inventory (inventory 9..35). Other slots (crafting, armor, offhand) are not
/// modeled yet and return false.
///
/// Button 0 is a left click (take/place/merge/swap a whole stack); button 1 is
/// a right click (take half or place one). Returns true when the inventory or
/// the carried stack changed.
[[nodiscard]] auto apply_player_click(std::vector<sim::item_stack> &inventory,
                                      sim::item_stack &carried,
                                      std::int16_t menu_slot,
                                      std::int8_t button,
                                      const sim::item_table &items) -> bool;

} // namespace refrain::server
