// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: d4f6267a5f946179

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_SERVERBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_SERVERBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/schema/protocol/777/common.hpp>
#include <refrain/schema/protocol/777/wire.hpp>

namespace refrain::protocol::v777::play::serverbound {

struct accept_teleportation {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "accept_teleportation";

    // attributes: #[varint]
    std::uint32_t id;
    double x;
    double y;
    double z;
    float y_rot;
    float x_rot;
};

struct chat_command {
    static constexpr std::uint16_t packet_id = 0x7;
    static constexpr std::string_view packet_name = "chat_command";

    // attributes: #[max(256)]
    std::string command;
};

struct chat {
    static constexpr std::uint16_t packet_id = 0x9;
    static constexpr std::string_view packet_name = "chat";

    // attributes: #[max(256)]
    std::string message;
    std::int64_t timestamp;
    std::int64_t salt;
    // attributes: #[fixed_bytes(256)]
    std::optional<std::vector<std::byte>> signature;
    // attributes: #[varint]
    std::uint32_t offset;
    // attributes: #[fixed_bytes(20)]
    std::vector<std::byte> acknowledged;
    // attributes: #[fixed]
    std::int8_t checksum;
};

struct chunk_batch_received {
    static constexpr std::uint16_t packet_id = 0xB;
    static constexpr std::string_view packet_name = "chunk_batch_received";

    float desired_chunks;
};

struct client_information {
    static constexpr std::uint16_t packet_id = 0xE;
    static constexpr std::string_view packet_name = "client_information";

    ::refrain::protocol::v777::common::client_information_data information;
};

struct keep_alive {
    static constexpr std::uint16_t packet_id = 0x1C;
    static constexpr std::string_view packet_name = "keep_alive";

    std::int64_t id;
};

struct move_player_pos {
    static constexpr std::uint16_t packet_id = 0x1E;
    static constexpr std::string_view packet_name = "move_player_pos";

    double x;
    double y;
    double z;
    std::uint8_t flags;
};

struct move_player_pos_rot {
    static constexpr std::uint16_t packet_id = 0x1F;
    static constexpr std::string_view packet_name = "move_player_pos_rot";

    double x;
    double y;
    double z;
    float y_rot;
    float x_rot;
    std::uint8_t flags;
};

struct move_player_rot {
    static constexpr std::uint16_t packet_id = 0x20;
    static constexpr std::string_view packet_name = "move_player_rot";

    float y_rot;
    float x_rot;
    std::uint8_t flags;
};

struct move_player_status_only {
    static constexpr std::uint16_t packet_id = 0x21;
    static constexpr std::string_view packet_name = "move_player_status_only";

    std::uint8_t flags;
};

struct player_action {
    static constexpr std::uint16_t packet_id = 0x29;
    static constexpr std::string_view packet_name = "player_action";

    // attributes: #[varint]
    std::uint32_t action;
    std::int64_t position;
    std::uint8_t direction;
    // attributes: #[varint]
    std::uint32_t sequence;
};

struct player_loaded {
    static constexpr std::uint16_t packet_id = 0x2C;
    static constexpr std::string_view packet_name = "player_loaded";

};

struct set_carried_item {
    static constexpr std::uint16_t packet_id = 0x36;
    static constexpr std::string_view packet_name = "set_carried_item";

    // attributes: #[fixed]
    std::int16_t slot;
};

struct use_item_on {
    static constexpr std::uint16_t packet_id = 0x42;
    static constexpr std::string_view packet_name = "use_item_on";

    // attributes: #[varint]
    std::uint32_t hand;
    std::int64_t position;
    // attributes: #[varint]
    std::uint32_t face;
    float click_x;
    float click_y;
    float click_z;
    bool inside;
    bool world_border;
    // attributes: #[varint]
    std::uint32_t sequence;
};

struct attack {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "attack";

    // attributes: #[varint]
    std::int32_t entity_id;
};

struct block_entity_tag_query {
    static constexpr std::uint16_t packet_id = 0x2;
    static constexpr std::string_view packet_name = "block_entity_tag_query";

    // attributes: #[varint]
    std::int32_t transaction_id;
    std::int64_t pos;
};

struct bundle_item_selected {
    static constexpr std::uint16_t packet_id = 0x3;
    static constexpr std::string_view packet_name = "bundle_item_selected";

    // attributes: #[varint]
    std::int32_t slot_id;
    // attributes: #[varint]
    std::int32_t selected_item_index;
};

struct change_difficulty {
    static constexpr std::uint16_t packet_id = 0x4;
    static constexpr std::string_view packet_name = "change_difficulty";

    std::uint32_t difficulty;
};

struct change_game_mode {
    static constexpr std::uint16_t packet_id = 0x5;
    static constexpr std::string_view packet_name = "change_game_mode";

    std::uint32_t mode;
};

struct chat_ack {
    static constexpr std::uint16_t packet_id = 0x6;
    static constexpr std::string_view packet_name = "chat_ack";

