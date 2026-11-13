// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Outbound surface of the play connections the runtime subsystems drive. A
// subsystem owns gameplay state and encoding; the gateway owns connections,
// backpressure and transport. `net::server` is one implementation
// (`server_gateway`); a unit test supplies a fake and drives the same `tick()`
// without a socket.

#pragma once

#include <refrain/net/server.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace refrain::server {

/// One play-connection outbox shared by the tick-loop subsystems.
class play_gateway {
  public:
    play_gateway() = default;
    play_gateway(const play_gateway &) = delete;
    auto operator=(const play_gateway &) -> play_gateway & = delete;
    virtual ~play_gateway() = default;

    /// False when no listener is attached. Send operations are then no-ops
    /// and the input drains return no events.
    [[nodiscard]] virtual auto available() const -> bool = 0;

    /// Send an already encoded packet body (packet id included).
    virtual void send_raw(std::uint64_t connection,
                          std::span<const std::byte> body) = 0;

    /// Send one already encoded body to several connections. The default
    /// sends it once per connection; a live gateway shares the encoded frame
    /// so a cohort announcement is compressed once.
    virtual void send_raw_many(std::span<const std::uint64_t> connections,
                               std::span<const std::byte> body) {
        for (const auto connection : connections) {
            send_raw(connection, body);
        }
    }

    /// Connections currently in play (closed connections excluded).
    [[nodiscard]] virtual auto active_connections() const
        -> std::vector<std::uint64_t> = 0;

    /// Bytes still queued for one connection.
    [[nodiscard]] virtual auto backlog(std::uint64_t connection) const
        -> std::size_t = 0;

    /// Send one chunk batch (bodies include their packet id) and optionally
    /// move the client's chunk cache centre.
    virtual void
    send_chunk_batch(std::uint64_t connection, std::int32_t chunk_x,
                     std::int32_t chunk_z,
                     std::span<const std::vector<std::byte>> packets,
                     bool update_center) = 0;

    /// Tell the client to drop one chunk from its cache.
    virtual void send_forget_chunk(std::uint64_t connection,
                                   std::int32_t chunk_x,
                                   std::int32_t chunk_z) = 0;

    [[nodiscard]] virtual auto take_creative_slots()
        -> std::vector<refrain::net::creative_slot_event> = 0;
    [[nodiscard]] virtual auto take_container_closes()
        -> std::vector<refrain::net::container_close_event> = 0;
    [[nodiscard]] virtual auto take_container_clicks()
        -> std::vector<refrain::net::container_click_event> = 0;

    virtual void
    send_container_content(std::uint64_t connection, std::int32_t container_id,
                           std::int32_t state_id,
                           std::span<const refrain::net::item_stack> items,
                           const refrain::net::item_stack &carried) = 0;
    virtual void
    send_container_set_slot(std::uint64_t connection, std::int32_t container_id,
                            std::int32_t state_id, std::int16_t slot,
                            const refrain::net::item_stack &item) = 0;
    virtual void send_cursor_item(std::uint64_t connection,
                                  const refrain::net::item_stack &item) = 0;
};

/// `play_gateway` over a live `net::server`.
///
/// The listener may be attached after construction so the tick-loop services
/// can be built before the socket is bound. Without a listener every method is
/// a no-op, matching the runtime's "listener failed, keep ticking" path.
class server_gateway final : public play_gateway {
  public:
    server_gateway() = default;
    explicit server_gateway(refrain::net::server *listener);

    void reset(refrain::net::server *listener);

    [[nodiscard]] auto available() const -> bool override;

    void send_raw(std::uint64_t connection,
                  std::span<const std::byte> body) override;
    void send_raw_many(std::span<const std::uint64_t> connections,
                       std::span<const std::byte> body) override;

    [[nodiscard]] auto active_connections() const
        -> std::vector<std::uint64_t> override;
    [[nodiscard]] auto backlog(std::uint64_t connection) const
        -> std::size_t override;

    void send_chunk_batch(std::uint64_t connection, std::int32_t chunk_x,
                          std::int32_t chunk_z,
                          std::span<const std::vector<std::byte>> packets,
                          bool update_center) override;
    void send_forget_chunk(std::uint64_t connection, std::int32_t chunk_x,
                           std::int32_t chunk_z) override;

    [[nodiscard]] auto take_creative_slots()
        -> std::vector<refrain::net::creative_slot_event> override;
    [[nodiscard]] auto take_container_closes()
        -> std::vector<refrain::net::container_close_event> override;
    [[nodiscard]] auto take_container_clicks()
        -> std::vector<refrain::net::container_click_event> override;

    void
    send_container_content(std::uint64_t connection, std::int32_t container_id,
                           std::int32_t state_id,
                           std::span<const refrain::net::item_stack> items,
                           const refrain::net::item_stack &carried) override;
    void send_container_set_slot(std::uint64_t connection,
                                 std::int32_t container_id,
                                 std::int32_t state_id, std::int16_t slot,
                                 const refrain::net::item_stack &item) override;
    void send_cursor_item(std::uint64_t connection,
                          const refrain::net::item_stack &item) override;

  private:
    refrain::net::server *listener_{nullptr};
};

} // namespace refrain::server
