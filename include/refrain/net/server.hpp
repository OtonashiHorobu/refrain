// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/status.hpp>
#include <refrain/net/transport.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace refrain::net {

struct server_config {
    endpoint bind{};
    status_info status{};
    /// Compression threshold advertised during login. The status phase always
    /// stays uncompressed.
    int compression_threshold{256};
    std::int32_t view_distance{10};
    std::int32_t simulation_distance{10};
    bool online_mode{false};
    bool hardcore{false};
    std::int32_t game_mode{1};
    double spawn_x{0.0};
    double spawn_y{64.0};
    double spawn_z{0.0};
    std::int64_t seed{0};
    /// Generated registry/tag catalog (JSON). Empty disables the registry
    /// sync and is only useful for protocol tests that stop before play.
    std::string registry_catalog_path{};
    /// Pre-encoded `level_chunk_with_light` bodies around the spawn point.
    /// They are produced by the simulation thread before the listener starts,
    /// so the network thread never reads mutable world state.
    std::shared_ptr<const std::vector<std::vector<std::byte>>> spawn_chunks{};
    /// Called on the simulation thread when a player destroys a block. Returns
    /// true when the world accepted the change.
    std::function<bool(std::int32_t, std::int32_t, std::int32_t)> break_block{};
    /// Called on the simulation thread for a block placement. The server
    /// currently places a plain stone block; inventory-driven placement lands
    /// with the item stack codec.
    std::function<bool(std::int32_t, std::int32_t, std::int32_t)> place_block{};

    /// Session server used by online-mode `hasJoined`.
    std::string session_server{"https://sessionserver.mojang.com"};
    std::int32_t auth_timeout_ms{5000};
};

/// Minimal protocol server. It currently answers the status/list ping and
/// rejects login with a disconnect; the login/configuration/play state machine
/// is added on top of the same transport and connection table.
class server {
  public:
    [[nodiscard]] static auto start(server_config config)
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

  private:
    struct impl;
    explicit server(std::unique_ptr<impl> state);

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::net
