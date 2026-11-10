// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/provider_transport.hpp>

#include <refrain/contracts/abi.hpp>
#include <refrain/core/log.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace refrain::server {

namespace {

[[nodiscard]] auto to_protocol(services::transport::transport_protocol protocol)
    -> net::transport_protocol {
    switch (protocol) {
    case services::transport::transport_protocol::tcp:
        return net::transport_protocol::tcp;
    case services::transport::transport_protocol::tls_tcp:
        return net::transport_protocol::tls_tcp;
    case services::transport::transport_protocol::raknet:
        return net::transport_protocol::raknet;
    case services::transport::transport_protocol::unix_stream:
        return net::transport_protocol::unix_stream;
    case services::transport::transport_protocol::memory_stream:
        return net::transport_protocol::memory_stream;
    case services::transport::transport_protocol::memory_message:
    case services::transport::transport_protocol::custom:
        break;
    }
    return net::transport_protocol::custom;
}

[[nodiscard]] auto transport_endpoint(const net::endpoint &endpoint)
    -> services::transport::transport_endpoint {
    auto out{services::transport::make_transport_endpoint()};
    out.host = contracts::to_borrowed(endpoint.host);
    out.port = endpoint.port;
    return out;
}

} // namespace

provider_transport::provider_transport(
    std::unique_ptr<services::transport::provider> provider,
    net::transport_handler handler)
    : provider_{std::move(provider)}, handler_{std::move(handler)} {
    poller_ = std::thread{[this] {
        log::set_thread_name("net-transport");
        poll_loop();
    }};
}

provider_transport::~provider_transport() { stop(); }

auto provider_transport::listen(const net::endpoint &bind) -> status {
    const auto endpoint{transport_endpoint(bind)};
    auto listened{provider_->listen(endpoint)};
    if (!listened) {
        return std::unexpected{
            make_error(error_code::io_error, "transport listen failed")};
    }
    return status_ok();
}

auto provider_transport::connect(const net::endpoint &target)
    -> result<std::uint64_t> {
    const auto endpoint{transport_endpoint(target)};
    auto connected{provider_->connect(endpoint)};
    if (!connected) {
        return std::unexpected{
            make_error(error_code::io_error, "transport connect failed")};
    }
    return *connected;
}

void provider_transport::send(std::uint64_t connection,
                              std::span<const std::byte> bytes) {
    static_cast<void>(provider_->send(
        connection, std::span<const std::uint8_t>{
                        reinterpret_cast<const std::uint8_t *>(bytes.data()),
                        bytes.size()}));
}

void provider_transport::close(std::uint64_t connection) {
    provider_->close(connection);
}

void provider_transport::stop() noexcept {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    provider_->stop();
    if (poller_.joinable()) {
        poller_.join();
    }
}

auto provider_transport::connection_count() const -> std::size_t {
    return static_cast<std::size_t>(provider_->connection_count());
}

auto provider_transport::local_port() const -> std::uint16_t {
    return provider_->local_port();
}

auto provider_transport::descriptor() const -> net::transport_descriptor {
    auto raw{services::transport::make_transport_descriptor()};
    provider_->descriptor(raw);
    return net::transport_descriptor{
        .provider = std::string{contracts::as_string_view(raw.provider)},
        .protocol = to_protocol(
            static_cast<services::transport::transport_protocol>(raw.protocol)),
        .reliable = raw.reliable != 0,
        .ordered = raw.ordered != 0,
        .message_oriented = raw.message_oriented != 0,
        .mtu = raw.mtu,
        .backpressure = raw.backpressure != 0,
    };
}

void provider_transport::poll_loop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        auto event{services::transport::make_transport_event()};
        auto result{provider_->poll(50, event)};
        if (!result) {
            if (handler_.on_error) {
                handler_.on_error("transport poll failed");
            }
            break;
        }
        if (!*result) {
            continue;
        }
        switch (static_cast<services::transport::transport_event_kind>(
            event.kind)) {
        case services::transport::transport_event_kind::opened:
            if (handler_.on_open) {
                handler_.on_open(event.connection);
            }
            break;
        case services::transport::transport_event_kind::data: {
            const auto bytes{contracts::as_bytes(event.data)};
            if (handler_.on_data) {
                handler_.on_data(
                    event.connection,
                    std::span<const std::byte>{
                        reinterpret_cast<const std::byte *>(bytes.data()),
                        bytes.size()});
            }
            break;
        }
        case services::transport::transport_event_kind::closed:
            if (handler_.on_close) {
                handler_.on_close(event.connection,
                                  contracts::as_string_view(event.error));
            }
            break;
        case services::transport::transport_event_kind::error:
            if (handler_.on_error) {
                handler_.on_error(contracts::as_string_view(event.error));
            }
            break;
        }
    }
}

auto make_provider_transport(
    std::unique_ptr<services::transport::provider> provider,
    net::transport_handler handler) -> std::unique_ptr<net::transport> {
    return std::make_unique<provider_transport>(std::move(provider),
                                                std::move(handler));
}

} // namespace refrain::server
