// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace refrain::net {

enum class protocol_event : std::uint8_t {
    status_request = 0,
    login_request = 1,
    login_acknowledged = 2,
    configuration_acknowledged = 3,
};

/// Per-connection protocol codec used by the network session. The protocol
/// provider implements framing, compression and the connection cipher; the
/// session keeps packet semantics and the state machine.
class protocol_codec {
  public:
    protocol_codec() = default;
    protocol_codec(const protocol_codec &) = delete;
    auto operator=(const protocol_codec &) -> protocol_codec & = delete;
    virtual ~protocol_codec() = default;

    virtual void create_connection(std::uint64_t connection) = 0;
    virtual void close_connection(std::uint64_t connection) = 0;
    virtual auto set_compression(std::uint64_t connection,
                                 std::int32_t threshold) -> status = 0;
    virtual auto set_cipher(std::uint64_t connection,
                            std::span<const std::byte> key,
                            std::span<const std::byte> iv) -> status = 0;

    /// Feed transport bytes into the connection's frame decoder.
    virtual auto feed(std::uint64_t connection, std::span<const std::byte> data)
        -> status = 0;

    /// Decode one packet. `payload` borrows from the codec and is valid until
    /// the next call on this connection.
    virtual auto next_packet(std::uint64_t connection, std::uint32_t &packet_id,
                             std::span<const std::byte> &payload)
        -> result<bool> = 0;

    /// Encode one packet into the connection's outbound queue.
    virtual auto send_packet(std::uint64_t connection, std::uint32_t packet_id,
                             std::span<const std::byte> payload) -> status = 0;

    /// Encode one packet for several connections. The default encodes it once
    /// per connection; a codec whose framing does not depend on the
    /// connection may compress once and share the frame, which matters when
    /// the same body goes to every player in play.
    ///
    /// A broadcast is best-effort: a connection that has already gone away is
    /// skipped and the rest of the cohort still receives the packet.
    virtual auto send_shared_packet(std::span<const std::uint64_t> connections,
                                    std::uint32_t packet_id,
                                    std::span<const std::byte> payload)
        -> status {
        for (const auto connection : connections) {
            static_cast<void>(send_packet(connection, packet_id, payload));
        }
        return status_ok();
    }

    /// Pop one framed/encrypted chunk. `data` borrows from the codec and is
    /// valid until the next call on this connection.
    virtual auto take_outbound(std::uint64_t connection,
                               std::span<const std::byte> &data) -> bool = 0;

    virtual void notify_state(std::uint64_t connection,
                              connection_state state) = 0;

    /// Advance the provider-owned state machine for one event.
    virtual auto transition(std::uint64_t connection, protocol_event event)
        -> connection_state = 0;
};

} // namespace refrain::net
