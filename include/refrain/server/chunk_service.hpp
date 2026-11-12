// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Chunk pipeline: the background column generator and the per-connection
// streamer. The service applies finished columns to the world, tells the
// streamer they are ready, runs one streaming pass and admits generation work
// for the next tick. Once per second it feeds the admission controller the
// tick p99, the queue depth and the resident memory.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_streamer.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/session/chunk_tracker.hpp>
#include <refrain/sim/world.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

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
    void attach(play_gateway &gateway, chunk_packet_source source);

    /// Apply the generated columns, stream one tick of the trackers'
    /// viewports, rank the wanted columns by the player positions and admit
    /// what the credit level and the tick's remaining headroom allow.
    /// `tick_start_ns` is the start of the current tick; the admitted work is
    /// bounded by the part of the budget the tick has not used yet.
    void tick(refrain::sim::world &world,
              refrain::session::chunk_tracker &views,
              std::uint64_t tick_start_ns);

    /// Record the work one tick used. Once per second this feeds the
    /// controller with the tick p99, the wanted depth and the resident memory.
    void observe_tick(std::uint64_t work_ns);

  private:
    /// Controller update period and the tick work window its p99 covers.
    static constexpr std::size_t controller_window_ticks{20};

    std::unique_ptr<chunk_generation> generation_{};
    std::optional<chunk_streamer> streamer_{};
    play_gateway *gateway_{nullptr};
    std::array<std::uint64_t, controller_window_ticks> tick_samples_{};
    std::size_t tick_sample_next_{0};
    std::uint32_t ticks_since_update_{0};
};

} // namespace refrain::server
