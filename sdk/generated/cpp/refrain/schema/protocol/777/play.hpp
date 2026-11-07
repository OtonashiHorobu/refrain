// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9582d232958dd1ff

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/packet.hpp>
#include <refrain/schema/protocol/777/play_clientbound.hpp>
#include <refrain/schema/protocol/777/play_serverbound.hpp>

namespace refrain::protocol::v777 {

/// Packets declared for this stage, ordered by flow and id.
inline constexpr std::array<packet_descriptor, 29> play_packets{{
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x4, "block_changed_ack"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x8, "block_update"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0xA, "change_difficulty"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0xB, "chunk_batch_finished"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0xC, "chunk_batch_start"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x22, "entity_event"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x27, "game_event"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x2C, "initialize_border"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x2D, "keep_alive"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x32, "login"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x41, "player_abilities"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x49, "player_position"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x63, "set_default_spawn_position"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x6B, "set_held_slot"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::clientbound, 0x73, "set_time"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x0, "accept_teleportation"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x7, "chat_command"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x9, "chat"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0xB, "chunk_batch_received"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0xE, "client_information"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x1C, "keep_alive"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x1E, "move_player_pos"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x1F, "move_player_pos_rot"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x20, "move_player_rot"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x21, "move_player_status_only"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x29, "player_action"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x2C, "player_loaded"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x36, "set_carried_item"},
  packet_descriptor{::refrain::net::connection_state::play, ::refrain::net::packet_flow::serverbound, 0x42, "use_item_on"},
}};

} // namespace refrain::protocol::v777

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_HPP
