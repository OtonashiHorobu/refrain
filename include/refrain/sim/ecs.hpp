// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/sim/player.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace refrain::sim {

namespace detail {
class entity_store;
} // namespace detail

/// Flat read projection of one runtime entity.
///
/// The simulation stores an entity as a set of components; this struct is the
/// form the rest of the runtime reads, so a caller that is not a simulation
/// system never sees a storage handle. The fields mirror the component values,
/// and a value returned by `entity_view` is a snapshot: it does not stay in
/// sync with later ticks.
struct entity_state {
    entity_id id{0};
    std::string type{};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double vx{0.0};
    double vy{0.0};
    double vz{0.0};
    /// Body rotation in degrees, as the column record stores it.
    float yaw{0.0F};
    float pitch{0.0F};
    bool on_ground{false};
    /// True for an entity a loaded column carries. The build has no entity AI
    /// or living-entity physics, so a generated entity stays where the
    /// structure placed it instead of falling; it is spawned when its column
    /// loads and removed when the column unloads.
    bool pinned{false};
};

/// Ascending logical-id view over the live runtime entities.
///
/// This is the only supported way to read the entity set: component pools
/// iterate in creation order, and the state hash, the save path and the
/// clientbound entity stream must see ascending logical ids instead. A spawn
/// appends because a new logical id is always the highest, a despawn removes
/// its entry, and adopting or reloading a column rebuilds and sorts the order.
///
/// The view borrows the entity store of the world it came from: it is valid
/// while that world is alive. It is not a snapshot, so a later spawn or
/// despawn is reflected by the next call.
class entity_view {
  public:
    entity_view() noexcept = default;

    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto empty() const noexcept -> bool;

    /// Projection of the entity at `index` in ascending logical-id order, or
    /// nullopt past the end.
    [[nodiscard]] auto at(std::size_t index) const
        -> std::optional<entity_state>;

    /// Projection of the entity with `id`, or nullopt when it is not live.
    [[nodiscard]] auto find(entity_id id) const -> std::optional<entity_state>;

    /// Visit every live entity in ascending logical-id order. The callback
    /// receives a projection by value; use `find()` for a single lookup.
    template <typename F> void each(F &&visit) const {
        const auto count{size()};
        for (std::size_t index{0}; index < count; ++index) {
            if (auto state{at(index)}) {
                visit(*state);
            }
        }
    }

  private:
    friend class world;

    explicit entity_view(const detail::entity_store *store) noexcept
        : store_{store} {}

    const detail::entity_store *store_{nullptr};
};

} // namespace refrain::sim
