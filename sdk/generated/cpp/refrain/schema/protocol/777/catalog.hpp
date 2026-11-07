// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: a05c72a592e0c44c

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CATALOG_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CATALOG_HPP

#include <cstdint>
#include <string_view>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/configuration.hpp>
#include <refrain/schema/protocol/777/handshake.hpp>
#include <refrain/schema/protocol/777/login.hpp>
#include <refrain/schema/protocol/777/play.hpp>
#include <refrain/schema/protocol/777/status.hpp>

namespace refrain::protocol::v777 {

/// Packet table lookup across every stage of the protocol.
[[nodiscard]] constexpr auto find_packet(::refrain::net::connection_state state,
                                      ::refrain::net::packet_flow flow,
                                      std::uint16_t id) noexcept
    -> const packet_descriptor * {
  switch (state) {
  case ::refrain::net::connection_state::configuration:
    return ::refrain::protocol::v777::find_packet(configuration_packets, flow, id);
  case ::refrain::net::connection_state::handshake:
    return ::refrain::protocol::v777::find_packet(handshake_packets, flow, id);
  case ::refrain::net::connection_state::login:
    return ::refrain::protocol::v777::find_packet(login_packets, flow, id);
  case ::refrain::net::connection_state::play:
    return ::refrain::protocol::v777::find_packet(play_packets, flow, id);
  case ::refrain::net::connection_state::status:
    return ::refrain::protocol::v777::find_packet(status_packets, flow, id);
  }
  return nullptr;
}

[[nodiscard]] constexpr auto find_packet(::refrain::net::connection_state state,
                                      ::refrain::net::packet_flow flow,
                                      std::string_view name) noexcept
    -> const packet_descriptor * {
  switch (state) {
  case ::refrain::net::connection_state::configuration:
    return ::refrain::protocol::v777::find_packet(configuration_packets, flow, name);
  case ::refrain::net::connection_state::handshake:
    return ::refrain::protocol::v777::find_packet(handshake_packets, flow, name);
  case ::refrain::net::connection_state::login:
    return ::refrain::protocol::v777::find_packet(login_packets, flow, name);
  case ::refrain::net::connection_state::play:
    return ::refrain::protocol::v777::find_packet(play_packets, flow, name);
  case ::refrain::net::connection_state::status:
    return ::refrain::protocol::v777::find_packet(status_packets, flow, name);
  }
  return nullptr;
}

} // namespace refrain::protocol::v777

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CATALOG_HPP
