// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 959e5112fb541051

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_HPP

#include <refrain/contracts/services/net_protocol_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/contracts/core.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::net_protocol {

enum class protocol_error : std::uint32_t { // enum 'protocol_error'
    frame_too_large = 1,
    malformed_packet = 2,
    unknown_packet = 3,
    unsupported_version = 4,
    compression_failed = 5,
};

enum class connection_state : std::uint32_t { // enum 'connection_state'
    handshake = 0,
    status = 1,
    login = 2,
    configuration = 3,
    play = 4,
};

using protocol_version = ::refrain_services_net_protocol_protocol_version;
/// Zero-initialized `protocol_version` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_protocol_version() noexcept -> protocol_version {
    protocol_version value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using frame_header = ::refrain_services_net_protocol_frame_header;
/// Zero-initialized `frame_header` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_frame_header() noexcept -> frame_header {
    frame_header value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Borrowed packet payload; valid for the duration of the contract call.
using packet_view = ::refrain_services_net_protocol_packet_view;
/// Zero-initialized `packet_view` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_packet_view() noexcept -> packet_view {
    packet_view value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(protocol_version) == 24);
static_assert(alignof(protocol_version) == 8);
static_assert(offsetof(protocol_version, struct_size) == 0);
static_assert(offsetof(protocol_version, protocol) == 4);
static_assert(offsetof(protocol_version, name) == 8);
static_assert(sizeof(frame_header) == 12);
static_assert(alignof(frame_header) == 4);
static_assert(offsetof(frame_header, struct_size) == 0);
static_assert(offsetof(frame_header, length) == 4);
static_assert(offsetof(frame_header, uncompressed_length) == 8);
static_assert(sizeof(packet_view) == 24);
static_assert(alignof(packet_view) == 8);
static_assert(offsetof(packet_view, struct_size) == 0);
static_assert(offsetof(packet_view, packet_id) == 4);
static_assert(offsetof(packet_view, payload) == 8);

inline constexpr std::string_view service_id = "refrain:net.protocol";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::net;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "frame-compression,registry-sync";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:net.protocol"};

/// Type-safe wrapper around `refrain_services_net_protocol_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_net_protocol_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_net_protocol_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Protocol number implemented by this provider (777 for vanilla 26.3).
    [[nodiscard]] auto protocol_id() const -> std::uint32_t {
        return static_cast<std::uint32_t>(table_->protocol_id(self_));
    }

    [[nodiscard]] auto accepts_version(std::uint32_t protocol) const -> bool {
        return (table_->accepts_version(self_, protocol) != 0);
    }

    /// Decode one packet from a connection's inbound buffer.
    [[nodiscard]] auto next_packet(::refrain::core::connection conn, ::refrain::services::net_protocol::packet_view &packet) const -> std::expected<::refrain::services::net_protocol::frame_header, ::refrain::services::net_protocol::protocol_error> {
        ::refrain::contracts::init_struct(packet);
        ::refrain_services_net_protocol_frame_header value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->next_packet(self_, conn.raw(), &packet, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return value;
    }

    /// Encode and enqueue one packet on the connection.
    [[nodiscard]] auto send_packet(::refrain::core::connection conn, const ::refrain::services::net_protocol::packet_view &packet) const -> std::expected<void, ::refrain::services::net_protocol::protocol_error> {
        const auto abi_packet = ::refrain::contracts::with_struct_size(packet);
        if (const auto code = table_->send_packet(self_, conn.raw(), &abi_packet); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto state_of(::refrain::core::connection conn) const -> ::refrain::services::net_protocol::connection_state {
        return static_cast<::refrain::services::net_protocol::connection_state>(table_->state_of(self_, conn.raw()));
    }

  private:
    const ::refrain_services_net_protocol_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::net_protocol

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_HPP
