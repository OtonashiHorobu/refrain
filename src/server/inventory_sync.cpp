// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/inventory_sync.hpp>

#include <refrain/core/log.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/server/inventory_menu.hpp>

#include <cstddef>
#include <utility>

namespace {

constexpr refrain::log::logger log_server{"server"};

} // namespace

namespace refrain::server {

inventory_sync::inventory_sync(play_gateway &gateway,
                               const refrain::data::vanilla_tables *tables)
    : gateway_(gateway) {
    // Map runtime item keys to protocol item ids once; the menu sync then
    // only compares and encodes stacks.
    if (tables == nullptr) {
        return;
    }
    if (const auto registry{tables->find_registry("minecraft:item")}) {
        const auto count{tables->registry_entry_count(*registry)};
        item_ids_.reserve(count);
        for (std::uint32_t index{0}; index < count; ++index) {
            const auto entry{tables->registry_entry(*registry, index)};
            if (entry.protocol_id >= 0) {
                const auto id{static_cast<std::uint32_t>(entry.protocol_id)};
                item_ids_.emplace(std::string{entry.name}, id);
                item_names_.emplace(id, std::string{entry.name});
            }
        }
    }
}

auto inventory_sync::protocol_stack(const refrain::sim::item_stack &stack) const
    -> refrain::net::item_stack {
    if (stack.count == 0 || stack.item.empty()) {
        return refrain::net::item_stack{};
    }
    const auto item{item_ids_.find(stack.item)};
    if (item == item_ids_.end()) {
        return refrain::net::item_stack{};
    }
    return refrain::net::item_stack{
        .count = stack.count,
        .item_id = item->second,
    };
}

auto inventory_sync::next_state(std::uint64_t connection) -> std::int32_t {
    auto &state{inventory_states_[connection]};
    state = (state + 1) & 32767;
    return state;
}

void inventory_sync::tick(refrain::session::play_session &players) {
    if (!gateway_.available()) {
        return;
    }
    process_container_clicks(players);
    process_inventory(players);
}

void inventory_sync::process_container_clicks(
    refrain::session::play_session &players) {
    for (const auto &slot : gateway_.take_creative_slots()) {
        auto *inventory = players.inventory_of(slot.connection);
        const auto index{static_cast<std::size_t>(slot.slot)};
        if (inventory == nullptr || slot.slot < 0 ||
            index >= inventory->size()) {
            continue;
        }
        auto &target{(*inventory)[index]};
        if (slot.item_id == 0 || slot.count == 0) {
            target = sim::item_stack{};
        } else {
            const auto name{item_names_.find(slot.item_id)};
            if (name == item_names_.end()) {
                continue;
            }
            target = sim::item_stack{
                .item = name->second,
                .count = slot.count,
            };
        }
        inventory_forced_.insert(slot.connection);
    }
    for (const auto &close : gateway_.take_container_closes()) {
        if (close.container_id != 0) {
            continue;
        }
        if (auto *inventory = players.inventory_of(close.connection)) {
            const auto carried{carried_items_.find(close.connection)};
            if (carried != carried_items_.end()) {
                const auto remainder{
                    refrain::server::place_back(*inventory, carried->second)};
                if (remainder.count == 0) {
                    carried_items_.erase(close.connection);
                } else {
                    carried->second = remainder;
                }
            }
        }
        inventory_forced_.insert(close.connection);
    }
    for (const auto &click : gateway_.take_container_clicks()) {
        if (click.container_id != 0) {
            continue;
        }
        auto *inventory = players.inventory_of(click.connection);
        if (inventory == nullptr) {
            continue;
        }
        const auto known{inventory_states_.find(click.connection)};
        const auto stale{known == inventory_states_.end() ||
                         known->second != click.state_id};
        if (click.mode != 0) {
            // Shift, drag and number-key clicks are not modeled; resend
            // the authoritative state so the client prediction reverts.
            inventory_forced_.insert(click.connection);
            continue;
        }
        auto &carried{carried_items_[click.connection]};
        const auto changed{refrain::server::apply_player_click(
            *inventory, carried, click.slot, click.button)};
        log_server.debug("click conn={} slot={} button={} mode={} "
                         "state={} stale={} -> carried='{}' x{} changed={}",
                         click.connection, click.slot, click.button, click.mode,
                         click.state_id, stale, carried.item, carried.count,
                         changed);
        if (stale) {
            inventory_forced_.insert(click.connection);
        }
    }
}

void inventory_sync::process_inventory(
    const refrain::session::play_session &players) {
    const auto active_ids{gateway_.active_connections()};
    const std::set<std::uint64_t> active{active_ids.begin(), active_ids.end()};
    for (auto iterator = inventory_snapshots_.begin();
         iterator != inventory_snapshots_.end();) {
        if (!active.contains(iterator->first)) {
            iterator = inventory_snapshots_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = carried_snapshots_.begin();
         iterator != carried_snapshots_.end();) {
        if (!active.contains(iterator->first)) {
            iterator = carried_snapshots_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = inventory_states_.begin();
         iterator != inventory_states_.end();) {
        if (!active.contains(iterator->first)) {
            iterator = inventory_states_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (const auto id : active_ids) {
        const auto *record{players.player_of(id)};
        if (record == nullptr) {
            continue;
        }
        const auto &inventory{record->inventory};
        const auto previous{inventory_snapshots_.find(id)};
        const auto forced{inventory_forced_.contains(id)};
        if (previous == inventory_snapshots_.end() || forced) {
            std::vector<refrain::net::item_stack> slots(46);
            for (std::size_t index{0}; index < inventory.size(); ++index) {
                const auto slot{refrain::server::menu_slot_of(index)};
                if (slot < 0) {
                    continue;
                }
                slots[static_cast<std::size_t>(slot)] =
                    protocol_stack(inventory[index]);
            }
            auto carried{refrain::net::item_stack{}};
            if (const auto stack{carried_items_.find(id)};
                stack != carried_items_.end()) {
                carried = protocol_stack(stack->second);
            }
            gateway_.send_container_content(id, 0, next_state(id), slots,
                                            carried);
            carried_snapshots_[id] = carried;
        } else {
            for (const auto slot : refrain::server::changed_menu_slots(
                     previous->second, inventory)) {
                const auto index{refrain::server::inventory_index_of(slot)};
                const auto stack{index < inventory.size()
                                     ? protocol_stack(inventory[index])
                                     : refrain::net::item_stack{}};
                gateway_.send_container_set_slot(id, 0, next_state(id), slot,
                                                 stack);
            }
            auto carried{refrain::net::item_stack{}};
            if (const auto stack{carried_items_.find(id)};
                stack != carried_items_.end()) {
                carried = protocol_stack(stack->second);
            }
            const auto sent{carried_snapshots_.find(id)};
            if (sent == carried_snapshots_.end() || sent->second != carried) {
                gateway_.send_cursor_item(id, carried);
                carried_snapshots_[id] = carried;
            }
        }
        inventory_snapshots_[id] = inventory;
    }
    inventory_forced_.clear();
    for (auto iterator = carried_items_.begin();
         iterator != carried_items_.end();) {
        if (!active.contains(iterator->first)) {
            iterator = carried_items_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

} // namespace refrain::server
