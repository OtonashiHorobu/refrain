// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/sim/world.hpp>

#include <cstdint>
#include <vector>

namespace refrain::server {

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
                                      std::int8_t button) -> bool;

} // namespace refrain::server
