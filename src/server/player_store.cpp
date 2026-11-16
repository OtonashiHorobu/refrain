// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Player store over a contract world port.
//
// The provider owns the authoritative player table. The runtime reads it once
// per tick as a projection (`player_count` plus the indexed accessor) and keeps
// the value copy in this store, so the per-packet path resolves a connection
// without an ABI call. Mutations are the connection-addressed provider calls
// the session already makes.
//
// The contract slice covers registration, removal, the movement transform, the
// teleport, the view distance and the relative move. Health, game mode, the
// ground flag and the inventory are separate surfaces and are named as gaps
// below instead of being applied to the cached copy only.

#include <refrain/server/player_store.hpp>

#include <refrain/core/log.hpp>
#include <refrain/sim/player.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_session{"session"};

class port_player_store final : public refrain::session::player_store {
  public:
    explicit port_player_store(refrain::server::world_port &world) noexcept
        : world_{&world} {}

    auto add(refrain::sim::player_table::registration record)
        -> refrain::result<refrain::sim::entity_id> override {
        const auto connection{record.connection};
        auto added{world_->add_player(std::move(record))};
        if (!added) {
            return std::unexpected{std::move(added.error())};
        }
        // The provider allocated the id; read the record it committed so the
        // cached copy starts from the authoritative fields rather than from
        // the request.
        auto projected{world_->player(connection)};
        if (!projected) {
            // The write landed but the record cannot be read back. Drop it, so
            // the provider and the session do not disagree about who is
            // connected, and report why.
            static_cast<void>(world_->remove_player(connection));
            return std::unexpected{std::move(projected.error())};
        }
        store(*projected);
        return *added;
    }

    auto remove(std::uint64_t connection) -> bool override {
        auto removed{world_->remove_player(connection)};
        if (!removed) {
            // A provider that rejects the call has no player surface left to
            // serve; report it once instead of pretending the player was
            // removed.
            log_session.warn("failed to remove the player of connection {}: {}",
                             connection, removed.error().message);
            return false;
        }
        erase(connection);
        return *removed;
    }

    auto find(std::uint64_t connection) const noexcept
        -> const refrain::sim::player_state * override {
        const auto found{by_connection_.find(connection)};
        if (found == by_connection_.end()) {
            return nullptr;
        }
        return find_entity(found->second);
    }

    auto find_entity(refrain::sim::entity_id id) const noexcept
        -> const refrain::sim::player_state * override {
        const auto found{records_.find(id)};
        return found == records_.end() ? nullptr : &found->second;
    }

    auto find_name(std::string_view name) const noexcept
        -> const refrain::sim::player_state * override {
        for (const auto &[id, record] : records_) {
            static_cast<void>(id);
            if (record.name == name) {
                return &record;
            }
        }
        return nullptr;
    }

    auto first() const noexcept -> const refrain::sim::player_state * override {
        return records_.empty() ? nullptr : &records_.begin()->second;
    }

    auto records() const noexcept
        -> const std::map<refrain::sim::entity_id, refrain::sim::player_state>
            & override {
        return records_;
    }

    auto size() const noexcept -> std::size_t override {
        return records_.size();
    }

    auto update_position(std::uint64_t connection, double x, double y, double z,
                         float yaw, float pitch) -> bool override {
        auto changed{
            world_->set_player_transform(connection, x, y, z, yaw, pitch)};
        if (!changed) {
            log_session.warn("failed to move the player of connection {}: {}",
                             connection, changed.error().message);
            return false;
        }
        auto *record{mutable_record(connection)};
        if (record != nullptr) {
            record->x = x;
            record->y = y;
            record->z = z;
            record->yaw = yaw;
            record->pitch = pitch;
        }
        return true;
    }

    auto set_on_ground(std::uint64_t, bool) -> bool override {
        // TODO: the contract has no ground-flag write; a composition reached
        // through it cannot apply the value and must not change the cached
        // copy, which the next projection refresh would overwrite anyway.
        return false;
    }

