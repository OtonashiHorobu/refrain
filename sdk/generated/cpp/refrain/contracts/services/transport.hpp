// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: c51c38b6cd952844

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_TRANSPORT_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_TRANSPORT_HPP

#include <refrain/contracts/services/transport_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::transport {

enum class transport_error : std::uint32_t { // enum 'transport_error'
    invalid_argument = 1,
    not_found = 2,
    already_exists = 3,
    permission_denied = 4,
    io_error = 5,
    out_of_memory = 6,
    unsupported = 7,
    closed = 8,
};

/// Carrying protocol. A protocol provider declares the subset it accepts.
enum class transport_protocol : std::uint32_t { // enum 'transport_protocol'
    tcp = 0,
    tls_tcp = 1,
    raknet = 2,
    unix_stream = 3,
    memory_stream = 4,
    memory_message = 5,
    custom = 6,
};

enum class transport_event_kind : std::uint32_t { // enum 'transport_event_kind'
    opened = 1,
    data = 2,
    closed = 3,
    error = 4,
};

using transport_endpoint = ::refrain_services_transport_transport_endpoint;
/// Zero-initialized `transport_endpoint` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_transport_endpoint() noexcept -> transport_endpoint {
    transport_endpoint value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using transport_descriptor = ::refrain_services_transport_transport_descriptor;
/// Zero-initialized `transport_descriptor` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_transport_descriptor() noexcept -> transport_descriptor {
    transport_descriptor value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// One queued transport event. `data` is borrowed from the provider and stays
/// valid until the next poll() call on that provider instance.
using transport_event = ::refrain_services_transport_transport_event;
/// Zero-initialized `transport_event` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_transport_event() noexcept -> transport_event {
    transport_event value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(transport_endpoint) == 32);
static_assert(alignof(transport_endpoint) == 8);
static_assert(offsetof(transport_endpoint, struct_size) == 0);
static_assert(offsetof(transport_endpoint, host) == 8);
static_assert(offsetof(transport_endpoint, port) == 24);
static_assert(sizeof(transport_descriptor) == 40);
static_assert(alignof(transport_descriptor) == 8);
static_assert(offsetof(transport_descriptor, struct_size) == 0);
static_assert(offsetof(transport_descriptor, provider) == 8);
static_assert(offsetof(transport_descriptor, protocol) == 24);
static_assert(offsetof(transport_descriptor, reliable) == 28);
static_assert(offsetof(transport_descriptor, ordered) == 29);
static_assert(offsetof(transport_descriptor, message_oriented) == 30);
static_assert(offsetof(transport_descriptor, mtu) == 32);
static_assert(offsetof(transport_descriptor, backpressure) == 36);
static_assert(sizeof(transport_event) == 48);
static_assert(alignof(transport_event) == 8);
static_assert(offsetof(transport_event, struct_size) == 0);
static_assert(offsetof(transport_event, kind) == 4);
static_assert(offsetof(transport_event, connection) == 8);
static_assert(offsetof(transport_event, data) == 16);
static_assert(offsetof(transport_event, error) == 32);

inline constexpr std::string_view service_id = "refrain:transport";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::worker;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::instanced;
inline constexpr std::string_view capabilities = "stream,message,reliable";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:transport"};

/// Type-safe wrapper around `refrain_services_transport_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_transport_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_transport_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    auto descriptor(::refrain::services::transport::transport_descriptor &value) const -> void {
        ::refrain::contracts::init_struct(value);
        table_->descriptor(self_, &value);
    }

    [[nodiscard]] auto listen(const ::refrain::services::transport::transport_endpoint &address) const -> std::expected<void, ::refrain::services::transport::transport_error> {
        const auto abi_address = ::refrain::contracts::with_struct_size(address);
        if (const auto code = table_->listen(self_, &abi_address); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::transport::transport_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto connect(const ::refrain::services::transport::transport_endpoint &address) const -> std::expected<std::uint64_t, ::refrain::services::transport::transport_error> {
        const auto abi_address = ::refrain::contracts::with_struct_size(address);
        ::uint64_t value{};
        if (const auto code = table_->connect(self_, &abi_address, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::transport::transport_error>(code));
        }
        return static_cast<std::uint64_t>(value);
    }

    [[nodiscard]] auto send(std::uint64_t connection, std::span<const std::uint8_t> data) const -> std::expected<void, ::refrain::services::transport::transport_error> {
        if (const auto code = table_->send(self_, connection, ::refrain::contracts::to_borrowed(data)); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::transport::transport_error>(code));
        }
        return {};
    }

    auto close(std::uint64_t connection) const -> void {
        table_->close(self_, connection);
    }

    /// Blocking poll with a bounded timeout. Returns false on timeout.
    /// Provides at most one event per call.
    [[nodiscard]] auto poll(std::uint32_t timeout_ms, ::refrain::services::transport::transport_event &event) const -> std::expected<bool, ::refrain::services::transport::transport_error> {
        ::refrain::contracts::init_struct(event);
        ::uint8_t value{};
        if (const auto code = table_->poll(self_, timeout_ms, &event, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::transport::transport_error>(code));
        }
        return (value != 0);
    }

    [[nodiscard]] auto local_port() const -> std::uint16_t {
        return static_cast<std::uint16_t>(table_->local_port(self_));
    }

    [[nodiscard]] auto connection_count() const -> std::uint64_t {
        return static_cast<std::uint64_t>(table_->connection_count(self_));
    }

    auto stop() const -> void {
        table_->stop(self_);
    }

  private:
    const ::refrain_services_transport_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::transport

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_TRANSPORT_HPP
