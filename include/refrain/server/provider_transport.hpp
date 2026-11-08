// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/services/transport.hpp>
#include <refrain/net/transport.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>

namespace refrain::server {

/// Adapts a `refrain:transport` provider (poll + event queue) to the callback
/// oriented `net::transport` interface used by the network server.
class provider_transport final : public net::transport {
  public:
    provider_transport(std::unique_ptr<services::transport::provider> provider,
                       net::transport_handler handler);
    ~provider_transport() override;

    provider_transport(const provider_transport &) = delete;
    auto operator=(const provider_transport &) -> provider_transport & = delete;

    [[nodiscard]] auto listen(const net::endpoint &bind) -> status override;
    [[nodiscard]] auto connect(const net::endpoint &target)
        -> result<std::uint64_t> override;
    void send(std::uint64_t connection,
              std::span<const std::byte> bytes) override;
    void close(std::uint64_t connection) override;
    void stop() noexcept override;
    [[nodiscard]] auto connection_count() const -> std::size_t override;
    [[nodiscard]] auto local_port() const -> std::uint16_t override;
    [[nodiscard]] auto descriptor() const -> net::transport_descriptor override;

  private:
    void poll_loop();

    std::unique_ptr<services::transport::provider> provider_;
    net::transport_handler handler_;
    std::atomic<bool> stopping_{false};
    std::thread poller_;
};

[[nodiscard]] auto
make_provider_transport(std::unique_ptr<services::transport::provider> provider,
                        net::transport_handler handler)
    -> std::unique_ptr<net::transport>;

} // namespace refrain::server
