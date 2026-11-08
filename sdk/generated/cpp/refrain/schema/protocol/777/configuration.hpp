// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8aa1fe7cac60c1a3

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/packet.hpp>
#include <refrain/schema/protocol/777/configuration_clientbound.hpp>
#include <refrain/schema/protocol/777/configuration_serverbound.hpp>

namespace refrain::protocol::v777 {

/// Packets declared for this stage, ordered by flow and id.
inline constexpr std::array<packet_descriptor, 31> configuration_packets{{
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x0, "cookie_request"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x1, "custom_payload"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x2, "disconnect"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x3, "finish_configuration"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x4, "keep_alive"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x5, "ping"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x6, "reset_chat"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x7, "registry_data"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x8, "resource_pack_pop"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x9, "resource_pack_push"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0xA, "post_effects"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0xB, "store_cookie"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0xC, "transfer"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0xD, "update_enabled_features"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0xE, "update_tags"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0xF, "select_known_packs"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x10, "custom_report_details"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x11, "server_links"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x12, "clear_dialog"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x13, "show_dialog"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::clientbound, 0x14, "code_of_conduct"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x0, "client_information"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x1, "cookie_response"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x2, "custom_payload"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x3, "finish_configuration"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x4, "keep_alive"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x5, "pong"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x6, "resource_pack"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x7, "select_known_packs"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x8, "custom_click_action"},
  packet_descriptor{::refrain::net::connection_state::configuration, ::refrain::net::packet_flow::serverbound, 0x9, "accept_code_of_conduct"},
}};

} // namespace refrain::protocol::v777

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_HPP
