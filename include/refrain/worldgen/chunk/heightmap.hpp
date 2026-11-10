// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The six heightmaps a generated chunk keeps. A heightmap stores, for every
// column, the first y above the highest state that matches its predicate; an
// empty column stores `min_y`. Updates and priming follow the reference
// rescan rules, including the downward walk when the current top stops
// matching.

#pragma once

#include <refrain/worldgen/chunk/block_state.hpp>

#include <array>
#include <cstdint>
#include <functional>

namespace refrain::worldgen {

/// Heightmap kinds, in registration order.
enum class heightmap_kind : std::uint8_t {
    world_surface_wg = 0,
    world_surface = 1,
    ocean_floor_wg = 2,
    ocean_floor = 3,
    motion_blocking = 4,
    motion_blocking_no_leaves = 5,
};

inline constexpr std::size_t heightmap_kind_count = 6;

/// `true` when `kind` counts `traits` as the first available block above it.
[[nodiscard]] auto matches(heightmap_kind kind, const state_traits &traits)
    -> bool;

/// The chunk access a heightmap needs while rescanning a column.
struct heightmap_source {
    std::function<std::uint16_t(jint x, jint y, jint z)> state_at{};
    std::function<state_traits(std::uint16_t state)> traits_of{};
};

/// One heightmap for a 16x16 chunk column set. Coordinates are chunk-local.
class heightmap {
  public:
    heightmap() = default;
    heightmap(heightmap_kind kind, jint min_y, jint height);

    /// Observes one block write. `state` must describe the
    /// block now present at `(x, y, z)`.
    void update(jint x, jint y, jint z, std::uint16_t state,
                const heightmap_source &source);

    /// Recomputes every column from the chunk.
    void prime(const heightmap_source &source);

    /// Raw first-available y (the highest matching y plus one, or `min_y`).
    [[nodiscard]] auto first_available(jint x, jint z) const -> jint;
    /// Raw highest matching y, or `min_y - 1` for an empty column.
    [[nodiscard]] auto highest_taken(jint x, jint z) const -> jint;

    [[nodiscard]] auto kind() const noexcept -> heightmap_kind { return kind_; }
    [[nodiscard]] auto min_y() const noexcept -> jint { return min_y_; }
    [[nodiscard]] auto height() const noexcept -> jint { return height_; }
    [[nodiscard]] auto empty() const noexcept -> bool;

    /// Overwrites one column; used when loading a persisted chunk.
    void set_first_available(jint x, jint z, jint value);
    void clear();

  private:
    [[nodiscard]] static auto index(jint x, jint z) -> std::size_t {
        return static_cast<std::size_t>(z) * 16U + static_cast<std::size_t>(x);
    }
    [[nodiscard]] auto matches_state(std::uint16_t state,
                                     const heightmap_source &source) const
        -> bool {
        return matches(kind_, source.traits_of(state));
    }

    heightmap_kind kind_{heightmap_kind::world_surface};
    jint min_y_{0};
    jint height_{0};
    std::array<jint, 256> first_available_{};
};

} // namespace refrain::worldgen