    // attributes: #[varint]
    std::int32_t offset;
};

struct chat_command_signed {
    static constexpr std::uint16_t packet_id = 0x8;
    static constexpr std::string_view packet_name = "chat_command_signed";

    // attributes: #[max(32767)]
    std::string command;
    std::int64_t time_stamp;
    std::int64_t salt;
    ::refrain::protocol::v777::wire::wire_argument_signatures argument_signatures;
    ::refrain::protocol::v777::wire::wire_update last_seen_messages;
};

struct chat_session_update {
    static constexpr std::uint16_t packet_id = 0xA;
    static constexpr std::string_view packet_name = "chat_session_update";

    ::refrain::net::uuid session_id;
    ::refrain::protocol::v777::wire::wire_data profile_public_key;
};

struct client_command {
    static constexpr std::uint16_t packet_id = 0xC;
    static constexpr std::string_view packet_name = "client_command";

    std::uint32_t action;
};

struct client_tick_end {
    static constexpr std::uint16_t packet_id = 0xD;
    static constexpr std::string_view packet_name = "client_tick_end";

};

struct command_suggestion {
    static constexpr std::uint16_t packet_id = 0xF;
    static constexpr std::string_view packet_name = "command_suggestion";

    // attributes: #[varint]
    std::int32_t id;
    // attributes: #[max(32500)]
    std::string command;
};

struct configuration_acknowledged {
    static constexpr std::uint16_t packet_id = 0x10;
    static constexpr std::string_view packet_name = "configuration_acknowledged";

};

struct container_button_click {
    static constexpr std::uint16_t packet_id = 0x11;
    static constexpr std::string_view packet_name = "container_button_click";

    // attributes: #[varint]
    std::int32_t container_id;
    // attributes: #[varint]
    std::int32_t button_id;
};

struct container_click {
    static constexpr std::uint16_t packet_id = 0x12;
    static constexpr std::string_view packet_name = "container_click";

    ::refrain::net::container_click_data payload;
};

struct container_close {
    static constexpr std::uint16_t packet_id = 0x13;
    static constexpr std::string_view packet_name = "container_close";

    // attributes: #[varint]
    std::int32_t container_id;
};

struct container_slot_state_changed {
    static constexpr std::uint16_t packet_id = 0x14;
    static constexpr std::string_view packet_name = "container_slot_state_changed";

    // attributes: #[varint]
    std::int32_t slot_id;
    // attributes: #[varint]
    std::int32_t container_id;
    bool new_state;
};

struct cookie_response {
    static constexpr std::uint16_t packet_id = 0x15;
    static constexpr std::string_view packet_name = "cookie_response";

    ::refrain::data::resource_location key;
    std::optional<std::vector<std::byte>> payload;
};

struct custom_payload {
    static constexpr std::uint16_t packet_id = 0x16;
    static constexpr std::string_view packet_name = "custom_payload";

    ::refrain::data::resource_location channel;
    // attributes: #[rest]
    std::vector<std::byte> data;
};

struct debug_subscription_request {
    static constexpr std::uint16_t packet_id = 0x17;
    static constexpr std::string_view packet_name = "debug_subscription_request";

    std::vector<std::uint32_t> payload;
};

struct edit_book {
    static constexpr std::uint16_t packet_id = 0x18;
    static constexpr std::string_view packet_name = "edit_book";

    // attributes: #[varint]
    std::int32_t slot;
    std::vector<std::string> pages;
    std::optional<std::string> title;
};

struct entity_tag_query {
    static constexpr std::uint16_t packet_id = 0x19;
    static constexpr std::string_view packet_name = "entity_tag_query";

    // attributes: #[varint]
    std::int32_t transaction_id;
    // attributes: #[varint]
    std::int32_t entity_id;
};

struct interact {
    static constexpr std::uint16_t packet_id = 0x1A;
    static constexpr std::string_view packet_name = "interact";

    // attributes: #[varint]
    std::int32_t entity_id;
    std::uint32_t hand;
    ::refrain::net::lp_vec3 location;
    bool using_secondary_action;
};

struct jigsaw_generate {
    static constexpr std::uint16_t packet_id = 0x1B;
    static constexpr std::string_view packet_name = "jigsaw_generate";

    std::int64_t pos;
    // attributes: #[varint]
    std::int32_t levels;
    bool keep_jigsaws;
};

struct lock_difficulty {
    static constexpr std::uint16_t packet_id = 0x1D;
    static constexpr std::string_view packet_name = "lock_difficulty";

    bool locked;
};

struct move_vehicle {
    static constexpr std::uint16_t packet_id = 0x22;
    static constexpr std::string_view packet_name = "move_vehicle";

