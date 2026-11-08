// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/net/status.hpp>
#include <refrain/net/transport.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace refrain::net {

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

  private:
    struct impl;
    explicit server(std::unique_ptr<impl> state);

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::net
