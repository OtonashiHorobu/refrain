// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 37ad437ddc6c0e0a

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_HANDSHAKE_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_HANDSHAKE_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/packet.hpp>
#include <refrain/schema/protocol/777/handshake_serverbound.hpp>

namespace refrain::protocol::v777 {

/// Packets declared for this stage, ordered by flow and id.
inline constexpr std::array<packet_descriptor, 1> handshake_packets{{
  packet_descriptor{::refrain::net::connection_state::handshake, ::refrain::net::packet_flow::serverbound, 0x0, "intention"},
}};

} // namespace refrain::protocol::v777

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_HANDSHAKE_HPP