    ::refrain::protocol::v777::wire::wire_position_and_rotation moving_to;
    bool on_ground;
};

struct paddle_boat {
    static constexpr std::uint16_t packet_id = 0x23;
    static constexpr std::string_view packet_name = "paddle_boat";

    bool left;
    bool right;
};

struct pick_item_from_block {
    static constexpr std::uint16_t packet_id = 0x24;
    static constexpr std::string_view packet_name = "pick_item_from_block";

    std::int64_t pos;
    bool include_data;
};

struct pick_item_from_entity {
    static constexpr std::uint16_t packet_id = 0x25;
    static constexpr std::string_view packet_name = "pick_item_from_entity";

    // attributes: #[varint]
    std::int32_t id;
    bool include_data;
};

struct ping_request {
    static constexpr std::uint16_t packet_id = 0x26;
    static constexpr std::string_view packet_name = "ping_request";

    std::int64_t time;
};

struct place_recipe {
    static constexpr std::uint16_t packet_id = 0x27;
    static constexpr std::string_view packet_name = "place_recipe";

    // attributes: #[varint]
    std::int32_t container_id;
    ::refrain::protocol::v777::wire::wire_recipe_display_id recipe;
    bool use_max_items;
};

struct player_abilities {
    static constexpr std::uint16_t packet_id = 0x28;
    static constexpr std::string_view packet_name = "player_abilities";

    // attributes: #[fixed]
    std::int8_t is_flying;
};

struct player_command {
    static constexpr std::uint16_t packet_id = 0x2A;
    static constexpr std::string_view packet_name = "player_command";

    // attributes: #[varint]
    std::int32_t id;
    std::uint32_t action;
    // attributes: #[varint]
    std::int32_t data;
};

struct player_input {
    static constexpr std::uint16_t packet_id = 0x2B;
    static constexpr std::string_view packet_name = "player_input";

    // attributes: #[fixed]
    std::int8_t input;
};

struct pong {
    static constexpr std::uint16_t packet_id = 0x2D;
    static constexpr std::string_view packet_name = "pong";

    // attributes: #[fixed]
    std::int32_t id;
};

struct punch {
    static constexpr std::uint16_t packet_id = 0x2E;
    static constexpr std::string_view packet_name = "punch";

};

struct recipe_book_change_settings {
    static constexpr std::uint16_t packet_id = 0x2F;
    static constexpr std::string_view packet_name = "recipe_book_change_settings";

    std::uint32_t book_type;
    bool is_open;
    bool is_filtering;
};

struct recipe_book_seen_recipe {
    static constexpr std::uint16_t packet_id = 0x30;
    static constexpr std::string_view packet_name = "recipe_book_seen_recipe";

    ::refrain::protocol::v777::wire::wire_recipe_display_id recipe;
};

struct rename_item {
    static constexpr std::uint16_t packet_id = 0x31;
    static constexpr std::string_view packet_name = "rename_item";

    // attributes: #[max(32767)]
    std::string name;
};

struct resource_pack {
    static constexpr std::uint16_t packet_id = 0x32;
    static constexpr std::string_view packet_name = "resource_pack";

    ::refrain::net::uuid id;
    std::uint32_t action;
};

struct seen_advancements {
    static constexpr std::uint16_t packet_id = 0x33;
    static constexpr std::string_view packet_name = "seen_advancements";

    ::refrain::net::advancement_seen payload;
};

struct select_trade {
    static constexpr std::uint16_t packet_id = 0x34;
    static constexpr std::string_view packet_name = "select_trade";

    // attributes: #[varint]
    std::int32_t item;
};

struct set_beacon {
    static constexpr std::uint16_t packet_id = 0x35;
    static constexpr std::string_view packet_name = "set_beacon";

    // attributes: #[optional_varint]
    std::optional<std::uint32_t> primary;
    // attributes: #[optional_varint]
    std::optional<std::uint32_t> secondary;
};

struct set_command_block {
    static constexpr std::uint16_t packet_id = 0x37;
    static constexpr std::string_view packet_name = "set_command_block";

    std::int64_t pos;
    // attributes: #[max(32767)]
    std::string command;
    std::uint32_t mode;
    // attributes: #[fixed]
    std::int8_t track_output;
};

struct set_command_minecart {
    static constexpr std::uint16_t packet_id = 0x38;
    static constexpr std::string_view packet_name = "set_command_minecart";

    // attributes: #[varint]
    std::int32_t entity;
    // attributes: #[max(32767)]
    std::string command;
    bool track_output;
};

struct set_creative_mode_slot {
    static constexpr std::uint16_t packet_id = 0x39;
    static constexpr std::string_view packet_name = "set_creative_mode_slot";

    // attributes: #[fixed]
    std::int16_t slot_num;
    ::refrain::net::item_stack item_stack;
};

struct set_game_rule {
    static constexpr std::uint16_t packet_id = 0x3A;
    static constexpr std::string_view packet_name = "set_game_rule";

