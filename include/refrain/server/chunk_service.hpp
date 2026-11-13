// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Chunk pipeline: the background column generator and the per-connection
// streamer. The service applies finished columns to the world, tells the
// streamer they are ready, runs one streaming pass and admits generation work
// for the next tick. Once per second it feeds the admission controller the
// tick p99, the queue depth and the resident memory.
//
// The service also owns the chunk residency policy. A column is resident while
// it is inside some connection's viewport or holds a ticket (spawn and forced
// columns); every other loaded column is unloaded at the end of the tick after
// its edits are flushed. The bound is therefore the union of the active
// viewport squares plus the ticket set, and it follows the view distances
// instead of growing with the distance a player has travelled. Unloading
// iterates the resident columns in ascending position order, so a replay
// unloads the same set at the same ticks.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_streamer.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/session/chunk_tracker.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace refrain::server {

class chunk_service {
  public:
    chunk_service();
    ~chunk_service();

    chunk_service(const chunk_service &) = delete;
    auto operator=(const chunk_service &) -> chunk_service & = delete;

    /// Start the background generator. A failure leaves the service without
    /// generation; streaming still works for columns the world already holds.
    [[nodiscard]] auto start_generation(chunk_generation::config settings)
        -> status;

    /// Attach the streamer once the listener and the protocol tables exist.
    /// `backlog` bounds what the stream may queue before it starves a client or
    /// pauses entirely; the service uses the same aggregate limit to stop
    /// admitting generation while the stream is paused.
    void attach(play_gateway &gateway, chunk_packet_source source,
                chunk_streamer::backlog_limits backlog);

    /// Same, with the default limits; delegating keeps the nested type out of a
    /// default argument.
    void attach(play_gateway &gateway, chunk_packet_source source) {
        attach(gateway, source, chunk_streamer::backlog_limits{});
    }

    /// Apply the generated columns, stream one tick of the trackers'
    /// viewports, rank the wanted columns by the player positions and admit
    /// what the credit level and the tick's remaining headroom allow.
    /// `tick_start_ns` is the start of the current tick; the admitted work is
    /// bounded by the part of the budget the tick has not used yet.
    void tick(world_port &world, refrain::session::chunk_tracker &views,
              std::uint64_t tick_start_ns);

    /// Record the work one tick used. Once per second this feeds the
    /// controller with the tick p99, the wanted depth and the resident memory.
    void observe_tick(std::uint64_t work_ns);

    /// Counters the runtime reports at boot and shutdown.
    struct residency_stats {
        /// Resident columns after the last residency pass.
        std::size_t resident_chunks{0};
        /// Highest resident count seen since construction.
        std::size_t peak_resident_chunks{0};
        /// Columns unloaded since construction.
        std::uint64_t unloaded_chunks{0};
    };

    [[nodiscard]] auto residency() const noexcept -> const residency_stats & {
        return residency_;
    }

    /// The attached streamer, null before `attach()`. A host may replace its
    /// encoder or read its counters.
    [[nodiscard]] auto streamer() noexcept -> chunk_streamer * {
        return streamer_ ? &*streamer_ : nullptr;
    }

    /// Resident set of this process in bytes, or zero when the host cannot
    /// report it.
    [[nodiscard]] static auto process_resident_bytes() -> std::uint64_t;

    /// Install the barrier that runs before a residency pass unloads dirty
    /// columns. The runtime passes its snapshot writer's `drain()`: an older
    /// in-flight snapshot of a column must land before the unload flush, never
    /// after it, or it could overwrite the newer edits on disk.
    void set_unload_barrier(std::function<void()> barrier) {
        unload_barrier_ = std::move(barrier);
    }

  private:
    /// Pin the columns of every current viewport, release the pins of columns
    /// that left every viewport, then unload every resident column without a
    /// ticket. The pass walks the resident columns in ascending position order
    /// and writes at most `max_dirty_unloads_per_tick` dirty ones, so one long
    /// tick cannot turn a batch of arrivals into a longer one.
    void enforce_residency(world_port &world);

    /// Limits shared by the streamer and the generator pause.
    chunk_streamer::backlog_limits backlog_{};
    /// Controller update period and the tick work window its p99 covers.
    static constexpr std::size_t controller_window_ticks{20};

    struct chunk_order {
        [[nodiscard]] auto
        operator()(const sim::chunk_pos &left,
                   const sim::chunk_pos &right) const noexcept -> bool {
            if (left.x != right.x) {
                return left.x < right.x;
            }
            return left.z < right.z;
        }
    };

    std::unique_ptr<chunk_generation> generation_{};
    std::optional<chunk_streamer> streamer_{};
    play_gateway *gateway_{nullptr};
    /// The viewport set the pins were last synchronised with. Pins are only
    /// rebuilt when this changes, so a still camera costs nothing per tick.
    std::vector<chunk_streamer::viewport> viewports_{};
    /// Columns pinned on behalf of the current viewports, one ticket each.
    std::set<sim::chunk_pos, chunk_order> view_tickets_{};
    /// Dirty columns one residency pass may flush. A batch larger than this
    /// stays resident for the next pass, which keeps a tile arriving from
    /// turning into a long tick; the generation rate is below the bound, so the
    /// backlog drains instead of growing.
    static constexpr std::size_t max_dirty_unloads_per_tick{4};
    residency_stats residency_{};
    std::function<void()> unload_barrier_{};
    std::array<std::uint64_t, controller_window_ticks> tick_samples_{};
    std::size_t tick_sample_next_{0};
    std::uint32_t ticks_since_update_{0};
};

} // namespace refrain::server
