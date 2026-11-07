// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 7cfeb406836d8806

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_SERVERBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_SERVERBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/common.hpp>

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

} // namespace refrain::protocol::v777::play::serverbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_SERVERBOUND_HPP
