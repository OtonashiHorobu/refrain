// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/sim/ecs.hpp>
#include <refrain/world/model.hpp>

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::sim::detail {

/// Components of one runtime entity.
///
/// The set is deliberately small: the point-entity behaviour the simulation
/// runs today needs identity, transform, rotation and the flags a generated
/// entity carries. Gameplay components (health, AI, inventory, ...) are added
/// as their systems land, never as fields on one shared entity struct.
namespace component {

/// Logical id, the only entity identifier the rest of the engine sees, plus the
/// canonical type key.
struct identity {
    entity_id id{0};
    std::string type{};
};

/// Feet position, velocity and ground contact of a point entity.
struct transform {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double vx{0.0};
    double vy{0.0};
    double vz{0.0};
    bool on_ground{false};
};

/// Body rotation in degrees.
struct rotation {
    float yaw{0.0F};
    float pitch{0.0F};
};

/// Tag of an entity a loaded column carries. Such an entity keeps the position
/// the structure wrote; point-entity physics does not move it.
struct pinned {};

/// The loaded column that spawned the entity and owns its stored record.
struct column_origin {
    ::refrain::world::chunk_pos pos{};
};

/// The player records, stored as one component table inside the same registry
/// as the runtime entities. The table keeps its records in ascending logical id
/// order and draws ids from the store's shared sequence, so player registration
/// and entity spawn can never hand out the same id twice.
struct players {
    player_table table{};
};

} // namespace component

/// EnTT-backed entity storage and the ascending logical-id order over it.
///
/// The registry is the only entity storage. `order_` is the only iteration
/// order any caller may use; it is kept sorted by logical id, while the
/// component pools iterate in creation order and are never walked directly.
/// `by_id_` is the forward logical-id index, and the `identity` component is
/// the reverse one, so both directions of the mapping are available without a
/// second copy of the entity data.
///
/// The player table is a component of the same registry, and `ids_` is the one
/// logical-id sequence player registration and entity spawn both draw from.
/// Keeping the table in the registry is what lets a later change address a
/// player through its own components instead of a side table.
class entity_store {
  public:
    entity_store();
    entity_store(const entity_store &) = delete;
    auto operator=(const entity_store &) -> entity_store & = delete;
    /// A store is not movable: the player table component refers to the
    /// store's own id sequence, which a move would leave pointing into the
    /// moved-from object. Callers that need to move a world move its owning
    /// pointer instead.
    entity_store(entity_store &&) = delete;
    auto operator=(entity_store &&) -> entity_store & = delete;
    ~entity_store();

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return order_.size();
    }
    [[nodiscard]] auto empty() const noexcept -> bool { return order_.empty(); }

    /// Player records owned by the registry.
    [[nodiscard]] auto players() noexcept -> player_table &;
    [[nodiscard]] auto players() const noexcept -> const player_table &;

    /// Allocate the next logical id from the sequence player registration and
    /// entity spawn share. Fails when the signed 32-bit wire range is
    /// exhausted.
    [[nodiscard]] auto allocate_id() -> result<entity_id>;

    /// Handle of the live entity with `id`; `entt::null` when it is absent.
    [[nodiscard]] auto find(entity_id id) const noexcept -> entt::entity;

    /// Create one entity with the given components and append it to the order.
    /// A fresh logical id is always the highest live id, so the append keeps
    /// the order sorted. `column` marks an entity a loaded column owns.
    auto
    create(entity_id id, std::string type, double x, double y, double z,
           float yaw, float pitch, bool pinned,
           std::optional<::refrain::world::chunk_pos> column = std::nullopt)
        -> entt::entity;

    /// Destroy the entity with `id`, keeping the order sorted; false when the
    /// id is not live.
    auto destroy(entity_id id) -> bool;

    /// Destroy a batch in one pass over the order. Removing entries from a
    /// sorted list preserves the relative order, so no re-sort is needed.
    void destroy(std::span<const entity_id> ids) noexcept;

    /// Rebuild the order from the live identities and sort it by logical id.
    /// Used after a bulk load; the incremental spawn path does not need it.
    void rebuild_order();

    /// Components of the live entities; simulation systems read them by
    /// walking `order()`.
    [[nodiscard]] auto registry() noexcept -> entt::registry & {
        return registry_;
    }
    [[nodiscard]] auto registry() const noexcept -> const entt::registry & {
        return registry_;
    }
    [[nodiscard]] auto order() const noexcept
        -> const std::vector<entt::entity> & {
        return order_;
    }

    /// Flat projection of the entity at `index` in the order. The caller
    /// checks the index.
    [[nodiscard]] auto projection_at(std::size_t index) const -> entity_state;
    /// Flat projection of the entity with `id`, or nullopt when it is absent.
    [[nodiscard]] auto projection(entity_id id) const
        -> std::optional<entity_state>;

  private:
    /// One sequence for player registration and entity spawn. Declared before
    /// the registry so the player table component that refers to it is
    /// destroyed first.
    detail::id_sequence ids_{};
    entt::registry registry_{};
    /// Entity that carries the player table component.
    entt::entity players_entity_{entt::null};
    /// Live handles in ascending logical-id order. The only iteration order.
    std::vector<entt::entity> order_{};
    /// Logical id to handle. Only asked for lookups, never iterated.
    std::unordered_map<entity_id, entt::entity> by_id_{};
};

} // namespace refrain::sim::detail
