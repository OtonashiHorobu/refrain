// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Player visibility for the entity broadcast. Mirrors the vanilla tracker:
// a player sees another player when both share a world, the horizontal
// distance is inside the effective tracking range, and the entity is inside
// the viewer's chunk viewport.

#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace refrain::server {

/// Vanilla `EntityTypes.PLAYER` tracking range, in chunks.
inline constexpr std::int32_t player_tracking_chunks = 32;

/// Effective tracking radius in chunks: the entity type range clamped by the
/// viewer's own view distance. The visibility rule and the index share this
/// value so the candidate window can never be narrower than the rule.
[[nodiscard]] auto effective_tracking_chunks(std::int32_t tracking_chunks,
                                             std::int32_t view_distance)
    -> std::int32_t;

struct visibility_query {
    std::string_view world{};
    double x{0.0};
    double z{0.0};
    /// Viewer view distance in chunks; the tracker clamps the entity type
    /// range with it.
    std::int32_t view_distance{10};
};

/// Vanilla `ChunkMap.TrackedEntity.updatePlayer`: the horizontal distance must
/// not exceed `min(tracking_chunks, view_distance) * 16`, and the chunk of the
/// entity must be inside the viewer's viewport.
[[nodiscard]] auto player_visible(const visibility_query &viewer,
                                  std::string_view entity_world,
                                  double entity_x, double entity_z,
                                  std::int32_t tracking_chunks) -> bool;

/// One player in the visibility index, positioned by chunk.
struct visibility_entity {
    std::uint32_t entity_id{0};
    std::string_view world{};
    double x{0.0};
    double z{0.0};
    std::int32_t view_distance{0};
};

/// Chunk-bucket index over the players of one tick.
///
/// The index answers two questions: which entities share a chunk viewport with
/// a viewer, and which entities have a point inside their chunk viewport. It
/// only selects candidate pairs; `player_visible` still decides every pair, so
/// the distance and viewport tests keep one definition.
///
/// Buckets are keyed by world and chunk, and each bucket holds the snapshot
/// positions of the players in that chunk. A query walks only the chunk rows a
/// window covers and only the buckets that exist in those rows, so a player
/// alone in the world costs a handful of tree lookups instead of a scan over
/// every player. Every result is in ascending entity id, which requires the
/// snapshot passed to `rebuild` to be ordered that way.
///
/// Colocated players stay quadratic: N players in one chunk are one bucket
/// with N entries, so each query returns N candidates and N players cost N^2
/// pairs per tick. That is accepted; a chunk bucket cannot separate players
/// inside the chunk, and a finer grid would only shrink the constant of the
/// same worst case.
class player_visibility_index {
  public:
    explicit player_visibility_index(std::int32_t tracking_chunks);

    /// Rebuild the index for one tick. `entities` must be ordered by ascending
    /// entity id; every query result keeps that order.
    void rebuild(std::span<const visibility_entity> entities);

    /// Append the snapshot positions whose chunk is inside `viewer`'s chunk
    /// viewport, ascending entity id. The distance rule still decides whether
    /// a candidate is visible.
    void candidates(const visibility_query &viewer,
                    std::vector<std::size_t> &out) const;

    /// Append the snapshot positions inside the largest chunk viewport of any
    /// indexed player around the point (x, z) in `world`, ascending entity id.
    /// This is a superset of the viewers that can see the point; the caller
    /// still filters with its tracking state or the visibility rule.
    void viewers(std::string_view world, double x, double z,
                 std::vector<std::size_t> &out) const;

  private:
    /// Ordered by row first: a viewport window is a range of rows, and inside
    /// a row a range of columns.
    struct chunk_position {
        std::int64_t z{0};
        std::int64_t x{0};
        friend auto operator<=>(const chunk_position &,
                                const chunk_position &) = default;
    };

    std::int32_t tracking_chunks_{0};
    std::int32_t max_chunks_{0};
    std::map<std::string_view,
             std::map<chunk_position, std::vector<std::size_t>>>
        worlds_{};
};

} // namespace refrain::server
