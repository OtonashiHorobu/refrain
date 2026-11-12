// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Player inventory and container menu sync. The service owns the item-id map,
// the per-connection menu state ids and the snapshots it diffs; the gateway
// owns the connections and the container packet encoding.

#pragma once

#include <refrain/net/wire.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/session/play_session.hpp>
#include <refrain/sim/world.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace refrain::data {
class vanilla_tables;
} // namespace refrain::data

namespace refrain::net {
class registry_catalog;
} // namespace refrain::net

namespace refrain::server {

/// Mirrors the simulation inventory into the player's container menu.
///
/// One `tick()` applies the clicks, closes and creative-slot events the
/// network layer queued, then sends the resulting menu state. A connection
/// that has no snapshot yet, or that has to be resynchronised, receives the
/// full 46-slot contents; otherwise only the changed menu slots and the
/// carried stack go out.
class inventory_sync {
  public:
    /// `tables` supplies the runtime item key to protocol item id map and the
    /// per-item stack sizes and max damage. `catalog` supplies the enchantment
    /// protocol ids the item component codec needs; without it an enchanted
    /// stack is published without its enchantment component.
    ///
    /// A null table publishes no item and behaves like an empty registry.
    inventory_sync(play_gateway &gateway,
                   const refrain::data::vanilla_tables *tables,
                   const refrain::net::registry_catalog *catalog = nullptr);

    void tick(refrain::session::play_session &players);

  private:
    void process_container_clicks(refrain::session::play_session &players);
    void process_inventory(const refrain::session::play_session &players);

    /// Runtime stack to protocol stack; unknown items and empty stacks encode
    /// as an empty stack.
    [[nodiscard]] auto
    protocol_stack(const refrain::sim::item_stack &stack) const
        -> refrain::net::item_stack;

    /// Menu state id echoed by the client. Ids wrap at 15 bits like the client
    /// counter.
    auto next_state(std::uint64_t connection) -> std::int32_t;

    play_gateway &gateway_;
    refrain::sim::item_table items_{};
    std::unordered_map<std::string, std::uint32_t> item_ids_{};
    std::unordered_map<std::uint32_t, std::string> item_names_{};
    std::unordered_map<std::string, std::uint32_t> enchantment_ids_{};
    std::uint32_t damage_component_id_{0};
    std::uint32_t custom_name_component_id_{0};
    std::uint32_t enchantments_component_id_{0};
    bool component_ids_{false};
    std::set<std::uint64_t> inventory_forced_{};
    std::unordered_map<std::uint64_t, refrain::sim::item_stack>
        carried_items_{};
    std::unordered_map<std::uint64_t, std::vector<refrain::sim::item_stack>>
        inventory_snapshots_{};
    std::unordered_map<std::uint64_t, refrain::net::item_stack>
        carried_snapshots_{};
    std::unordered_map<std::uint64_t, std::int32_t> inventory_states_{};
};

} // namespace refrain::server
