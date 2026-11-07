// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9582d232958dd1ff

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/packet.hpp>
#include <refrain/schema/protocol/777/login_clientbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>

namespace refrain::protocol::v777 {

/// Packets declared for this stage, ordered by flow and id.
inline constexpr std::array<packet_descriptor, 7> login_packets{{
  packet_descriptor{::refrain::net::connection_state::login, ::refrain::net::packet_flow::clientbound, 0x0, "disconnect"},
  packet_descriptor{::refrain::net::connection_state::login, ::refrain::net::packet_flow::clientbound, 0x1, "hello"},
  packet_descriptor{::refrain::net::connection_state::login, ::refrain::net::packet_flow::clientbound, 0x2, "login_finished"},
  packet_descriptor{::refrain::net::connection_state::login, ::refrain::net::packet_flow::clientbound, 0x3, "login_compression"},
  packet_descriptor{::refrain::net::connection_state::login, ::refrain::net::packet_flow::serverbound, 0x0, "hello"},
  packet_descriptor{::refrain::net::connection_state::login, ::refrain::net::packet_flow::serverbound, 0x1, "key"},
  packet_descriptor{::refrain::net::connection_state::login, ::refrain::net::packet_flow::serverbound, 0x3, "login_acknowledged"},
}};

} // namespace refrain::protocol::v777

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_HPP
