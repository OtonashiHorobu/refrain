// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: d5b1a43efe750e10

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_WIRE_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/net/wire.hpp>

namespace refrain::protocol::v777::wire {

struct wire_entries_entry {
    std::int8_t field_0;
    ::refrain::net::item_stack field_1;
};

struct wire_message_signature {
    // attributes: #[fixed_bytes(256)]
    std::vector<std::byte> field_0;
};

struct wire_optional_payload {
    double field_0;
    double field_1;
    double field_2;
};

struct wire_entry_3 {
    // attributes: #[max(16)]
    std::string name;
    ::refrain::protocol::v777::wire::wire_message_signature signature;
};

struct wire_argument_signatures {
    std::vector<::refrain::protocol::v777::wire::wire_entry_3> entries;
};

struct wire_update {
    // attributes: #[varint]
    std::int32_t offset;
    // attributes: #[fixed_bytes(20)]
    std::vector<std::byte> acknowledged;
    std::int8_t checksum;
};

struct wire_data {
    std::int64_t expires_at;
    // attributes: #[max(512)]
    std::vector<std::byte> key;
    // attributes: #[max(4096)]
    std::vector<std::byte> key_signature;
};

struct wire_entry_4 {
    ::refrain::data::resource_location game_rule_key;
    // attributes: #[max(32767)]
    std::string value;
};

struct wire_vec3i {
    // attributes: #[varint]
    std::int32_t x;
    // attributes: #[varint]
    std::int32_t y;
    // attributes: #[varint]
    std::int32_t z;
};

struct wire_data_3 {
    std::optional<::refrain::data::resource_location> test;
    ::refrain::protocol::v777::wire::wire_vec3i size;
    // attributes: #[varint]
    std::uint32_t rotation;
    bool ignore_entities;
    // attributes: #[varint]
    std::int32_t status;
    std::optional<::refrain::net::text_component> error_message;
};

struct wire_chunk_biome_data {
    std::int64_t pos;
    // attributes: #[max(2097152)]
    std::vector<std::byte> buffer;
};

struct wire_entry {
    // attributes: #[max(32767)]
    std::string text;
    std::optional<::refrain::net::text_component> tooltip;
};

struct wire_global_pos {
    ::refrain::data::resource_location dimension;
    std::int64_t pos;
};

struct wire_common_player_spawn_info {
    // attributes: #[varint]
    std::int32_t dimension_type;
    ::refrain::data::resource_location dimension;
    std::int64_t seed;
    // attributes: #[varint]
    std::uint32_t game_type;
    std::optional<std::uint32_t> previous_game_type;
    bool is_debug;
    bool is_flat;
    std::optional<::refrain::protocol::v777::wire::wire_global_pos> last_death_location;
    // attributes: #[varint]
    std::int32_t portal_cooldown;
    // attributes: #[varint]
    std::int32_t sea_level;
};

struct wire_vec3 {
    double x;
    double y;
    double z;
};

struct wire_lerp_steps {
    ::refrain::protocol::v777::wire::wire_vec3 position;
    ::refrain::protocol::v777::wire::wire_vec3 movement;
    std::int8_t y_rot;
    std::int8_t x_rot;
    float weight;
};

struct wire_recipe_display_id {
    // attributes: #[varint]
    std::int32_t index;
};

struct wire_type_settings {
    bool open;
    bool filtering;
};

struct wire_book_settings {
    ::refrain::protocol::v777::wire::wire_type_settings o_o_crafting;
    ::refrain::protocol::v777::wire::wire_type_settings o_o_furnace;
    ::refrain::protocol::v777::wire::wire_type_settings o_o_blast_furnace;
    ::refrain::protocol::v777::wire::wire_type_settings o_o_smoker;
};

struct wire_swing_animation {
    // attributes: #[varint]
    std::int32_t type_;
    // attributes: #[varint]
    std::int32_t duration;
};

struct wire_position_and_rotation {
    ::refrain::protocol::v777::wire::wire_vec3 position;
    float y_rot;
    float x_rot;
};

struct wire_position_move_rotation {
    ::refrain::protocol::v777::wire::wire_vec3 position;
    ::refrain::protocol::v777::wire::wire_vec3 delta_movement;
    float y_rot;
    float x_rot;
};
[[nodiscard]] inline auto encode_wire_entries_entry(::refrain::net::writer &out, const wire_entries_entry &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.field_0));
  }
  {
    if (auto written = ::refrain::net::encode_item_stack(out, value.field_1); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_entries_entry(::refrain::net::reader &in, wire_entries_entry &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.field_0 = static_cast<decltype(value.field_0)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_item_stack(in, value.field_1); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_message_signature(::refrain::net::writer &out, const wire_message_signature &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (value.field_0.size() != 256) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'field_0' must be exactly 256 byte(s)")};
    }
    out.write_bytes(std::span<const std::byte>{value.field_0 });
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_message_signature(::refrain::net::reader &in, wire_message_signature &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_bytes(256);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.field_0 = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_optional_payload(::refrain::net::writer &out, const wire_optional_payload &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.field_0);
  }
  {
    out.write_f64(value.field_1);
  }
  {
    out.write_f64(value.field_2);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_optional_payload(::refrain::net::reader &in, wire_optional_payload &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.field_0 = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.field_1 = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.field_2 = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_entry_3(::refrain::net::writer &out, const wire_entry_3 &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.name, 16); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_message_signature(out, value.signature); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_entry_3(::refrain::net::reader &in, wire_entry_3 &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(16);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_message_signature(in, value.signature); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_argument_signatures(::refrain::net::writer &out, const wire_argument_signatures &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &item_0 : value.entries) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_entry_3(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_argument_signatures(::refrain::net::reader &in, wire_argument_signatures &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.entries.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_entry_3 value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_entry_3(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.entries.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_update(::refrain::net::writer &out, const wire_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
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
    out.write_i64(static_cast<std::int64_t>(value.checksum));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_update(::refrain::net::reader &in, wire_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
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
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.checksum = static_cast<decltype(value.checksum)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_data(::refrain::net::writer &out, const wire_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.expires_at));
  }
  {
    if (value.key.size() > 512) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'key' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.key.size()));
    out.write_bytes(std::span<const std::byte>{value.key });
  }
  {
    if (value.key_signature.size() > 4096) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'key_signature' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.key_signature.size()));
    out.write_bytes(std::span<const std::byte>{value.key_signature });
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_data(::refrain::net::reader &in, wire_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.expires_at = static_cast<decltype(value.expires_at)>(*value_0);
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 512) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'key' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 4096) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'key_signature' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key_signature = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_entry_4(::refrain::net::writer &out, const wire_entry_4 &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.game_rule_key); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.value, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_entry_4(::refrain::net::reader &in, wire_entry_4 &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.game_rule_key = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.value = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_vec3i(::refrain::net::writer &out, const wire_vec3i &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.x));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.y));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.z));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_vec3i(::refrain::net::reader &in, wire_vec3i &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = static_cast<decltype(value.x)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = static_cast<decltype(value.y)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = static_cast<decltype(value.z)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_data_3(::refrain::net::writer &out, const wire_data_3 &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.test.has_value());
    if (value.test.has_value()) {
      if (auto written = out.write_identifier(value.test.value()); !written) {
        return written;
      }
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3i(out, value.size); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.rotation));
  }
  {
    out.write_bool(value.ignore_entities);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.status));
  }
  {
    out.write_bool(value.error_message.has_value());
    if (value.error_message.has_value()) {
      if (auto written = ::refrain::net::encode_text_component(out, value.error_message.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_data_3(::refrain::net::reader &in, wire_data_3 &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.test = std::move(value_0);
    } else {
      value.test.reset();
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3i(in, value.size); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.rotation = static_cast<decltype(value.rotation)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.ignore_entities = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.status = static_cast<decltype(value.status)>(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.error_message = std::move(value_0);
    } else {
      value.error_message.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_chunk_biome_data(::refrain::net::writer &out, const wire_chunk_biome_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    if (value.buffer.size() > 2097152) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'buffer' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.buffer.size()));
    out.write_bytes(std::span<const std::byte>{value.buffer });
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_chunk_biome_data(::refrain::net::reader &in, wire_chunk_biome_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 2097152) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'buffer' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.buffer = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_entry(::refrain::net::writer &out, const wire_entry &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.text, 32767); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.tooltip.has_value());
    if (value.tooltip.has_value()) {
      if (auto written = ::refrain::net::encode_text_component(out, value.tooltip.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_entry(::refrain::net::reader &in, wire_entry &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.text = std::move(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.tooltip = std::move(value_0);
    } else {
      value.tooltip.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_global_pos(::refrain::net::writer &out, const wire_global_pos &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.dimension); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_global_pos(::refrain::net::reader &in, wire_global_pos &value) -> ::refrain::status {
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
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_common_player_spawn_info(::refrain::net::writer &out, const wire_common_player_spawn_info &value) -> ::refrain::status {
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
    out.write_bool(value.previous_game_type.has_value());
    if (value.previous_game_type.has_value()) {
      out.write_varint(static_cast<std::uint32_t>(value.previous_game_type.value()));
    }
  }
  {
    out.write_bool(value.is_debug);
  }
  {
    out.write_bool(value.is_flat);
  }
  {
    out.write_bool(value.last_death_location.has_value());
    if (value.last_death_location.has_value()) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_global_pos(out, value.last_death_location.value()); !written) {
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

[[nodiscard]] inline auto decode_wire_common_player_spawn_info(::refrain::net::reader &in, wire_common_player_spawn_info &value) -> ::refrain::status {
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
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::uint32_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = static_cast<decltype(value_0)>(*value_1);
      value.previous_game_type = std::move(value_0);
    } else {
      value.previous_game_type.reset();
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.is_debug = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.is_flat = *value_0;
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::wire::wire_global_pos value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_global_pos(in, value_0); !decoded) {
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

[[nodiscard]] inline auto encode_wire_vec3(::refrain::net::writer &out, const wire_vec3 &value) -> ::refrain::status {
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
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_vec3(::refrain::net::reader &in, wire_vec3 &value) -> ::refrain::status {
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
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_lerp_steps(::refrain::net::writer &out, const wire_lerp_steps &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3(out, value.position); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3(out, value.movement); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.y_rot));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.x_rot));
  }
  {
    out.write_f32(value.weight);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_lerp_steps(::refrain::net::reader &in, wire_lerp_steps &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3(in, value.position); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3(in, value.movement); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = static_cast<decltype(value.y_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = static_cast<decltype(value.x_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.weight = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_recipe_display_id(::refrain::net::writer &out, const wire_recipe_display_id &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.index));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_recipe_display_id(::refrain::net::reader &in, wire_recipe_display_id &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.index = static_cast<decltype(value.index)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_type_settings(::refrain::net::writer &out, const wire_type_settings &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.open);
  }
  {
    out.write_bool(value.filtering);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_type_settings(::refrain::net::reader &in, wire_type_settings &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.open = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.filtering = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_book_settings(::refrain::net::writer &out, const wire_book_settings &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.o_o_crafting); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.o_o_furnace); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.o_o_blast_furnace); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.o_o_smoker); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_book_settings(::refrain::net::reader &in, wire_book_settings &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.o_o_crafting); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.o_o_furnace); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.o_o_blast_furnace); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.o_o_smoker); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_swing_animation(::refrain::net::writer &out, const wire_swing_animation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.type_));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.duration));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_swing_animation(::refrain::net::reader &in, wire_swing_animation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.type_ = static_cast<decltype(value.type_)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.duration = static_cast<decltype(value.duration)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_position_and_rotation(::refrain::net::writer &out, const wire_position_and_rotation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3(out, value.position); !written) {
      return written;
    }
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_position_and_rotation(::refrain::net::reader &in, wire_position_and_rotation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3(in, value.position); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
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

[[nodiscard]] inline auto encode_wire_position_move_rotation(::refrain::net::writer &out, const wire_position_move_rotation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3(out, value.position); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3(out, value.delta_movement); !written) {
      return written;
    }
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_position_move_rotation(::refrain::net::reader &in, wire_position_move_rotation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3(in, value.position); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3(in, value.delta_movement); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
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

} // namespace refrain::protocol::v777::wire

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_WIRE_HPP
