// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/net/base.hpp>
#include <refrain/net/protocol_codec.hpp>

#include <cstdint>
#include <memory>

namespace refrain::protocol {

/// Connection codec for Java protocol 777: VarInt framing, optional zlib
/// compression and AES-128-CFB8. Packet ids and semantics remain outside.
class java777_codec final : public net::protocol_codec {
  public:
    java777_codec();
    ~java777_codec() override;

    java777_codec(const java777_codec &) = delete;
    auto operator=(const java777_codec &) -> java777_codec & = delete;

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
    void notify_state(std::uint64_t connection,
                      net::connection_state state) override;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::protocol
