// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 26f951db567137ba

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/packet.hpp>
#include <refrain/schema/protocol/777/status_clientbound.hpp>
#include <refrain/schema/protocol/777/status_serverbound.hpp>

namespace refrain::protocol::v777 {

/// Packets declared for this stage, ordered by flow and id.
inline constexpr std::array<packet_descriptor, 4> status_packets{{
  packet_descriptor{::refrain::net::connection_state::status, ::refrain::net::packet_flow::clientbound, 0x0, "status_response"},
  packet_descriptor{::refrain::net::connection_state::status, ::refrain::net::packet_flow::clientbound, 0x1, "pong_response"},
  packet_descriptor{::refrain::net::connection_state::status, ::refrain::net::packet_flow::serverbound, 0x0, "status_request"},
  packet_descriptor{::refrain::net::connection_state::status, ::refrain::net::packet_flow::serverbound, 0x1, "ping_request"},
}};

} // namespace refrain::protocol::v777

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_HPP
