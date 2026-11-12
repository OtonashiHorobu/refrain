// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Chunk pipeline: the background column generator and the per-connection
// streamer. The service applies finished columns to the world, tells the
// streamer they are ready and runs one streaming pass.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_streamer.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/session/chunk_tracker.hpp>
#include <refrain/sim/world.hpp>

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

    /// Apply the generated columns, report them to the streamer and stream one
    /// tick of the trackers' viewports.
    void tick(refrain::sim::world &world,
              refrain::session::chunk_tracker &views);

  private:
    std::unique_ptr<chunk_generation> generation_{};
    std::optional<chunk_streamer> streamer_{};
};

} // namespace refrain::server
