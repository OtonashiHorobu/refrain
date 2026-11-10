// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:transport` provider backed by the standalone Asio TCP transport.
// Inbound callbacks are copied into an event queue; protocol/session code
// polls the queue from its own thread.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/transport_abi.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/net/transport.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using refrain::contracts::as_bytes;
using refrain::contracts::as_string_view;

enum class queued_kind : std::uint32_t {
    opened = REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_OPENED,
    data = REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_DATA,
    closed = REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_CLOSED,
    error = REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_ERROR,
};

struct queued_event {
    queued_kind kind{queued_kind::opened};
    std::uint64_t connection{0};
    std::vector<std::byte> data{};
    std::string error{};
};

struct asio_transport_provider {
    std::unique_ptr<refrain::net::transport> transport{};
    std::mutex mutex{};
    std::condition_variable ready{};
    std::deque<queued_event> queue{};
    queued_event current{};
    bool stopping{false};

    void enqueue(queued_event event) {
        const std::lock_guard lock{mutex};
        if (stopping) {
            return;
        }
        queue.push_back(std::move(event));
        ready.notify_one();
    }
};

void provider_descriptor(void *self,
                         refrain_services_transport_transport_descriptor *out) {
    static_cast<void>(self);
    if (out == nullptr) {
        return;
    }
    out->struct_size = REFRAIN_SERVICES_TRANSPORT_TRANSPORT_DESCRIPTOR_SIZE;
    static constexpr std::string_view provider{"refrain.transport.asio.tcp"};
    out->provider = refrain::contracts::to_borrowed(provider);
    out->protocol = REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_TCP;
    out->reliable = 1;
    out->ordered = 1;
    out->message_oriented = 0;
    out->mtu = 0;
    out->backpressure = 0;
}

auto ensure_transport(asio_transport_provider &state)
    -> refrain::result<refrain::net::transport *> {
    if (state.transport) {
        return state.transport.get();
    }
    refrain::net::transport_handler handler{
        .on_open =
            [&state](std::uint64_t connection) {
                state.enqueue(
                    {.kind = queued_kind::opened, .connection = connection});
            },
        .on_data =
            [&state](std::uint64_t connection,
                     std::span<const std::byte> data) {
                queued_event event{.kind = queued_kind::data,
                                   .connection = connection};
                event.data.assign(data.begin(), data.end());
                state.enqueue(std::move(event));
            },
        .on_close =
            [&state](std::uint64_t connection, std::string_view reason) {
                state.enqueue({.kind = queued_kind::closed,
                               .connection = connection,
                               .error = std::string{reason}});
            },
        .on_error =
            [&state](std::string_view reason) {
                state.enqueue(
                    {.kind = queued_kind::error, .error = std::string{reason}});
            },
    };
    auto opened{refrain::net::transport::open(std::move(handler), 1)};
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    state.transport = std::move(*opened);
    return state.transport.get();
}

auto provider_listen(
    void *self, const refrain_services_transport_transport_endpoint *address)
    -> refrain_status {
    if (self == nullptr || address == nullptr) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    auto transport{ensure_transport(state)};
    if (!transport) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_IO_ERROR;
    }
    const refrain::net::endpoint endpoint{
        .host = std::string{as_string_view(address->host)},
        .port = address->port,
    };
    if (auto listened{(*transport)->listen(endpoint)}; !listened) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_IO_ERROR;
    }
    return REFRAIN_STATUS_OK;
}

auto provider_connect(
    void *self, const refrain_services_transport_transport_endpoint *address,
    std::uint64_t *out) -> refrain_status {
    if (self == nullptr || address == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    auto transport{ensure_transport(state)};
    if (!transport) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_IO_ERROR;
    }
    const refrain::net::endpoint endpoint{
        .host = std::string{as_string_view(address->host)},
        .port = address->port,
    };
    auto connection{(*transport)->connect(endpoint)};
    if (!connection) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_IO_ERROR;
    }
    *out = *connection;
    return REFRAIN_STATUS_OK;
}

auto provider_send(void *self, std::uint64_t connection, refrain_bytes data)
    -> refrain_status {
    if (self == nullptr) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    if (!state.transport) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_CLOSED;
    }
    const auto bytes{as_bytes(data)};
    state.transport->send(
        connection,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
    return REFRAIN_STATUS_OK;
}

void provider_close(void *self, std::uint64_t connection) {
    if (self == nullptr) {
        return;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    if (state.transport) {
        state.transport->close(connection);
    }
}

auto provider_poll(void *self, std::uint32_t timeout_ms,
                   refrain_services_transport_transport_event *out,
                   std::uint8_t *result) -> refrain_status {
    if (self == nullptr || out == nullptr || result == nullptr) {
        return REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    std::unique_lock lock{state.mutex};
    state.ready.wait_for(lock, std::chrono::milliseconds{timeout_ms}, [&state] {
        return state.stopping || !state.queue.empty();
    });
    if (state.stopping || state.queue.empty()) {
        *result = 0;
        return REFRAIN_STATUS_OK;
    }
    state.current = std::move(state.queue.front());
    state.queue.pop_front();
    out->struct_size = REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_SIZE;
    out->kind = static_cast<std::uint32_t>(state.current.kind);
    out->connection = state.current.connection;
    out->data = refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(state.current.data.data()),
        state.current.data.size()});
    out->error = refrain::contracts::to_borrowed(state.current.error);
    *result = 1;
    return REFRAIN_STATUS_OK;
}

auto provider_local_port(void *self) -> std::uint16_t {
    if (self == nullptr) {
        return 0;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    return state.transport ? state.transport->local_port() : 0;
}

auto provider_connection_count(void *self) -> std::uint64_t {
    if (self == nullptr) {
        return 0;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    return state.transport
               ? static_cast<std::uint64_t>(state.transport->connection_count())
               : 0;
}

void provider_stop(void *self) {
    if (self == nullptr) {
        return;
    }
    auto &state{*static_cast<asio_transport_provider *>(self)};
    {
        const std::lock_guard lock{state.mutex};
        state.stopping = true;
    }
    state.ready.notify_all();
    if (state.transport) {
        state.transport->stop();
    }
}

refrain_services_transport_vtable make_vtable() {
    return refrain_services_transport_vtable{
        .struct_size = REFRAIN_SERVICES_TRANSPORT_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_TRANSPORT_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_TRANSPORT_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_TRANSPORT_ABI_PATCH,
        .descriptor = &provider_descriptor,
        .listen = &provider_listen,
        .connect = &provider_connect,
        .send = &provider_send,
        .close = &provider_close,
        .poll = &provider_poll,
        .local_port = &provider_local_port,
        .connection_count = &provider_connection_count,
        .stop = &provider_stop,
    };
}

auto make_asio_transport(refrain::mod::host &host) -> refrain::status {
    static std::unique_ptr<asio_transport_provider> provider{};
    static const auto vtable{make_vtable()};
    provider = std::make_unique<asio_transport_provider>();
    const refrain::mod::provider_api api{
        .vtable = &vtable,
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_TRANSPORT_ABI_MAJOR,
    };
    auto status{
        host.register_provider("refrain:transport", "asio_tcp", api, "1.0.0")};
    if (!status) {
        provider.reset();
    }
    return status;
}

} // namespace

REFR_MOD_STATIC("refrain.transport.asio.tcp", "1.0.0", 1, make_asio_transport);
