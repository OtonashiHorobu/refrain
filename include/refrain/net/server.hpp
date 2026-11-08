// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/net/status.hpp>
#include <refrain/net/transport.hpp>
#include <refrain/net/wire.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace refrain::net {

/// Profile returned by an injected session verifier (online mode tests).
struct session_profile {
    uuid id{};
    std::string name{};
};

using session_verifier = std::function<result<session_profile>(
    std::string_view name, std::string_view server_hash)>;

/// One request to (re)center a connection's chunk view. The runtime owns the
/// world and encodes the missing chunks; the network layer only reports the
/// viewport and sends the resulting batches.
struct chunk_view_request {
    std::uint64_t connection{0};
    std::int32_t chunk_x{0};
    std::int32_t chunk_z{0};
    std::int32_t radius{0};
    /// Already-sent square around the center (spawn chunks); the runtime seeds
    /// its known set with it so those chunks are not resent.
    std::int32_t seed_radius{0};
};

/// Runtime values the network layer cannot take from configuration: world
/// spawn state, pre-encoded chunks and world callbacks.
struct server_inputs {
    double spawn_x{0.0};
    double spawn_y{64.0};
    double spawn_z{0.0};
    std::int64_t seed{0};
    std::int32_t game_mode{1};
    bool hardcore{false};
    /// Generated registry/tag catalog (JSON). Empty disables registry sync.
    std::string registry_catalog_path{};
    /// Pre-encoded `level_chunk_with_light` bodies around the spawn point.
    std::shared_ptr<const std::vector<std::vector<std::byte>>> spawn_chunks{};
    /// Called on the simulation thread when a player destroys a block.
    std::function<bool(std::int32_t, std::int32_t, std::int32_t)> break_block{};
    /// Called on the simulation thread for a block placement.
    std::function<bool(std::int32_t, std::int32_t, std::int32_t)> place_block{};
    /// Optional online-mode session verifier. When set, the server does not
    /// create the HTTP session client, which makes online login testable
    /// without the Mojang service.
    session_verifier verify_session{};
};

/// Minimal protocol server. It currently answers the status/list ping and
/// rejects login with a disconnect; the login/configuration/play state machine
/// is added on top of the same transport and connection table.
class server {
  public:
    /// Start with runtime inputs and the `network` namespace view. The view is
    /// read once; later configuration changes require a restart unless the
    /// host re-applies them.
    [[nodiscard]] static auto start(server_inputs inputs,
                                    core::config::view config)
        -> result<std::unique_ptr<server>>;
    ~server();

    server(const server &) = delete;
    auto operator=(const server &) -> server & = delete;

    void stop() noexcept;
    /// Drain packets that arrived since the previous call. Must be called on
    /// the simulation thread; login/configuration handling stays on the I/O
    /// thread, play packets are applied here.
    void tick();
    [[nodiscard]] auto local_port() const -> std::uint16_t;

    /// Drain viewport changes observed since the previous call.
    [[nodiscard]] auto take_chunk_view_requests()
        -> std::vector<chunk_view_request>;

    /// Send one chunk batch (bodies include their packet id). `update_center`
    /// also emits `set_chunk_cache_center`; an empty batch with a center
    /// update is valid.
    void send_chunk_batch(std::uint64_t connection, std::int32_t chunk_x,
                          std::int32_t chunk_z,
                          std::span<const std::vector<std::byte>> packets,
                          bool update_center);

    /// Connections currently in play (closed connections excluded).
    [[nodiscard]] auto active_connections() const -> std::vector<std::uint64_t>;

    /// Send a full container snapshot (player inventory uses container 0).
    void send_container_content(std::uint64_t connection,
                                std::int32_t container_id,
                                std::int32_t state_id,
                                std::span<const item_stack> items,
                                const item_stack &carried);

  private:
    struct impl;
    explicit server(std::unique_ptr<impl> state);

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::net