    std::vector<::refrain::protocol::v777::wire::wire_entry_4> entries;
};

struct set_jigsaw_block {
    static constexpr std::uint16_t packet_id = 0x3B;
    static constexpr std::string_view packet_name = "set_jigsaw_block";

    std::int64_t pos;
    ::refrain::data::resource_location name;
    ::refrain::data::resource_location target;
    ::refrain::data::resource_location pool;
    // attributes: #[max(32767)]
    std::string final_state;
    // attributes: #[max(32767)]
    std::string joint;
    // attributes: #[varint]
    std::int32_t selection_priority;
    // attributes: #[varint]
    std::int32_t placement_priority;
};

struct set_structure_block {
    static constexpr std::uint16_t packet_id = 0x3C;
    static constexpr std::string_view packet_name = "set_structure_block";

    std::int64_t pos;
    std::uint32_t update_type;
    std::uint32_t mode;
    // attributes: #[max(32767)]
    std::string name;
    // attributes: #[fixed]
    std::int8_t offset;
    // attributes: #[fixed]
    std::int8_t size;
    // attributes: #[fixed]
    std::int8_t mirror;
    // attributes: #[fixed]
    std::int8_t rotation;
    // attributes: #[fixed]
    std::int8_t data;
    // attributes: #[fixed]
    std::int8_t integrity;
    std::uint32_t mirror_2;
    std::uint32_t rotation_2;
    // attributes: #[max(32767)]
    std::string data_2;
    float integrity_2;
    // attributes: #[varlong]
    std::int64_t seed;
    // attributes: #[fixed]
    std::int8_t field_15;
};

struct set_test_block {
    static constexpr std::uint16_t packet_id = 0x3D;
    static constexpr std::string_view packet_name = "set_test_block";

    std::int64_t position;
    std::uint32_t mode;
    // attributes: #[max(32767)]
    std::string message;
};

struct sign_update {
    static constexpr std::uint16_t packet_id = 0x3E;
    static constexpr std::string_view packet_name = "sign_update";

    std::int64_t pos;
    // attributes: #[fixed_count(4)]
    std::vector<std::string> lines;
    std::uint32_t slot;
};

struct spectator_action {
    static constexpr std::uint16_t packet_id = 0x3F;
    static constexpr std::string_view packet_name = "spectator_action";

    // attributes: #[optional_varint]
    std::optional<std::int32_t> spectate_entity_id;
};

struct teleport_to_entity {
    static constexpr std::uint16_t packet_id = 0x40;
    static constexpr std::string_view packet_name = "teleport_to_entity";

    ::refrain::net::uuid uuid;
};

struct test_instance_block_action {
    static constexpr std::uint16_t packet_id = 0x41;
    static constexpr std::string_view packet_name = "test_instance_block_action";

    std::int64_t pos;
    std::uint32_t action;
    ::refrain::protocol::v777::wire::wire_data_3 data;
};

struct use_item {
    static constexpr std::uint16_t packet_id = 0x43;
    static constexpr std::string_view packet_name = "use_item";

    std::uint32_t hand;
    // attributes: #[varint]
    std::int32_t sequence;
    float y_rot;
    float x_rot;
};

struct custom_click_action {
    static constexpr std::uint16_t packet_id = 0x44;
    static constexpr std::string_view packet_name = "custom_click_action";

