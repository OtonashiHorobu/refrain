// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace refrain::session {

/// One viewport change the simulation should fill with chunk packets.
struct chunk_view_request {
    std::uint64_t connection{0};
    std::int32_t chunk_x{0};
    std::int32_t chunk_z{0};
    std::int32_t radius{0};
};

/// The look-ahead centre of one connection, derived from the chunk centres the
/// simulation accepted for it.
///
/// The estimate never reads a client-reported velocity: it uses only the
/// sequence of accepted viewport centres, which is the movement the simulation
/// already validated. A single centre change, a direction change or a viewport
/// that stopped changing does not produce a look-ahead, so a camera jittering
/// across a chunk border cannot make the generator chase it.
struct chunk_lookahead {
    /// The accepted chunk centre plus the whole-chunk offset below.
    std::int32_t chunk_x{0};
    std::int32_t chunk_z{0};
    /// Whole chunks ahead of the accepted centre; zero when inactive.
    std::int32_t offset_x{0};
    std::int32_t offset_z{0};
    /// Accepted centre velocity in chunks per second, per axis.
    double velocity_x{0.0};
    double velocity_z{0.0};
    /// Consecutive accepted centre changes that kept the same direction. The
    /// look-ahead activates once it reaches `activation_steps`.
    std::uint32_t heading_streak{0};
    /// True when the estimate is fresh and confident enough to pre-generate.
    bool active{false};
};

/// Session-owned chunk subscription tracking. The network layer no longer
/// decides when a player needs a new viewport; it only reports movement and
/// client view distance.
///
/// Transport callbacks update the tracker while the simulation drains it, so
/// every method takes the internal lock.
///
/// The tracker also estimates where a moving player's viewport is heading.
/// The accepted centre changes are the only input: chunk positions are
/// integers, so a walk across one border is one sample, and two consecutive
/// changes in the same direction are what turns an estimate into a look-ahead
/// centre. The estimate is a hint for generation admission and prefetch; the
/// set of columns a client is sent still follows its accepted viewport
/// exactly.
///
/// The estimate is a small state machine over the accepted centre changes:
///
/// - a change records one sample; the direction of the window (newest minus
///   oldest sample) is the heading, so a diagonal walk that crosses one axis
///   at a time still counts as one direction;
/// - the streak increments while the window keeps the same direction, and a
///   step against the established heading resets it to one on the same
///   update, so a reversal stops pre-generation at once instead of waiting
///   for the estimate to age out;
/// - the look-ahead activates only with a streak of `activation_steps`, a
///   sample span of `min_sample_span_ns` and an age below `stale_after_ns`, so
///   a single border crossing, a camera jittering across a border or a player
///   who stopped inside a chunk never produce one;
/// - the offset is the accepted velocity projected over `lookahead_ns`,
///   rounded up to whole chunks, dropped below `min_projected_chunks` and
///   capped at `max_lookahead_chunks`.
class chunk_tracker {
  public:
    /// Largest look-ahead offset, in chunks. The generator works in tiles, so
    /// a longer projection would ask for terrain several tiles beyond
    /// anything the accepted movement can reach within the projection time.
    static constexpr std::int32_t max_lookahead_chunks{2};

    /// Accepted movement over a shorter span than this is a lag correction or
    /// a teleport, not a heading, so it produces no velocity.
    static constexpr std::uint64_t min_sample_span_ns{100'000'000};

    /// Time the accepted velocity is projected over. Between half and one and
    /// a half seconds of walking: long enough to start the next tile before
    /// the client asks for it, short enough that a stopped player's estimate
    /// expires quickly.
    static constexpr std::uint64_t lookahead_ns{1'500'000'000};

    /// Accepted centre changes without a new one for this long mean the
    /// player stopped inside a chunk; the look-ahead collapses instead of
    /// pinning columns the player may never reach.
    static constexpr std::uint64_t stale_after_ns{3'000'000'000};

    /// Consecutive same-direction centre changes needed before the look-ahead
    /// activates.
    static constexpr std::uint32_t activation_steps{2};

    /// Speed below this is noise from corrections and does not project.
    static constexpr double min_speed_chunks_per_second{0.15};

    /// A projected distance of less than this many chunks rounds to no
    /// look-ahead, so a slowly drifting player does not pin a band forever.
    static constexpr double min_projected_chunks{0.25};

    /// Centre changes kept per connection, newest last. Four covers a
    /// diagonal stair step and a short stop without letting a stale sample
    /// decide the heading.
    static constexpr std::size_t history_size{4};

    /// Update one player's viewport. Returns true when the center or radius
    /// changed, or when the player has no view yet.
    auto update(std::uint64_t connection, std::int32_t chunk_x,
                std::int32_t chunk_z, std::int32_t radius) -> bool;

    /// Same, with the caller's clock. The runtime uses the four-argument form;
    /// a test passes time explicitly so the estimate is deterministic.
    auto update(std::uint64_t connection, std::int32_t chunk_x,
                std::int32_t chunk_z, std::int32_t radius, std::uint64_t now_ns)
        -> bool;

    /// Drain pending viewport requests.
    [[nodiscard]] auto take_requests() -> std::vector<chunk_view_request>;

    auto remove(std::uint64_t connection) -> bool;

    /// Look-ahead centre of one connection, evaluated at the current time.
    /// Inactive for an unknown connection, one that has not changed its centre
    /// in two consecutive same-direction steps, or one that stopped moving.
    [[nodiscard]] auto lookahead(std::uint64_t connection) const
        -> chunk_lookahead;

    /// Same, at the caller's clock.
    [[nodiscard]] auto lookahead(std::uint64_t connection,
                                 std::uint64_t now_ns) const -> chunk_lookahead;

  private:
    /// One accepted centre change.
    struct centre_sample {
        std::int32_t chunk_x{0};
        std::int32_t chunk_z{0};
        std::uint64_t at_ns{0};
    };

    struct view {
        std::int32_t chunk_x{0};
        std::int32_t chunk_z{0};
        std::int32_t radius{0};
        bool initialized{false};
        /// Accepted centre changes, oldest first, bounded by `history_size`.
        std::vector<centre_sample> samples{};
        /// Consecutive samples whose window displacement kept one direction.
        std::uint32_t heading_streak{0};
        /// Sign of the window displacement when the streak was last counted.
        std::int32_t heading_x{0};
        std::int32_t heading_z{0};
    };

    /// Record one accepted centre change and update the heading state machine.
    static void record_change(view &entry, std::int32_t chunk_x,
                              std::int32_t chunk_z, std::uint64_t now_ns);

    mutable std::mutex mutex_{};
    std::unordered_map<std::uint64_t, view> views_{};
    std::vector<chunk_view_request> pending_{};
};

} // namespace refrain::session
