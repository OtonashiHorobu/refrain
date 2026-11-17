// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/chunk_tracker.hpp>

#include <refrain/core/time.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace refrain::session {

namespace {

/// Sign of one displacement component, as a direction.
[[nodiscard]] auto sign_of(std::int32_t value) -> std::int32_t {
    return value > 0 ? 1 : (value < 0 ? -1 : 0);
}

/// Project one axis velocity onto whole chunks. A distance below
/// `min_projected_chunks` is treated as standing still, and the result is
/// capped so a teleport correction cannot ask for terrain tiles away.
[[nodiscard]] auto lookahead_offset(double velocity, std::uint64_t horizon_ns)
    -> std::int32_t {
    const auto projected{velocity * static_cast<double>(horizon_ns) /
                         1'000'000'000.0};
    if (std::abs(projected) < chunk_tracker::min_projected_chunks) {
        return 0;
    }
    const auto chunks{
        static_cast<std::int32_t>(std::ceil(std::abs(projected)))};
    const auto bounded(std::clamp(chunks, std::int32_t{1},
                                  chunk_tracker::max_lookahead_chunks));
    return projected > 0.0 ? bounded : -bounded;
}

} // namespace

auto chunk_tracker::update(std::uint64_t connection, std::int32_t chunk_x,
                           std::int32_t chunk_z, std::int32_t radius) -> bool {
    return update(connection, chunk_x, chunk_z, radius,
                  static_cast<std::uint64_t>(refrain::core::now_ns()));
}

auto chunk_tracker::update(std::uint64_t connection, std::int32_t chunk_x,
                           std::int32_t chunk_z, std::int32_t radius,
                           std::uint64_t now_ns) -> bool {
    if (connection == 0 || radius < 0) {
        return false;
    }
    const std::lock_guard lock{mutex_};
    auto &entry{views_[connection]};
    if (entry.initialized && entry.chunk_x == chunk_x &&
        entry.chunk_z == chunk_z && entry.radius == radius) {
        return false;
    }
    const auto centre_changed{!entry.initialized || entry.chunk_x != chunk_x ||
                              entry.chunk_z != chunk_z};
    entry.chunk_x = chunk_x;
    entry.chunk_z = chunk_z;
    entry.radius = radius;
    entry.initialized = true;
    if (centre_changed) {
        record_change(entry, chunk_x, chunk_z, now_ns);
    }
    pending_.push_back({.connection = connection,
                        .chunk_x = chunk_x,
                        .chunk_z = chunk_z,
                        .radius = radius});
    return true;
}

void chunk_tracker::record_change(view &entry, std::int32_t chunk_x,
                                  std::int32_t chunk_z, std::uint64_t now_ns) {
    if (entry.samples.empty()) {
        entry.samples.push_back(
            {.chunk_x = chunk_x, .chunk_z = chunk_z, .at_ns = now_ns});
        entry.heading_streak = 0;
        entry.heading_x = 0;
        entry.heading_z = 0;
        return;
    }
    const auto &previous{entry.samples.back()};
    entry.samples.push_back(
        {.chunk_x = chunk_x, .chunk_z = chunk_z, .at_ns = now_ns});
    if (entry.samples.size() > history_size) {
        entry.samples.erase(entry.samples.begin());
    }
    const auto step_x{chunk_x - previous.chunk_x};
    const auto step_z{chunk_z - previous.chunk_z};
    // The direction of the whole window, not just the last step: a diagonal
    // walk crosses one axis at a time, and its window displacement keeps the
    // same sign while the player keeps going the same way.
    const auto &oldest{entry.samples.front()};
    const auto window_x{sign_of(chunk_x - oldest.chunk_x)};
    const auto window_z{sign_of(chunk_z - oldest.chunk_z)};
    const auto reversed_axis = [](std::int32_t step, std::int32_t heading) {
        return step != 0 && heading != 0 && sign_of(step) != heading;
    };
    if (reversed_axis(step_x, entry.heading_x) ||
        reversed_axis(step_z, entry.heading_z)) {
        // A step against the established heading collapses the estimate on the
        // same update instead of waiting for it to expire: a player who turns
        // around must stop prefetching the old direction at once.
        entry.heading_streak = 1;
    } else if (window_x == entry.heading_x && window_z == entry.heading_z &&
               (window_x != 0 || window_z != 0)) {
        ++entry.heading_streak;
    } else {
        entry.heading_streak = 1;
    }
    entry.heading_x = window_x;
    entry.heading_z = window_z;
}

auto chunk_tracker::take_requests() -> std::vector<chunk_view_request> {
    const std::lock_guard lock{mutex_};
    return std::exchange(pending_, {});
}

auto chunk_tracker::remove(std::uint64_t connection) -> bool {
    const std::lock_guard lock{mutex_};
    return views_.erase(connection) != 0;
}

auto chunk_tracker::lookahead(std::uint64_t connection) const
    -> chunk_lookahead {
    return lookahead(connection,
                     static_cast<std::uint64_t>(refrain::core::now_ns()));
}

auto chunk_tracker::lookahead(std::uint64_t connection,
                              std::uint64_t now_ns) const -> chunk_lookahead {
    const std::lock_guard lock{mutex_};
    const auto found{views_.find(connection)};
    if (found == views_.end() || !found->second.initialized) {
        return {};
    }
    const auto &entry{found->second};
    chunk_lookahead result{
        .chunk_x = entry.chunk_x,
        .chunk_z = entry.chunk_z,
        .heading_streak = entry.heading_streak,
    };
    if (entry.samples.size() < 2 || entry.heading_streak < activation_steps) {
        return result;
    }
    const auto &oldest{entry.samples.front()};
    const auto &newest{entry.samples.back()};
    if (newest.at_ns <= oldest.at_ns ||
        newest.at_ns - oldest.at_ns < min_sample_span_ns) {
        return result;
    }
    // The sample clock can be ahead of the caller's clock in a test that
    // injects time; clamps keep the estimate meaningful either way.
    if (now_ns > newest.at_ns && now_ns - newest.at_ns > stale_after_ns) {
        return result;
    }
    const auto span_ns{static_cast<double>(newest.at_ns - oldest.at_ns)};
    const auto velocity_x{static_cast<double>(newest.chunk_x - oldest.chunk_x) *
                          1'000'000'000.0 / span_ns};
    const auto velocity_z{static_cast<double>(newest.chunk_z - oldest.chunk_z) *
                          1'000'000'000.0 / span_ns};
    result.velocity_x = velocity_x;
    result.velocity_z = velocity_z;
    if (std::max(std::abs(velocity_x), std::abs(velocity_z)) <
        min_speed_chunks_per_second) {
        return result;
    }
    result.offset_x = lookahead_offset(velocity_x, lookahead_ns);
    result.offset_z = lookahead_offset(velocity_z, lookahead_ns);
    if (result.offset_x == 0 && result.offset_z == 0) {
        return result;
    }
    result.chunk_x = entry.chunk_x + result.offset_x;
    result.chunk_z = entry.chunk_z + result.offset_z;
    result.active = true;
    return result;
}

} // namespace refrain::session
