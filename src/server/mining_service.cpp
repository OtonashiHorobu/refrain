// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/mining_service.hpp>

#include <refrain/core/log.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

constexpr refrain::log::logger log_mining{"mining"};

/// Horizontal chunk coordinate of a block coordinate, floored like the
/// reference's `SectionPos.blockToSectionCoord`.
[[nodiscard]] auto chunk_of(std::int32_t block) noexcept -> std::int32_t {
    constexpr std::int32_t edge{16};
    return block >= 0 ? block / edge : (block - (edge - 1)) / edge;
}

[[nodiscard]] auto same_pos(const refrain::sim::block_pos &left,
                            const refrain::sim::block_pos &right) noexcept
    -> bool {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

} // namespace

namespace refrain::server::mining {

auto block_name(std::string_view key) noexcept -> std::string_view {
    const auto bracket{key.find('[')};
    return bracket == std::string_view::npos ? key : key.substr(0, bracket);
}

auto correct_tool(const data::block_mining &block,
                  const data::tool_mining *tool) noexcept -> bool {
    if (!block.requires_correct_tool_for_drops()) {
        return true;
    }
    return tool != nullptr && data::correct_tool_for_drops(*tool, block);
}

auto per_tick_progress(const data::block_mining &block,
                       const data::tool_mining *tool) noexcept -> float {
    if (block.unbreakable()) {
        return 0.0F;
    }
    // TODO: the reference multiplies the tool speed by the player's effects
    // (haste, mining fatigue), the efficiency enchantment, the
    // block-break-speed attribute and the submerged speed, and divides it
    // while airborne; none of those are modelled, so the table speed is used
    // as is.
    const auto speed{tool == nullptr ? 1.0F : data::mining_speed(*tool, block)};
    const auto modifier{correct_tool(block, tool) ? 30.0F : 100.0F};
    // The reference divides the speed by the destroy time and then by the
    // modifier. A zero destroy time makes the quotient infinite, which is how
    // a block with no destroy time breaks on the first tick.
    return speed / block.hardness / modifier;
}

auto accumulated_progress(float per_tick, std::int64_t elapsed) noexcept
    -> float {
    return per_tick * static_cast<float>(elapsed + 1);
}

auto stage_of(float progress) noexcept -> std::int32_t {
    const auto scaled{progress * 10.0F};
    if (!std::isfinite(scaled) || scaled >= 2147483000.0F) {
        return 10;
    }
    return static_cast<std::int32_t>(scaled);
}

auto within_reach(double eye_x, double eye_y, double eye_z,
                  const sim::block_pos &pos, double range) noexcept -> bool {
    // Distance from the eye point to the nearest point of the block's unit
    // box, matching the reference's AABB.distanceToSqr on the block cell.
    const auto distance_axis = [](double eye, double low, double high) {
        if (eye < low) {
            return low - eye;
        }
        if (eye > high) {
            return eye - high;
        }
        return 0.0;
    };
    const auto dx{distance_axis(eye_x, static_cast<double>(pos.x),
                                static_cast<double>(pos.x) + 1.0)};
    const auto dy{distance_axis(eye_y, static_cast<double>(pos.y),
                                static_cast<double>(pos.y) + 1.0)};
    const auto dz{distance_axis(eye_z, static_cast<double>(pos.z),
                                static_cast<double>(pos.z) + 1.0)};
    return dx * dx + dy * dy + dz * dz < range * range;
}

} // namespace refrain::server::mining

