// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9582d232958dd1ff

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_CLIENTBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_CLIENTBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>

namespace refrain::protocol::v777::play::clientbound {

struct global_pos {
    ::refrain::data::resource_location dimension;
    std::int64_t position;
};

struct spawn_info {
    /// Dimension type registry holder.
    // attributes: #[varint]
    std::uint32_t dimension_type;
    ::refrain::data::resource_location dimension;
    std::int64_t seed;
    // attributes: #[varint]
    std::uint32_t game_type;
    /// `0` means absent; a present value is stored as `value + 1`.
    // attributes: #[optional_varint]
    std::optional<std::uint32_t> previous_game_type;
    bool debug;
    bool flat;
    std::optional<::refrain::protocol::v777::play::clientbound::global_pos> last_death_location;
    // attributes: #[varint]
    std::uint32_t portal_cooldown;
    // attributes: #[varint]
    std::uint32_t sea_level;
};

struct clock_update {
    ::refrain::data::resource_location clock;
    // attributes: #[varint]
    std::uint32_t state;
};

struct block_changed_ack {
    static constexpr std::uint16_t packet_id = 0x4;
    static constexpr std::string_view packet_name = "block_changed_ack";

    // attributes: #[varint]
    std::uint32_t sequence;
};

struct block_update {
    static constexpr std::uint16_t packet_id = 0x8;
    static constexpr std::string_view packet_name = "block_update";

    std::int64_t position;
    // attributes: #[varint]
    std::uint32_t state;
};

struct change_difficulty {
    static constexpr std::uint16_t packet_id = 0xA;
    static constexpr std::string_view packet_name = "change_difficulty";

    // attributes: #[varint]
    std::uint32_t difficulty;
    bool locked;
};

struct chunk_batch_finished {
    static constexpr std::uint16_t packet_id = 0xB;
    static constexpr std::string_view packet_name = "chunk_batch_finished";

    // attributes: #[varint]
    std::uint32_t batch_size;
};

struct chunk_batch_start {
    static constexpr std::uint16_t packet_id = 0xC;
    static constexpr std::string_view packet_name = "chunk_batch_start";

};

struct entity_event {
    static constexpr std::uint16_t packet_id = 0x22;
    static constexpr std::string_view packet_name = "entity_event";

    // attributes: #[fixed]
    std::int32_t entity_id;
    std::uint8_t event;
};

struct game_event {
    static constexpr std::uint16_t packet_id = 0x27;
    static constexpr std::string_view packet_name = "game_event";

    std::uint8_t event;
    float value;
};

struct initialize_border {
    static constexpr std::uint16_t packet_id = 0x2C;
    static constexpr std::string_view packet_name = "initialize_border";

    double center_x;
    double center_z;
    double old_size;
    double new_size;
    // attributes: #[varlong]
    std::int64_t lerp_time;
    // attributes: #[varint]
    std::uint32_t absolute_max_size;
    // attributes: #[varint]
    std::uint32_t warning_blocks;
    // attributes: #[varint]
    std::uint32_t warning_time;
};

struct keep_alive {
    static constexpr std::uint16_t packet_id = 0x2D;
    static constexpr std::string_view packet_name = "keep_alive";

    std::int64_t id;
};

struct login {
    static constexpr std::uint16_t packet_id = 0x32;
    static constexpr std::string_view packet_name = "login";

    // attributes: #[fixed]
    std::int32_t player_id;
    bool hardcore;
    std::vector<::refrain::data::resource_location> levels;
    // attributes: #[varint]
    std::uint32_t max_players;
    // attributes: #[varint]
    std::uint32_t chunk_radius;
    // attributes: #[varint]
    std::uint32_t simulation_distance;
    bool reduced_debug_info;
    bool show_death_screen;
    bool limited_crafting;
    ::refrain::protocol::v777::play::clientbound::spawn_info spawn;
    bool online_mode;
    bool enforces_secure_chat;
};

struct player_abilities {
    static constexpr std::uint16_t packet_id = 0x41;
    static constexpr std::string_view packet_name = "player_abilities";

    std::uint8_t flags;
    float flying_speed;
    float walking_speed;
};

struct player_position {
    static constexpr std::uint16_t packet_id = 0x49;
    static constexpr std::string_view packet_name = "player_position";

