// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/net/base.hpp>
#include <refrain/net/protocol_codec.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace refrain::protocol {

/// Connection codec for Java protocol 777: VarInt framing, optional zlib
/// compression and AES-128-CFB8. Packet ids and semantics remain outside.
///
/// The codec is shared by every connection that the network session accepts.
/// Its connection table is sharded, and each connection's framing, cipher and
/// queue sit behind that connection's own lock, so encoding a packet for one
/// connection does not wait for a packet of another.
class java777_codec final : public net::protocol_codec {
  public:
    /// Test seam: invoked inside `send_packet` with the connection id, while
    /// that connection's encode lock is held and before the packet is framed.
    /// A test uses it to park one connection's encode and observe that another
    /// connection's encode completes meanwhile, with no wall-clock race. The
    /// server never installs one.
    using encode_probe = std::function<void(std::uint64_t connection)>;

    java777_codec();
    explicit java777_codec(encode_probe probe);
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
    [[nodiscard]] auto
    send_shared_packet(std::span<const std::uint64_t> connections,
                       std::uint32_t packet_id,
                       std::span<const std::byte> payload) -> status override;
    [[nodiscard]] auto take_outbound(std::uint64_t connection,
                                     std::span<const std::byte> &data)
        -> bool override;
    void notify_state(std::uint64_t connection,
                      net::connection_state state) override;
    [[nodiscard]] auto transition(std::uint64_t connection,
                                  net::protocol_event event)
        -> net::connection_state override;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::protocol