    auto set_view_distance(std::uint64_t connection, std::int32_t distance)
        -> bool override {
        auto changed{world_->set_player_view_distance(connection, distance)};
        if (!changed) {
            log_session.warn(
                "failed to set the view distance of connection {} to {}: {}",
                connection, distance, changed.error().message);
            return false;
        }
        if (auto *record{mutable_record(connection)}; record != nullptr) {
            record->view_distance = distance;
        }
        return true;
    }

    auto set_health(std::uint64_t, float) -> bool override {
        // TODO: damage is a separate contract surface; a provider-backed
        // session cannot apply a health change until the sim contract carries
        // one.
        return false;
    }

    auto set_game_mode(std::uint64_t, std::uint32_t) -> bool override {
        // TODO: the contract has no game-mode write; the projection reports
        // the mode the provider holds, and a console or command path that
        // changes it needs a provider call first.
        return false;
    }

    auto inventory_of(std::uint64_t)
        -> std::vector<refrain::sim::item_stack> * override {
        // TODO: item operations are a separate contract surface, so the
        // provider-backed projection carries no inventory. A caller that needs
        // one keeps using the in-process table, which this store does not
        // expose.
        return nullptr;
    }

    auto refresh() -> refrain::status override {
        auto count{world_->player_count()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        std::map<refrain::sim::entity_id, refrain::sim::player_state>
            refreshed{};
        std::unordered_map<std::uint64_t, refrain::sim::entity_id> index{};
        for (std::uint32_t position{0}; position < *count; ++position) {
            auto projected{world_->player_at(position)};
            if (!projected) {
                return std::unexpected{std::move(projected.error())};
            }
            const auto id{projected->entity_id};
            auto &record{refreshed[id]};
            record.id = id;
            record.connection = projected->connection;
            record.profile_id = projected->profile_id;
            record.name = std::move(projected->name);
            record.world = std::move(projected->world);
            record.x = projected->x;
            record.y = projected->y;
            record.z = projected->z;
            record.yaw = projected->yaw;
            record.pitch = projected->pitch;
            record.on_ground = projected->on_ground;
            record.game_mode = projected->game_mode;
            record.health = projected->health;
            record.view_distance = projected->view_distance;
            index.emplace(record.connection, id);
        }
        records_ = std::move(refreshed);
        by_connection_ = std::move(index);
        return refrain::status_ok();
    }

  private:
    /// Cache one projected record. The projection is a value, so this is the
    /// only place the runtime copies provider state.
    void store(const refrain::server::player_projection &projected) {
        auto &record{records_[projected.entity_id]};
        record.id = projected.entity_id;
        record.connection = projected.connection;
        record.profile_id = projected.profile_id;
        record.name = projected.name;
        record.world = projected.world;
        record.x = projected.x;
        record.y = projected.y;
        record.z = projected.z;
        record.yaw = projected.yaw;
        record.pitch = projected.pitch;
        record.on_ground = projected.on_ground;
        record.game_mode = projected.game_mode;
        record.health = projected.health;
        record.view_distance = projected.view_distance;
        by_connection_[projected.connection] = projected.entity_id;
    }

    void erase(std::uint64_t connection) {
        const auto found{by_connection_.find(connection)};
        if (found == by_connection_.end()) {
            return;
        }
        records_.erase(found->second);
        by_connection_.erase(found);
    }

    [[nodiscard]] auto mutable_record(std::uint64_t connection)
        -> refrain::sim::player_state * {
        const auto found{by_connection_.find(connection)};
        if (found == by_connection_.end()) {
            return nullptr;
        }
        const auto record{records_.find(found->second)};
        return record == records_.end() ? nullptr : &record->second;
    }

    refrain::server::world_port *world_{nullptr};
    std::map<refrain::sim::entity_id, refrain::sim::player_state> records_{};
    std::unordered_map<std::uint64_t, refrain::sim::entity_id> by_connection_{};
};

} // namespace

namespace refrain::server {

auto make_world_port_player_store(world_port &world)
    -> std::unique_ptr<session::player_store> {
    return std::make_unique<port_player_store>(world);
}

} // namespace refrain::server
