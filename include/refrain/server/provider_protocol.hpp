// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/services/net_protocol.hpp>
#include <refrain/net/protocol_codec.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace refrain::server {

/// Adapts a `refrain:net.protocol` provider to the narrow codec interface the
/// network session uses. Packet semantics and the state machine remain outside
/// the provider.
class provider_protocol final : public net::protocol_codec {
  public:
    explicit provider_protocol(
        std::unique_ptr<services::net_protocol::provider> provider);

    void create_connection(std::uint64_t connection) override;
    void close_connection(std::uint64_t connection) override;
    [[nodiscard]] auto set_compression(std::uint64_t connection,
                                       std::int32_t threshold)
        -> status override;
    [[nodiscard]] auto set_cipher(std::uint64_t connection,
                                  std::span<const std::byte> key,
                                  std::span<const std::byte> iv)
        -> status override;
    [[nodiscard]] auto feed(std::uint64_t connection,
                            std::span<const std::byte> data) -> status override;
    [[nodiscard]] auto next_packet(std::uint64_t connection,
                                   std::uint32_t &packet_id,
                                   std::span<const std::byte> &payload)
        -> result<bool> override;
    [[nodiscard]] auto send_packet(std::uint64_t connection,
                                   std::uint32_t packet_id,
                                   std::span<const std::byte> payload)
        -> status override;
    [[nodiscard]] auto take_outbound(std::uint64_t connection,
                                     std::span<const std::byte> &data)
        -> bool override;

  private:
    std::unique_ptr<services::net_protocol::provider> provider_;
};

[[nodiscard]] auto make_provider_protocol(
    std::unique_ptr<services::net_protocol::provider> provider)
    -> std::unique_ptr<net::protocol_codec>;

} // namespace refrain::server