namespace refrain::server {

mining_service::mining_service(const data::block_mining_table &blocks,
                               const data::item_tool_table &tools,
                               world_port &world, sim::player_table &players,
                               outbound out)
    : blocks_{blocks}, tools_{tools}, world_{world}, players_{players},
      out_{std::move(out)} {}

auto mining_service::player(std::uint64_t connection) const noexcept
    -> sim::player_state * {
    return players_.find_by_connection(connection);
}

auto mining_service::mining_table(sim::block_state_id state) const noexcept
    -> const data::block_mining & {
    const auto key{world_.block_key(state)};
    if (key.empty()) {
        return data::block_mining_table::missing();
    }
    const auto *found{blocks_.find(mining::block_name(key))};
    return found == nullptr ? data::block_mining_table::missing() : *found;
}

auto mining_service::held_tool(const sim::player_state &player,
                               std::uint8_t slot) const noexcept
    -> const data::tool_mining * {
    if (slot >= player.inventory.size()) {
        return nullptr;
    }
    const auto &stack{player.inventory[slot]};
    if (sim::stack_empty(stack)) {
        return nullptr;
    }
    return tools_.find(stack.item);
}

void mining_service::send_stage(sim::entity_id miner, std::string_view world,
                                const sim::block_pos &pos, std::int32_t stage) {
    if (!out_.destroy_stage) {
        return;
    }
    const auto entity_id{static_cast<std::int32_t>(miner)};
    // The reference broadcasts the animation to every other player in the same
    // level within 32 blocks; the player that mines renders its own prediction.
    players_.for_each([&](const sim::player_state &observer) {
        if (observer.id == miner ||
            observer.connection == sim::synthetic_connection ||
            observer.world != world) {
            return;
        }
        const auto dx{static_cast<double>(pos.x) - observer.x};
        const auto dy{static_cast<double>(pos.y) - observer.y};
        const auto dz{static_cast<double>(pos.z) - observer.z};
        if (dx * dx + dy * dy + dz * dz >= 1024.0) {
            return;
        }
        out_.destroy_stage(observer.connection, entity_id, pos, stage);
    });
}

void mining_service::clear_stage(const mining_progress &state,
                                 const sim::block_pos &pos) {
    send_stage(state.entity, state.world, pos, -1);
}

void mining_service::send_block_state(std::uint64_t connection,
                                      const sim::block_pos &pos,
                                      sim::block_state_id state) {
    if (!out_.block_state || connection == sim::synthetic_connection) {
        return;
    }
    out_.block_state(connection, pos, state);
}

auto mining_service::break_block(sim::player_state &actor,
                                 const sim::block_pos &pos,
                                 const data::block_mining &block,
                                 std::uint8_t tool_slot, bool damage_tool)
    -> bool {
    const auto current{world_.block_at(pos)};
    if (current == sim::air_block) {
        return false;
    }
    const std::array<sim::block_change, 1> batch{sim::block_change{
        .pos = pos, .old_state = current, .new_state = sim::air_block}};
    // The world port publishes the change and records the edit the chunk
    // streamer drains into a `block_update` in the same tick.
    if (auto applied{world_.set_blocks(batch)}; !applied) {
        log_mining.warn("failed to break the block at {}, {}, {}: {}", pos.x,
                        static_cast<std::int32_t>(pos.y), pos.z,
                        applied.error().message);
        return false;
    }
    // TODO: the block leaves no drop, no experience and no release of its
    // block entity contents; the loot table, the item entity and the pickup
    // path are not implemented.
    // The reference charges the tool for every block whose destroy time is not
    // zero; an instant block costs no durability and creative breaks are free.
    if (damage_tool && block.hardness != 0.0F &&
        tool_slot < actor.inventory.size()) {
        if (const auto *tool{held_tool(actor, tool_slot)};
            tool != nullptr && tool->damage_per_block > 0) {
            (void)world_.damage_item(actor, tool_slot, tool->damage_per_block);
        }
    }
    return true;
}

auto mining_service::advance(std::uint64_t connection, sim::player_state &actor,
                             mining_progress &state, const sim::block_pos &pos,
                             std::int64_t start_tick) -> bool {
    const auto state_id{world_.block_at(pos)};
    const auto &block{mining_table(state_id)};
    if (state_id == sim::air_block || block.is_air()) {
        // The block left the world underneath the break; drop the progress and
        // clear the animation.
        clear_stage(state, pos);
        return true;
    }
    const auto *tool{held_tool(actor, state.tool_slot)};
    const auto progress{mining::accumulated_progress(
        mining::per_tick_progress(block, tool), ticks_ - start_tick)};
    // TODO: the reference emits a block-break sound and particle level event on
    // every mining tick and a destroy event; the runtime has no level-event
    // sender, so only the destruction animation packet is modelled.
    const auto stage{mining::stage_of(progress)};
    if (stage != state.last_stage) {
        send_stage(state.entity, state.world, pos, stage);
        state.last_stage = stage;
    }
    if (progress >= 1.0F) {
        clear_stage(state, pos);
        if (!break_block(actor, pos, block, state.tool_slot, true)) {
            send_block_state(connection, pos, world_.block_at(pos));
        }
        return true;
    }
    return false;
}

void mining_service::tick() {
    ++ticks_;
    for (auto iterator{states_.begin()}; iterator != states_.end();) {
        auto &state{iterator->second};
        const auto active_pos{state.delayed ? state.delayed_pos : state.pos};
        auto *actor{player(iterator->first)};
        if (actor == nullptr || actor->dead) {
            // The connection left or the player died: the player record is
            // gone, so only the observers still see the animation.
            clear_stage(state, active_pos);
            iterator = states_.erase(iterator);
            continue;
        }
        if (actor->game_mode != state.game_mode) {
            clear_stage(state, active_pos);
            iterator = states_.erase(iterator);
            continue;
        }
        const auto ended{state.delayed ? advance(iterator->first, *actor, state,
                                                 state.delayed_pos,
                                                 state.delayed_start_tick)
                                       : advance(iterator->first, *actor, state,
                                                 state.pos, state.start_tick)};
        if (ended) {
            iterator = states_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

void mining_service::start(std::uint64_t connection,
                           const sim::block_pos &pos) {
    auto *actor{player(connection)};
    if (actor == nullptr || actor->dead) {
        return;
    }
    // TODO: the reference also refuses a break under spawn protection, outside
    // the world border and on a game-master block (command block, barrier,
    // structure block) without the matching permission; this runtime models
    // none of those, so every non-air block breaks under the same rules.
    // Adventure and spectator modes restrict block actions. The client
    // predicted a break, so it gets the authoritative state back.
    if (actor->game_mode == sim::game_mode_adventure ||
        actor->game_mode == sim::game_mode_spectator) {
        // TODO: adventure mode refuses the break unconditionally here; the
        // reference lets an item with a `can_break` component break the blocks
        // that component names, and the item model does not carry that
        // component yet.
        send_block_state(connection, pos, world_.block_at(pos));
        return;
    }
    if (pos.y > sim::world::build_max_y()) {
        send_block_state(connection, pos, world_.block_at(pos));
        return;
    }
    // A column the runtime has not loaded has no authoritative block to mine;
    // the reference would load it, this server leaves that to the chunk
    // pipeline and refuses the action instead.
    if (!world_.column_loaded(
            sim::chunk_pos{chunk_of(pos.x), chunk_of(pos.z)})) {
        return;
    }
    const auto range{actor->game_mode == sim::game_mode_creative
                         ? mining::creative_interaction_range
                         : mining::interaction_range};
    if (!mining::within_reach(actor->x, actor->y + mining::eye_height, actor->z,
                              pos, range)) {
        return;
    }
    const auto state_id{world_.block_at(pos)};
    const auto &block{mining_table(state_id)};
    if (state_id == sim::air_block || block.is_air()) {
        // Nothing to break. The reference reports success without changing the
        // world, so the client's prediction already matches.
        return;
    }
    if (actor->game_mode == sim::game_mode_creative) {
        // Creative breaks immediately, but an item whose tool component forbids
        // creative destruction (a sword, a mace, a trident) cannot break it.
        const auto *tool{held_tool(*actor, actor->selected_slot)};
        if (tool != nullptr && !tool->can_destroy_blocks_in_creative) {
            send_block_state(connection, pos, state_id);
            return;
        }
        if (!break_block(*actor, pos, block, actor->selected_slot, false)) {
            send_block_state(connection, pos, world_.block_at(pos));
        }
        return;
    }
    // A new target replaces the previous break. The old block keeps its state
    // but may still show an animation for the observers.
    if (auto found{states_.find(connection)}; found != states_.end()) {
        const auto &previous{found->second};
        const auto previous_pos{previous.delayed ? previous.delayed_pos
                                                 : previous.pos};
        clear_stage(previous, previous_pos);
        if (previous.destroying && !same_pos(previous.pos, pos)) {
            send_block_state(connection, previous.pos,
                             world_.block_at(previous.pos));
        }
        states_.erase(found);
    }
    const auto *tool{held_tool(*actor, actor->selected_slot)};
    const auto per_tick{mining::per_tick_progress(block, tool)};
    const auto first{mining::accumulated_progress(per_tick, 0)};
    if (first >= 1.0F) {
        // The reference's "insta mine": a block the held tool removes in one
        // tick, such as a zero destroy time block.
        if (!break_block(*actor, pos, block, actor->selected_slot, true)) {
            send_block_state(connection, pos, world_.block_at(pos));
        }
        return;
    }
    mining_progress &created{states_[connection]};
    created = mining_progress{};
    created.destroying = true;
    created.pos = pos;
    created.start_tick = ticks_;
    created.game_mode = actor->game_mode;
    created.tool_slot = actor->selected_slot;
    created.entity = actor->id;
    created.world = actor->world;
    created.last_stage = mining::stage_of(first);
    send_stage(created.entity, created.world, pos, created.last_stage);
}

void mining_service::stop(std::uint64_t connection, const sim::block_pos &pos) {
    auto *actor{player(connection)};
    if (actor == nullptr || actor->dead) {
        return;
    }
    const auto found{states_.find(connection)};
    if (found == states_.end()) {
        return;
    }
    auto &state{found->second};
    if (!same_pos(state.pos, pos) || !state.destroying) {
        return;
    }
    const auto state_id{world_.block_at(pos)};
    const auto &block{mining_table(state_id)};
    if (state_id == sim::air_block || block.is_air()) {
        clear_stage(state, pos);
        states_.erase(found);
        return;
    }
    const auto *tool{held_tool(*actor, state.tool_slot)};
    const auto progress{mining::accumulated_progress(
        mining::per_tick_progress(block, tool), ticks_ - state.start_tick)};
    // A client reports a finished block; the reference accepts it once the
    // server's own accumulation has passed 0.7 of the destroy time. Below that
    // the block keeps accumulating without a new start packet, so a client that
    // is one network round ahead does not lose its break.
    if (progress >= 0.7F) {
        const auto tool_slot{state.tool_slot};
        clear_stage(state, pos);
        states_.erase(found);
        if (!break_block(*actor, pos, block, tool_slot, true)) {
            send_block_state(connection, pos, world_.block_at(pos));
        }
        return;
    }
    state.destroying = false;
    state.delayed = true;
    state.delayed_pos = pos;
    state.delayed_start_tick = state.start_tick;
}

void mining_service::abort(std::uint64_t connection,
                           const sim::block_pos &pos) {
    auto *actor{player(connection)};
    const auto found{states_.find(connection)};
    if (found != states_.end()) {
        const auto &state{found->second};
        const auto active_pos{state.delayed ? state.delayed_pos : state.pos};
        clear_stage(state, active_pos);
        if (!same_pos(active_pos, pos)) {
            clear_stage(state, pos);
        }
        states_.erase(found);
        return;
    }
    if (actor != nullptr) {
        send_stage(actor->id, actor->world, pos, -1);
    }
}

void mining_service::reset(std::uint64_t connection) {
    const auto found{states_.find(connection)};
    if (found == states_.end()) {
        return;
    }
    clear_stage(found->second, found->second.delayed ? found->second.delayed_pos
                                                     : found->second.pos);
    states_.erase(found);
}

void mining_service::clear() {
    for (const auto &[connection, state] : states_) {
        static_cast<void>(connection);
        clear_stage(state, state.delayed ? state.delayed_pos : state.pos);
    }
    states_.clear();
}

auto mining_service::progress_of(std::uint64_t connection) const noexcept
    -> const mining_progress * {
    const auto found{states_.find(connection)};
    return found == states_.end() ? nullptr : &found->second;
}

} // namespace refrain::server