    ::refrain::data::resource_location id;
    ::refrain::net::nbt_blob payload;
};
[[nodiscard]] inline auto encode_accept_teleportation(::refrain::net::writer &out, const accept_teleportation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_f64(value.x);
  }
  {
    out.write_f64(value.y);
  }
  {
    out.write_f64(value.z);
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_accept_teleportation(::refrain::net::reader &in, accept_teleportation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chat_command(::refrain::net::writer &out, const chat_command &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.command, 256); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chat_command(::refrain::net::reader &in, chat_command &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(256);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.command = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chat(::refrain::net::writer &out, const chat &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.message, 256); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.timestamp));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.salt));
  }
  {
    out.write_bool(value.signature.has_value());
    if (value.signature.has_value()) {
      if (value.signature.value().size() != 256) {
        return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'signature' must be exactly 256 byte(s)")};
      }
      out.write_bytes(std::span<const std::byte>{value.signature.value() });
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.offset));
  }
  {
    if (value.acknowledged.size() != 20) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'acknowledged' must be exactly 20 byte(s)")};
    }
    out.write_bytes(std::span<const std::byte>{value.acknowledged });
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.checksum));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chat(::refrain::net::reader &in, chat &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(256);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.message = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.timestamp = static_cast<decltype(value.timestamp)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.salt = static_cast<decltype(value.salt)>(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::vector<std::byte> value_0{};
      auto value_1 = in.read_bytes(256);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::vector<std::byte>{value_1->begin(), value_1->end()};
      value.signature = std::move(value_0);
    } else {
      value.signature.reset();
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.offset = static_cast<decltype(value.offset)>(*value_0);
  }
  {
    auto value_0 = in.read_bytes(20);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.acknowledged = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.checksum = static_cast<decltype(value.checksum)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chunk_batch_received(::refrain::net::writer &out, const chunk_batch_received &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f32(value.desired_chunks);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chunk_batch_received(::refrain::net::reader &in, chunk_batch_received &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.desired_chunks = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_client_information(::refrain::net::writer &out, const client_information &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::common::encode_client_information_data(out, value.information); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_client_information(::refrain::net::reader &in, client_information &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::common::decode_client_information_data(in, value.information); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_keep_alive(::refrain::net::writer &out, const keep_alive &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_keep_alive(::refrain::net::reader &in, keep_alive &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_player_pos(::refrain::net::writer &out, const move_player_pos &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.x);
  }
  {
    out.write_f64(value.y);
  }
  {
    out.write_f64(value.z);
  }
  {
    out.write_u8(value.flags);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_player_pos(::refrain::net::reader &in, move_player_pos &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = *value_0;
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags = static_cast<decltype(value.flags)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_player_pos_rot(::refrain::net::writer &out, const move_player_pos_rot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.x);
  }
  {
    out.write_f64(value.y);
  }
  {
    out.write_f64(value.z);
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  {
    out.write_u8(value.flags);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_player_pos_rot(::refrain::net::reader &in, move_player_pos_rot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = *value_0;
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags = static_cast<decltype(value.flags)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_player_rot(::refrain::net::writer &out, const move_player_rot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  {
    out.write_u8(value.flags);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_player_rot(::refrain::net::reader &in, move_player_rot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = *value_0;
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags = static_cast<decltype(value.flags)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_player_status_only(::refrain::net::writer &out, const move_player_status_only &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(value.flags);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_player_status_only(::refrain::net::reader &in, move_player_status_only &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags = static_cast<decltype(value.flags)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_action(::refrain::net::writer &out, const player_action &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.action));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  {
    out.write_u8(value.direction);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sequence));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_action(::refrain::net::reader &in, player_action &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.action = static_cast<decltype(value.action)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.direction = static_cast<decltype(value.direction)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sequence = static_cast<decltype(value.sequence)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_loaded(::refrain::net::writer &out, const player_loaded &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_loaded(::refrain::net::reader &in, player_loaded &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_carried_item(::refrain::net::writer &out, const set_carried_item &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u16(static_cast<std::uint16_t>(value.slot));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_carried_item(::refrain::net::reader &in, set_carried_item &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = static_cast<decltype(value.slot)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_use_item_on(::refrain::net::writer &out, const use_item_on &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.hand));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.face));
  }
  {
    out.write_f32(value.click_x);
  }
  {
    out.write_f32(value.click_y);
  }
  {
    out.write_f32(value.click_z);
  }
  {
    out.write_bool(value.inside);
  }
  {
    out.write_bool(value.world_border);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sequence));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_use_item_on(::refrain::net::reader &in, use_item_on &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hand = static_cast<decltype(value.hand)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.face = static_cast<decltype(value.face)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.click_x = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.click_y = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.click_z = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.inside = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.world_border = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sequence = static_cast<decltype(value.sequence)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_attack(::refrain::net::writer &out, const attack &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_attack(::refrain::net::reader &in, attack &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_entity_tag_query(::refrain::net::writer &out, const block_entity_tag_query &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.transaction_id));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_entity_tag_query(::refrain::net::reader &in, block_entity_tag_query &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.transaction_id = static_cast<decltype(value.transaction_id)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_bundle_item_selected(::refrain::net::writer &out, const bundle_item_selected &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.slot_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.selected_item_index));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_bundle_item_selected(::refrain::net::reader &in, bundle_item_selected &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot_id = static_cast<decltype(value.slot_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.selected_item_index = static_cast<decltype(value.selected_item_index)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_change_difficulty(::refrain::net::writer &out, const change_difficulty &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.difficulty);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_change_difficulty(::refrain::net::reader &in, change_difficulty &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.difficulty = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_change_game_mode(::refrain::net::writer &out, const change_game_mode &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.mode);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_change_game_mode(::refrain::net::reader &in, change_game_mode &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.mode = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chat_ack(::refrain::net::writer &out, const chat_ack &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.offset));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chat_ack(::refrain::net::reader &in, chat_ack &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.offset = static_cast<decltype(value.offset)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chat_command_signed(::refrain::net::writer &out, const chat_command_signed &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.command, 32767); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.time_stamp));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.salt));
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_argument_signatures(out, value.argument_signatures); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_update(out, value.last_seen_messages); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chat_command_signed(::refrain::net::reader &in, chat_command_signed &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.command = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.time_stamp = static_cast<decltype(value.time_stamp)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.salt = static_cast<decltype(value.salt)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_argument_signatures(in, value.argument_signatures); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_update(in, value.last_seen_messages); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chat_session_update(::refrain::net::writer &out, const chat_session_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_uuid(value.session_id); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_data(out, value.profile_public_key); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chat_session_update(::refrain::net::reader &in, chat_session_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_uuid();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.session_id = *value_0;
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_data(in, value.profile_public_key); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_client_command(::refrain::net::writer &out, const client_command &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.action);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_client_command(::refrain::net::reader &in, client_command &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.action = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_client_tick_end(::refrain::net::writer &out, const client_tick_end &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_client_tick_end(::refrain::net::reader &in, client_tick_end &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_command_suggestion(::refrain::net::writer &out, const command_suggestion &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    if (auto written = out.write_string(value.command, 32500); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_command_suggestion(::refrain::net::reader &in, command_suggestion &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_string(32500);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.command = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_configuration_acknowledged(::refrain::net::writer &out, const configuration_acknowledged &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_configuration_acknowledged(::refrain::net::reader &in, configuration_acknowledged &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_button_click(::refrain::net::writer &out, const container_button_click &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.button_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_button_click(::refrain::net::reader &in, container_button_click &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.button_id = static_cast<decltype(value.button_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_click(::refrain::net::writer &out, const container_click &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_container_click_data(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_click(::refrain::net::reader &in, container_click &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_container_click_data(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_close(::refrain::net::writer &out, const container_close &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_close(::refrain::net::reader &in, container_close &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_slot_state_changed(::refrain::net::writer &out, const container_slot_state_changed &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.slot_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    out.write_bool(value.new_state);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_slot_state_changed(::refrain::net::reader &in, container_slot_state_changed &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot_id = static_cast<decltype(value.slot_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.new_state = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_cookie_response(::refrain::net::writer &out, const cookie_response &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.key); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.payload.has_value());
    if (value.payload.has_value()) {
      out.write_varint(static_cast<std::uint32_t>(value.payload.value().size()));
      out.write_bytes(std::span<const std::byte>{value.payload.value() });
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_cookie_response(::refrain::net::reader &in, cookie_response &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key = std::move(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::vector<std::byte> value_0{};
      auto length_1 = in.read_varint();
      if (!length_1) return std::unexpected{std::move(length_1.error())};
      auto value_1 = in.read_bytes(*length_1);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::vector<std::byte>{value_1->begin(), value_1->end()};
      value.payload = std::move(value_0);
    } else {
      value.payload.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_custom_payload(::refrain::net::writer &out, const custom_payload &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.channel); !written) {
      return written;
    }
  }
  {
    out.write_bytes(value.data);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_custom_payload(::refrain::net::reader &in, custom_payload &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.channel = std::move(*value_0);
  }
  {
    auto value_0 = in.read_bytes(in.remaining());
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_debug_subscription_request(::refrain::net::writer &out, const debug_subscription_request &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.payload.size()));
    for (const auto &item_0 : value.payload) {
      out.write_varint(item_0);
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_debug_subscription_request(::refrain::net::reader &in, debug_subscription_request &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.payload.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::uint32_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.payload.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_edit_book(::refrain::net::writer &out, const edit_book &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.slot));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.pages.size()));
    for (const auto &item_0 : value.pages) {
      if (auto written = out.write_string(item_0, 32767); !written) {
        return written;
      }
    }
  }
  {
    out.write_bool(value.title.has_value());
    if (value.title.has_value()) {
      if (auto written = out.write_string(value.title.value(), 32767); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_edit_book(::refrain::net::reader &in, edit_book &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = static_cast<decltype(value.slot)>(*value_0);
  }
  {
    value.pages.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::string value_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.pages.push_back(std::move(value_0));
    }
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::string value_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.title = std::move(value_0);
    } else {
      value.title.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_entity_tag_query(::refrain::net::writer &out, const entity_tag_query &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.transaction_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_entity_tag_query(::refrain::net::reader &in, entity_tag_query &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.transaction_id = static_cast<decltype(value.transaction_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_interact(::refrain::net::writer &out, const interact &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    out.write_varint(value.hand);
  }
  {
    if (auto written = ::refrain::net::encode_lp_vec3(out, value.location); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.using_secondary_action);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_interact(::refrain::net::reader &in, interact &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hand = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_lp_vec3(in, value.location); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.using_secondary_action = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_jigsaw_generate(::refrain::net::writer &out, const jigsaw_generate &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.levels));
  }
  {
    out.write_bool(value.keep_jigsaws);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_jigsaw_generate(::refrain::net::reader &in, jigsaw_generate &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.levels = static_cast<decltype(value.levels)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.keep_jigsaws = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_lock_difficulty(::refrain::net::writer &out, const lock_difficulty &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.locked);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_lock_difficulty(::refrain::net::reader &in, lock_difficulty &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.locked = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_vehicle(::refrain::net::writer &out, const move_vehicle &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_position_and_rotation(out, value.moving_to); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.on_ground);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_vehicle(::refrain::net::reader &in, move_vehicle &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_position_and_rotation(in, value.moving_to); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.on_ground = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_paddle_boat(::refrain::net::writer &out, const paddle_boat &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.left);
  }
  {
    out.write_bool(value.right);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_paddle_boat(::refrain::net::reader &in, paddle_boat &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.left = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.right = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_pick_item_from_block(::refrain::net::writer &out, const pick_item_from_block &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_bool(value.include_data);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_pick_item_from_block(::refrain::net::reader &in, pick_item_from_block &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.include_data = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_pick_item_from_entity(::refrain::net::writer &out, const pick_item_from_entity &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_bool(value.include_data);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_pick_item_from_entity(::refrain::net::reader &in, pick_item_from_entity &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.include_data = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_ping_request(::refrain::net::writer &out, const ping_request &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.time));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_ping_request(::refrain::net::reader &in, ping_request &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.time = static_cast<decltype(value.time)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_place_recipe(::refrain::net::writer &out, const place_recipe &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_recipe_display_id(out, value.recipe); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.use_max_items);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_place_recipe(::refrain::net::reader &in, place_recipe &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_recipe_display_id(in, value.recipe); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.use_max_items = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_abilities(::refrain::net::writer &out, const player_abilities &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(static_cast<std::uint8_t>(value.is_flying));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_abilities(::refrain::net::reader &in, player_abilities &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.is_flying = static_cast<decltype(value.is_flying)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_command(::refrain::net::writer &out, const player_command &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_varint(value.action);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.data));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_command(::refrain::net::reader &in, player_command &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.action = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = static_cast<decltype(value.data)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_input(::refrain::net::writer &out, const player_input &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(static_cast<std::uint8_t>(value.input));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_input(::refrain::net::reader &in, player_input &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.input = static_cast<decltype(value.input)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_pong(::refrain::net::writer &out, const pong &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_pong(::refrain::net::reader &in, pong &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_punch(::refrain::net::writer &out, const punch &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_punch(::refrain::net::reader &in, punch &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_recipe_book_change_settings(::refrain::net::writer &out, const recipe_book_change_settings &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.book_type);
  }
  {
    out.write_bool(value.is_open);
  }
  {
    out.write_bool(value.is_filtering);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_recipe_book_change_settings(::refrain::net::reader &in, recipe_book_change_settings &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.book_type = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.is_open = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.is_filtering = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_recipe_book_seen_recipe(::refrain::net::writer &out, const recipe_book_seen_recipe &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_recipe_display_id(out, value.recipe); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_recipe_book_seen_recipe(::refrain::net::reader &in, recipe_book_seen_recipe &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_recipe_display_id(in, value.recipe); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_rename_item(::refrain::net::writer &out, const rename_item &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.name, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_rename_item(::refrain::net::reader &in, rename_item &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_resource_pack(::refrain::net::writer &out, const resource_pack &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_uuid(value.id); !written) {
      return written;
    }
  }
  {
    out.write_varint(value.action);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_resource_pack(::refrain::net::reader &in, resource_pack &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_uuid();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.action = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_seen_advancements(::refrain::net::writer &out, const seen_advancements &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_advancement_seen(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_seen_advancements(::refrain::net::reader &in, seen_advancements &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_advancement_seen(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_select_trade(::refrain::net::writer &out, const select_trade &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.item));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_select_trade(::refrain::net::reader &in, select_trade &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.item = static_cast<decltype(value.item)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_beacon(::refrain::net::writer &out, const set_beacon &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.primary.has_value() ? static_cast<std::uint32_t>(value.primary.value()) + 1U : 0U);
  }
  {
    out.write_varint(value.secondary.has_value() ? static_cast<std::uint32_t>(value.secondary.value()) + 1U : 0U);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_beacon(::refrain::net::reader &in, set_beacon &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto present_0 = in.read_varint();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0 != 0) {
      value.primary = static_cast<std::uint32_t>(*present_0 - 1U);
    } else {
      value.primary.reset();
    }
  }
  {
    auto present_0 = in.read_varint();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0 != 0) {
      value.secondary = static_cast<std::uint32_t>(*present_0 - 1U);
    } else {
      value.secondary.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_command_block(::refrain::net::writer &out, const set_command_block &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    if (auto written = out.write_string(value.command, 32767); !written) {
      return written;
    }
  }
  {
    out.write_varint(value.mode);
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.track_output));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_command_block(::refrain::net::reader &in, set_command_block &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.command = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.mode = *value_0;
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.track_output = static_cast<decltype(value.track_output)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_command_minecart(::refrain::net::writer &out, const set_command_minecart &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity));
  }
  {
    if (auto written = out.write_string(value.command, 32767); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.track_output);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_command_minecart(::refrain::net::reader &in, set_command_minecart &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity = static_cast<decltype(value.entity)>(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.command = std::move(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.track_output = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_creative_mode_slot(::refrain::net::writer &out, const set_creative_mode_slot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u16(static_cast<std::uint16_t>(value.slot_num));
  }
  {
    if (auto written = ::refrain::net::encode_item_stack(out, value.item_stack); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_creative_mode_slot(::refrain::net::reader &in, set_creative_mode_slot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot_num = static_cast<decltype(value.slot_num)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_item_stack(in, value.item_stack); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_game_rule(::refrain::net::writer &out, const set_game_rule &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &item_0 : value.entries) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_entry_4(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_game_rule(::refrain::net::reader &in, set_game_rule &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.entries.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_entry_4 value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_entry_4(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.entries.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_jigsaw_block(::refrain::net::writer &out, const set_jigsaw_block &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    if (auto written = out.write_identifier(value.name); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_identifier(value.target); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_identifier(value.pool); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.final_state, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.joint, 32767); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.selection_priority));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.placement_priority));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_jigsaw_block(::refrain::net::reader &in, set_jigsaw_block &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.target = std::move(*value_0);
  }
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pool = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.final_state = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.joint = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.selection_priority = static_cast<decltype(value.selection_priority)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.placement_priority = static_cast<decltype(value.placement_priority)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_structure_block(::refrain::net::writer &out, const set_structure_block &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_varint(value.update_type);
  }
  {
    out.write_varint(value.mode);
  }
  {
    if (auto written = out.write_string(value.name, 32767); !written) {
      return written;
    }
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.offset));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.size));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.mirror));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.rotation));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.data));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.integrity));
  }
  {
    out.write_varint(value.mirror_2);
  }
  {
    out.write_varint(value.rotation_2);
  }
  {
    if (auto written = out.write_string(value.data_2, 32767); !written) {
      return written;
    }
  }
  {
    out.write_f32(value.integrity_2);
  }
  {
    out.write_varlong(static_cast<std::uint64_t>(value.seed));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.field_15));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_structure_block(::refrain::net::reader &in, set_structure_block &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.update_type = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.mode = *value_0;
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.offset = static_cast<decltype(value.offset)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.size = static_cast<decltype(value.size)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.mirror = static_cast<decltype(value.mirror)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.rotation = static_cast<decltype(value.rotation)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = static_cast<decltype(value.data)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.integrity = static_cast<decltype(value.integrity)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.mirror_2 = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.rotation_2 = *value_0;
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data_2 = std::move(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.integrity_2 = *value_0;
  }
  {
    auto value_0 = in.read_varlong();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.seed = static_cast<decltype(value.seed)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.field_15 = static_cast<decltype(value.field_15)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_test_block(::refrain::net::writer &out, const set_test_block &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  {
    out.write_varint(value.mode);
  }
  {
    if (auto written = out.write_string(value.message, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_test_block(::refrain::net::reader &in, set_test_block &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.mode = *value_0;
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.message = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_sign_update(::refrain::net::writer &out, const sign_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    if (value.lines.size() != 4) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'lines' must have exactly 4 element(s)")};
    }
    for (const auto &item_0 : value.lines) {
      if (auto written = out.write_string(item_0, 32767); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(value.slot);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_sign_update(::refrain::net::reader &in, sign_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    value.lines.clear();
    value.lines.reserve(4);
    for (std::uint32_t index_0 = 0; index_0 < 4; ++index_0) {
      std::string value_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.lines.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_spectator_action(::refrain::net::writer &out, const spectator_action &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.spectate_entity_id.has_value() ? static_cast<std::uint32_t>(value.spectate_entity_id.value()) + 1U : 0U);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_spectator_action(::refrain::net::reader &in, spectator_action &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto present_0 = in.read_varint();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0 != 0) {
      value.spectate_entity_id = static_cast<std::int32_t>(*present_0 - 1U);
    } else {
      value.spectate_entity_id.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_teleport_to_entity(::refrain::net::writer &out, const teleport_to_entity &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_uuid(value.uuid); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_teleport_to_entity(::refrain::net::reader &in, teleport_to_entity &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_uuid();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.uuid = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_test_instance_block_action(::refrain::net::writer &out, const test_instance_block_action &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_varint(value.action);
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_data_3(out, value.data); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_test_instance_block_action(::refrain::net::reader &in, test_instance_block_action &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.action = *value_0;
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_data_3(in, value.data); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_use_item(::refrain::net::writer &out, const use_item &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.hand);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sequence));
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_use_item(::refrain::net::reader &in, use_item &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hand = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sequence = static_cast<decltype(value.sequence)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_custom_click_action(::refrain::net::writer &out, const custom_click_action &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.id); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::net::encode_nbt(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_custom_click_action(::refrain::net::reader &in, custom_click_action &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = std::move(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_nbt(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::play::serverbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_SERVERBOUND_HPP
