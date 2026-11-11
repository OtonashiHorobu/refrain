// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace refrain::net {

/// Carrying protocol of one transport provider. `memory_stream` is a test
/// transport that preserves stream semantics without a socket.
enum class transport_protocol : std::uint8_t {
    tcp = 0,
    tls_tcp = 1,
    raknet = 2,
    unix_stream = 3,
    memory_stream = 4,
    custom = 5,
};

/// Capabilities a protocol provider can require before accepting a transport.
struct transport_descriptor {
    std::string provider{};
    transport_protocol protocol{transport_protocol::tcp};
    bool reliable{true};
    bool ordered{true};
    bool message_oriented{false};
    std::uint32_t mtu{0};
    bool backpressure{false};
};

struct endpoint {
    std::string host{"0.0.0.0"};
    std::uint16_t port{25565};
};

/// Callbacks run on the transport's I/O thread. `on_data` receives a borrowed
/// view valid only for the duration of the call; the receiver copies what it
/// keeps. `on_close` runs exactly once per connection.
struct transport_handler {
    std::function<void(std::uint64_t)> on_open{};
    std::function<void(std::uint64_t, std::span<const std::byte>)> on_data{};
    std::function<void(std::uint64_t, std::string_view)> on_close{};
    /// Listener/resolver failures that are not tied to one connection.
    std::function<void(std::string_view)> on_error{};
};

/// TCP transport built on standalone Asio. The implementation owns one
/// `io_context` and its worker threads; public calls are thread-safe and never
/// block on the I/O thread. Asio types stay in `src/net/transport_asio.cpp`.
class transport {
  public:
    [[nodiscard]] static auto open(transport_handler handler,
                                   std::size_t io_threads = 1)
        -> result<std::unique_ptr<transport>>;

    transport() = default;
    transport(const transport &) = delete;
    auto operator=(const transport &) -> transport & = delete;
    virtual ~transport() = default;

    /// Bind and accept on `endpoint.host:endpoint.port`. Port 0 asks the
    /// kernel for one; `local_port()` reports the bound value.
    [[nodiscard]] virtual auto listen(const endpoint &bind) -> status = 0;

    /// Start an outbound connection. Returns the connection id immediately;
    /// `on_open` fires after the TCP handshake and `on_close` on failure.
    [[nodiscard]] virtual auto connect(const endpoint &target)
        -> result<std::uint64_t> = 0;

    /// Copy `bytes` into the connection's write queue. A closed or unknown id
    /// is ignored.
    virtual void send(std::uint64_t connection,
                      std::span<const std::byte> bytes) = 0;

    /// Close one connection; `on_close` fires once.
    virtual void close(std::uint64_t connection) = 0;

    /// Close the listener and every connection, stop the I/O threads and join
    /// them. Must not be called from a transport callback.
    virtual void stop() noexcept = 0;

    /// Bytes still queued for the connection. A transport without an internal
    /// queue reports zero. Callers use it to skip optional updates for a
    /// client that is behind.
    [[nodiscard]] virtual auto backlog(std::uint64_t /*connection*/) const
        -> std::size_t {
        return 0;
    }

    [[nodiscard]] virtual auto connection_count() const -> std::size_t = 0;

    /// Bound listener port (0 when not listening).
    [[nodiscard]] virtual auto local_port() const -> std::uint16_t = 0;

    /// Static description used by the host to match transport capabilities
    /// with protocol requirements.
    [[nodiscard]] virtual auto descriptor() const -> transport_descriptor = 0;
};

} // namespace refrain::net