    // attributes: #[varint]
    std::uint32_t id;
    double x;
    double y;
    double z;
    double delta_x;
    double delta_y;
    double delta_z;
    float y_rot;
    float x_rot;
    // attributes: #[fixed]
    std::int32_t relatives;
};

struct set_default_spawn_position {
    static constexpr std::uint16_t packet_id = 0x63;
    static constexpr std::string_view packet_name = "set_default_spawn_position";

    ::refrain::data::resource_location dimension;
    std::int64_t position;
    float yaw;
    float pitch;
};

struct set_held_slot {
    static constexpr std::uint16_t packet_id = 0x6B;
    static constexpr std::string_view packet_name = "set_held_slot";

    // attributes: #[varint]
    std::uint32_t slot;
};

struct set_time {
    static constexpr std::uint16_t packet_id = 0x73;
    static constexpr std::string_view packet_name = "set_time";

    std::int64_t game_time;
    std::vector<::refrain::protocol::v777::play::clientbound::clock_update> clock_updates;
};
[[nodiscard]] inline auto encode_global_pos(::refrain::net::writer &out, const global_pos &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.dimension); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_global_pos(::refrain::net::reader &in, global_pos &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_spawn_info(::refrain::net::writer &out, const spawn_info &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.dimension_type));
  }
  {
    if (auto written = out.write_identifier(value.dimension); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.seed));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.game_type));
  }
  {
    out.write_varint(value.previous_game_type.has_value() ? static_cast<std::uint32_t>(value.previous_game_type.value()) + 1U : 0U);
  }
  {
    out.write_bool(value.debug);
  }
  {
    out.write_bool(value.flat);
  }
  {
    out.write_bool(value.last_death_location.has_value());
    if (value.last_death_location.has_value()) {
      if (auto written = ::refrain::protocol::v777::play::clientbound::encode_global_pos(out, value.last_death_location.value()); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.portal_cooldown));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sea_level));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_spawn_info(::refrain::net::reader &in, spawn_info &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension_type = static_cast<decltype(value.dimension_type)>(*value_0);
  }
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.seed = static_cast<decltype(value.seed)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.game_type = static_cast<decltype(value.game_type)>(*value_0);
  }
  {
    auto present_0 = in.read_varint();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0 != 0) {
      value.previous_game_type = static_cast<std::uint32_t>(*present_0 - 1U);
    } else {
      value.previous_game_type.reset();
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.debug = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flat = *value_0;
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::play::clientbound::global_pos value_0{};
      if (auto decoded = ::refrain::protocol::v777::play::clientbound::decode_global_pos(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.last_death_location = std::move(value_0);
    } else {
      value.last_death_location.reset();
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.portal_cooldown = static_cast<decltype(value.portal_cooldown)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sea_level = static_cast<decltype(value.sea_level)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_clock_update(::refrain::net::writer &out, const clock_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.clock); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.state));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_clock_update(::refrain::net::reader &in, clock_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.clock = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.state = static_cast<decltype(value.state)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_changed_ack(::refrain::net::writer &out, const block_changed_ack &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.sequence));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_changed_ack(::refrain::net::reader &in, block_changed_ack &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sequence = static_cast<decltype(value.sequence)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_update(::refrain::net::writer &out, const block_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.state));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_update(::refrain::net::reader &in, block_update &value) -> ::refrain::status {
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
    value.state = static_cast<decltype(value.state)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_change_difficulty(::refrain::net::writer &out, const change_difficulty &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.difficulty));
  }
  {
    out.write_bool(value.locked);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_change_difficulty(::refrain::net::reader &in, change_difficulty &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.difficulty = static_cast<decltype(value.difficulty)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.locked = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chunk_batch_finished(::refrain::net::writer &out, const chunk_batch_finished &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.batch_size));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chunk_batch_finished(::refrain::net::reader &in, chunk_batch_finished &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.batch_size = static_cast<decltype(value.batch_size)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chunk_batch_start(::refrain::net::writer &out, const chunk_batch_start &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chunk_batch_start(::refrain::net::reader &in, chunk_batch_start &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_entity_event(::refrain::net::writer &out, const entity_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.entity_id));
  }
  {
    out.write_u8(value.event);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_entity_event(::refrain::net::reader &in, entity_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.event = static_cast<decltype(value.event)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_game_event(::refrain::net::writer &out, const game_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(value.event);
  }
  {
    out.write_f32(value.value);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_game_event(::refrain::net::reader &in, game_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.event = static_cast<decltype(value.event)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.value = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_initialize_border(::refrain::net::writer &out, const initialize_border &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.center_x);
  }
  {
    out.write_f64(value.center_z);
  }
  {
    out.write_f64(value.old_size);
  }
  {
    out.write_f64(value.new_size);
  }
  {
    out.write_varlong(static_cast<std::uint64_t>(value.lerp_time));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.absolute_max_size));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.warning_blocks));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.warning_time));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_initialize_border(::refrain::net::reader &in, initialize_border &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.center_x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.center_z = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.old_size = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.new_size = *value_0;
  }
  {
    auto value_0 = in.read_varlong();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.lerp_time = static_cast<decltype(value.lerp_time)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.absolute_max_size = static_cast<decltype(value.absolute_max_size)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.warning_blocks = static_cast<decltype(value.warning_blocks)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.warning_time = static_cast<decltype(value.warning_time)>(*value_0);
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

[[nodiscard]] inline auto encode_login(::refrain::net::writer &out, const login &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.player_id));
  }
  {
    out.write_bool(value.hardcore);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.levels.size()));
    for (const auto &item_0 : value.levels) {
      if (auto written = out.write_identifier(item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.max_players));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.chunk_radius));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.simulation_distance));
  }
  {
    out.write_bool(value.reduced_debug_info);
  }
  {
    out.write_bool(value.show_death_screen);
  }
  {
    out.write_bool(value.limited_crafting);
  }
  {
    if (auto written = ::refrain::protocol::v777::play::clientbound::encode_spawn_info(out, value.spawn); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.online_mode);
  }
  {
    out.write_bool(value.enforces_secure_chat);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_login(::refrain::net::reader &in, login &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.player_id = static_cast<decltype(value.player_id)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hardcore = *value_0;
  }
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.levels.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.levels.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.max_players = static_cast<decltype(value.max_players)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.chunk_radius = static_cast<decltype(value.chunk_radius)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.simulation_distance = static_cast<decltype(value.simulation_distance)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.reduced_debug_info = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.show_death_screen = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.limited_crafting = *value_0;
  }
  {
    if (auto decoded = ::refrain::protocol::v777::play::clientbound::decode_spawn_info(in, value.spawn); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.online_mode = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.enforces_secure_chat = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_abilities(::refrain::net::writer &out, const player_abilities &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(value.flags);
  }
  {
    out.write_f32(value.flying_speed);
  }
  {
    out.write_f32(value.walking_speed);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_abilities(::refrain::net::reader &in, player_abilities &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags = static_cast<decltype(value.flags)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flying_speed = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.walking_speed = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_position(::refrain::net::writer &out, const player_position &value) -> ::refrain::status {
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
    out.write_f64(value.delta_x);
  }
  {
    out.write_f64(value.delta_y);
  }
  {
    out.write_f64(value.delta_z);
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.relatives));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_position(::refrain::net::reader &in, player_position &value) -> ::refrain::status {
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
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.delta_x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.delta_y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.delta_z = *value_0;
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
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.relatives = static_cast<decltype(value.relatives)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_default_spawn_position(::refrain::net::writer &out, const set_default_spawn_position &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.dimension); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  {
    out.write_f32(value.yaw);
  }
  {
    out.write_f32(value.pitch);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_default_spawn_position(::refrain::net::reader &in, set_default_spawn_position &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.yaw = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pitch = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_held_slot(::refrain::net::writer &out, const set_held_slot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.slot));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_held_slot(::refrain::net::reader &in, set_held_slot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = static_cast<decltype(value.slot)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_time(::refrain::net::writer &out, const set_time &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.game_time));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.clock_updates.size()));
    for (const auto &item_0 : value.clock_updates) {
      if (auto written = ::refrain::protocol::v777::play::clientbound::encode_clock_update(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_time(::refrain::net::reader &in, set_time &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.game_time = static_cast<decltype(value.game_time)>(*value_0);
  }
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.clock_updates.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::play::clientbound::clock_update value_0{};
      if (auto decoded = ::refrain::protocol::v777::play::clientbound::decode_clock_update(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.clock_updates.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::play::clientbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_CLIENTBOUND_HPP
