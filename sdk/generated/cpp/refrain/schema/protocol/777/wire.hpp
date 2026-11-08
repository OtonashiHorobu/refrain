// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8aa1fe7cac60c1a3

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
#include <map>
#include <utility>
#include <variant>

namespace refrain::protocol::v777::wire {

struct wire_at_entity_payload {
    // attributes: #[varint]
    std::int32_t entity;
    std::uint32_t to_anchor;
};

struct wire_entries_entry {
    // attributes: #[fixed]
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

struct wire_attribute_snapshot {
    ::refrain::data::resource_location id;
    double amount;
    std::uint32_t operation;
};

struct wire_get_values {
    std::uint32_t attribute;
    double base;
    std::vector<::refrain::protocol::v777::wire::wire_attribute_snapshot> modifiers;
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
    // attributes: #[fixed]
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
    std::uint32_t rotation;
    bool ignore_entities;
    std::uint32_t status;
    std::optional<::refrain::net::text_component> error_message;
};

struct wire_untrusted_entry {
    std::variant<std::uint32_t, ::refrain::net::text_component> type_;
    // attributes: #[max(32767)]
    std::string link;
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

struct wire_chat_type_decoration {
    // attributes: #[max(32767)]
    std::string translation_key;
    std::vector<std::uint32_t> parameters;
    ::refrain::net::nbt_blob style;
};

struct wire_chat_type {
    ::refrain::protocol::v777::wire::wire_chat_type_decoration chat;
    ::refrain::protocol::v777::wire::wire_chat_type_decoration narration;
};

struct wire_bound {
    std::variant<std::uint32_t, ::refrain::protocol::v777::wire::wire_chat_type> chat_type;
    ::refrain::net::text_component name;
    std::optional<::refrain::net::text_component> target_name;
};

struct wire_packed_registry_entry {
    ::refrain::data::resource_location id;
    std::optional<::refrain::net::nbt_blob> data;
};

struct wire_sound_event {
    ::refrain::data::resource_location location;
    std::optional<float> fixed_range;
};

struct wire_explosion_particle_info {
    ::refrain::net::particle_options particle;
    float scaling;
    float speed;
};

struct wire_block_entities_data {
    // attributes: #[fixed]
    std::int8_t packed_xz;
    // attributes: #[fixed]
    std::int16_t y;
    std::uint32_t type_;
    ::refrain::net::nbt_blob tag;
};

struct wire_clientbound_level_chunk_packet_data {
    std::map<std::uint32_t, std::vector<std::int64_t>> heightmaps;
    // attributes: #[max(2097152)]
    std::vector<std::byte> buffer;
    std::vector<::refrain::protocol::v777::wire::wire_block_entities_data> block_entities_data;
};

struct wire_light_data {
    // attributes: #[max(32767)]
    std::vector<std::byte> sky_y_mask;
    // attributes: #[max(32767)]
    std::vector<std::byte> block_y_mask;
    // attributes: #[max(32767)]
    std::vector<std::byte> empty_sky_y_mask;
    // attributes: #[max(32767)]
    std::vector<std::byte> empty_block_y_mask;
    std::vector<std::vector<std::byte>> sky_updates;
    std::vector<std::vector<std::byte>> block_updates;
};

struct wire_global_pos {
    ::refrain::data::resource_location dimension;
    std::int64_t pos;
};

struct wire_common_player_spawn_info {
    std::uint32_t dimension_type;
    ::refrain::data::resource_location dimension;
    std::int64_t seed;
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

struct wire_map_decoration {
    std::uint32_t type_;
    // attributes: #[fixed]
    std::int8_t x;
    // attributes: #[fixed]
    std::int8_t y;
    // attributes: #[fixed]
    std::int8_t rot;
    std::optional<::refrain::net::nbt_blob> name;
};

struct wire_vec3 {
    double x;
    double y;
    double z;
};

struct wire_lerp_steps {
    ::refrain::protocol::v777::wire::wire_vec3 position;
    ::refrain::protocol::v777::wire::wire_vec3 movement;
    // attributes: #[fixed]
    std::int8_t y_rot;
    // attributes: #[fixed]
    std::int8_t x_rot;
    float weight;
};

struct wire_packed {
    std::vector<::refrain::net::message_signature_packed> entries;
};

struct wire_body {
    // attributes: #[max(256)]
    std::string content;
    std::int64_t time_stamp;
    std::int64_t salt;
    ::refrain::protocol::v777::wire::wire_packed last_seen;
};

struct wire_entry_2 {
    ::refrain::net::recipe_display_entry contents;
    // attributes: #[fixed]
    std::int8_t flags_;
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
    ::refrain::protocol::v777::wire::wire_type_settings crafting;
    ::refrain::protocol::v777::wire::wire_type_settings furnace;
    ::refrain::protocol::v777::wire::wire_type_settings blast_furnace;
    ::refrain::protocol::v777::wire::wire_type_settings smoker;
};

struct wire_network_payload {
    std::map<::refrain::data::resource_location, std::vector<std::uint32_t>> tags;
};

struct wire_swing_animation {
    std::uint32_t type_;
    // attributes: #[varint]
    std::int32_t duration;
};

struct wire_advancement_holder {
    ::refrain::data::resource_location id;
    ::refrain::net::advancement value;
};

struct wire_added {
    ::refrain::protocol::v777::wire::wire_advancement_holder advancement;
    float x;
    float y;
};

struct wire_weighted_entry {
    std::uint32_t weight;
    ::refrain::protocol::v777::wire::wire_explosion_particle_info value;
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
[[nodiscard]] inline auto encode_wire_at_entity_payload(::refrain::net::writer &out, const wire_at_entity_payload &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity));
  }
  {
    out.write_varint(value.to_anchor);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_at_entity_payload(::refrain::net::reader &in, wire_at_entity_payload &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity = static_cast<decltype(value.entity)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.to_anchor = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_entries_entry(::refrain::net::writer &out, const wire_entries_entry &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(static_cast<std::uint8_t>(value.field_0));
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
    auto value_0 = in.read_u8();
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

[[nodiscard]] inline auto encode_wire_attribute_snapshot(::refrain::net::writer &out, const wire_attribute_snapshot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.id); !written) {
      return written;
    }
  }
  {
    out.write_f64(value.amount);
  }
  {
    out.write_varint(value.operation);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_attribute_snapshot(::refrain::net::reader &in, wire_attribute_snapshot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = std::move(*value_0);
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.amount = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.operation = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_get_values(::refrain::net::writer &out, const wire_get_values &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.attribute);
  }
  {
    out.write_f64(value.base);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.modifiers.size()));
    for (const auto &item_0 : value.modifiers) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_attribute_snapshot(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_get_values(::refrain::net::reader &in, wire_get_values &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.attribute = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.base = *value_0;
  }
  {
    value.modifiers.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_attribute_snapshot value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_attribute_snapshot(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.modifiers.push_back(std::move(value_0));
    }
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
    value.entries.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
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
    out.write_u8(static_cast<std::uint8_t>(value.checksum));
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
    auto value_0 = in.read_u8();
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
    out.write_varint(value.rotation);
  }
  {
    out.write_bool(value.ignore_entities);
  }
  {
    out.write_varint(value.status);
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
    value.rotation = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.ignore_entities = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.status = *value_0;
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

[[nodiscard]] inline auto encode_wire_untrusted_entry(::refrain::net::writer &out, const wire_untrusted_entry &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(std::holds_alternative<std::uint32_t>(value.type_));
    if (std::holds_alternative<std::uint32_t>(value.type_)) {
      out.write_varint(std::get<0>(value.type_));
    } else {
      if (auto written = ::refrain::net::encode_text_component(out, std::get<1>(value.type_)); !written) {
        return written;
      }
    }
  }
  {
    if (auto written = out.write_string(value.link, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_untrusted_entry(::refrain::net::reader &in, wire_untrusted_entry &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto left_0 = in.read_bool();
    if (!left_0) return std::unexpected{std::move(left_0.error())};
    if (*left_0) {
      std::uint32_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.type_.template emplace<0>(std::move(value_0));
    } else {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.type_.template emplace<1>(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.link = std::move(*value_0);
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

[[nodiscard]] inline auto encode_wire_chat_type_decoration(::refrain::net::writer &out, const wire_chat_type_decoration &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.translation_key, 32767); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.parameters.size()));
    for (const auto &item_0 : value.parameters) {
      out.write_varint(item_0);
    }
  }
  {
    if (auto written = ::refrain::net::encode_nbt(out, value.style); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_chat_type_decoration(::refrain::net::reader &in, wire_chat_type_decoration &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.translation_key = std::move(*value_0);
  }
  {
    value.parameters.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::uint32_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.parameters.push_back(std::move(value_0));
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_nbt(in, value.style); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_chat_type(::refrain::net::writer &out, const wire_chat_type &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_chat_type_decoration(out, value.chat); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_chat_type_decoration(out, value.narration); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_chat_type(::refrain::net::reader &in, wire_chat_type &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_chat_type_decoration(in, value.chat); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_chat_type_decoration(in, value.narration); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_bound(::refrain::net::writer &out, const wire_bound &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (std::holds_alternative<std::uint32_t>(value.chat_type)) {
      out.write_varint(std::get<0>(value.chat_type));
    } else {
      out.write_varint(0);
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_chat_type(out, std::get<1>(value.chat_type)); !written) {
        return written;
      }
    }
  }
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.name); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.target_name.has_value());
    if (value.target_name.has_value()) {
      if (auto written = ::refrain::net::encode_text_component(out, value.target_name.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_bound(::refrain::net::reader &in, wire_bound &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto holder_0 = in.read_varint();
    if (!holder_0) return std::unexpected{std::move(holder_0.error())};
    if (*holder_0 != 0) {
      value.chat_type.template emplace<0>(*holder_0);
    } else {
      ::refrain::protocol::v777::wire::wire_chat_type value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_chat_type(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.chat_type.template emplace<1>(std::move(value_0));
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.name); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.target_name = std::move(value_0);
    } else {
      value.target_name.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_packed_registry_entry(::refrain::net::writer &out, const wire_packed_registry_entry &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.id); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.data.has_value());
    if (value.data.has_value()) {
      if (auto written = ::refrain::net::encode_nbt(out, value.data.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_packed_registry_entry(::refrain::net::reader &in, wire_packed_registry_entry &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = std::move(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::nbt_blob value_0{};
      if (auto decoded = ::refrain::net::decode_nbt(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.data = std::move(value_0);
    } else {
      value.data.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_sound_event(::refrain::net::writer &out, const wire_sound_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.location); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.fixed_range.has_value());
    if (value.fixed_range.has_value()) {
      out.write_f32(value.fixed_range.value());
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_sound_event(::refrain::net::reader &in, wire_sound_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.location = std::move(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      float value_0{};
      auto value_1 = in.read_f32();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.fixed_range = std::move(value_0);
    } else {
      value.fixed_range.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_explosion_particle_info(::refrain::net::writer &out, const wire_explosion_particle_info &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_particle_options(out, value.particle); !written) {
      return written;
    }
  }
  {
    out.write_f32(value.scaling);
  }
  {
    out.write_f32(value.speed);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_explosion_particle_info(::refrain::net::reader &in, wire_explosion_particle_info &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_particle_options(in, value.particle); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.scaling = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.speed = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_block_entities_data(::refrain::net::writer &out, const wire_block_entities_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(static_cast<std::uint8_t>(value.packed_xz));
  }
  {
    out.write_u16(static_cast<std::uint16_t>(value.y));
  }
  {
    out.write_varint(value.type_);
  }
  {
    if (auto written = ::refrain::net::encode_nbt(out, value.tag); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_block_entities_data(::refrain::net::reader &in, wire_block_entities_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.packed_xz = static_cast<decltype(value.packed_xz)>(*value_0);
  }
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = static_cast<decltype(value.y)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.type_ = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_nbt(in, value.tag); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_clientbound_level_chunk_packet_data(::refrain::net::writer &out, const wire_clientbound_level_chunk_packet_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.heightmaps.size()));
    for (const auto &entry_0 : value.heightmaps) {
      out.write_varint(entry_0.first);
      out.write_varint(static_cast<std::uint32_t>(entry_0.second.size()));
      for (const auto &item_1 : entry_0.second) {
        out.write_i64(static_cast<std::int64_t>(item_1));
      }
    }
  }
  {
    if (value.buffer.size() > 2097152) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'buffer' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.buffer.size()));
    out.write_bytes(std::span<const std::byte>{value.buffer });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.block_entities_data.size()));
    for (const auto &item_0 : value.block_entities_data) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_block_entities_data(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_clientbound_level_chunk_packet_data(::refrain::net::reader &in, wire_clientbound_level_chunk_packet_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.heightmaps.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::uint32_t map_key_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      map_key_0 = *value_1;
      std::vector<std::int64_t> map_value_0{};
      map_value_0.clear();
      auto count_2 = in.read_varint();
      if (!count_2) return std::unexpected{std::move(count_2.error())};
      for (std::uint32_t index_2 = 0; index_2 < *count_2; ++index_2) {
        std::int64_t value_2{};
        auto value_3 = in.read_i64();
        if (!value_3) return std::unexpected{std::move(value_3.error())};
        value_2 = static_cast<decltype(value_2)>(*value_3);
        map_value_0.push_back(std::move(value_2));
      }
      value.heightmaps.emplace(std::move(map_key_0), std::move(map_value_0));
    }
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
  {
    value.block_entities_data.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_block_entities_data value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_block_entities_data(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.block_entities_data.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_light_data(::refrain::net::writer &out, const wire_light_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (value.sky_y_mask.size() > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'sky_y_mask' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.sky_y_mask.size()));
    out.write_bytes(std::span<const std::byte>{value.sky_y_mask });
  }
  {
    if (value.block_y_mask.size() > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'block_y_mask' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.block_y_mask.size()));
    out.write_bytes(std::span<const std::byte>{value.block_y_mask });
  }
  {
    if (value.empty_sky_y_mask.size() > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'empty_sky_y_mask' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.empty_sky_y_mask.size()));
    out.write_bytes(std::span<const std::byte>{value.empty_sky_y_mask });
  }
  {
    if (value.empty_block_y_mask.size() > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'empty_block_y_mask' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.empty_block_y_mask.size()));
    out.write_bytes(std::span<const std::byte>{value.empty_block_y_mask });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sky_updates.size()));
    for (const auto &item_0 : value.sky_updates) {
      out.write_varint(static_cast<std::uint32_t>(item_0.size()));
      out.write_bytes(std::span<const std::byte>{item_0 });
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.block_updates.size()));
    for (const auto &item_0 : value.block_updates) {
      out.write_varint(static_cast<std::uint32_t>(item_0.size()));
      out.write_bytes(std::span<const std::byte>{item_0 });
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_light_data(::refrain::net::reader &in, wire_light_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'sky_y_mask' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sky_y_mask = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'block_y_mask' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.block_y_mask = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'empty_sky_y_mask' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.empty_sky_y_mask = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 32767) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'empty_block_y_mask' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.empty_block_y_mask = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    value.sky_updates.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::vector<std::byte> value_0{};
      auto length_1 = in.read_varint();
      if (!length_1) return std::unexpected{std::move(length_1.error())};
      auto value_1 = in.read_bytes(*length_1);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::vector<std::byte>{value_1->begin(), value_1->end()};
      value.sky_updates.push_back(std::move(value_0));
    }
  }
  {
    value.block_updates.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::vector<std::byte> value_0{};
      auto length_1 = in.read_varint();
      if (!length_1) return std::unexpected{std::move(length_1.error())};
      auto value_1 = in.read_bytes(*length_1);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::vector<std::byte>{value_1->begin(), value_1->end()};
      value.block_updates.push_back(std::move(value_0));
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
    out.write_varint(value.dimension_type);
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
    out.write_varint(value.game_type);
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
    value.dimension_type = *value_0;
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
    value.game_type = *value_0;
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

[[nodiscard]] inline auto encode_wire_map_decoration(::refrain::net::writer &out, const wire_map_decoration &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.type_);
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.x));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.y));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.rot));
  }
  {
    out.write_bool(value.name.has_value());
    if (value.name.has_value()) {
      if (auto written = ::refrain::net::encode_nbt(out, value.name.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_map_decoration(::refrain::net::reader &in, wire_map_decoration &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.type_ = *value_0;
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = static_cast<decltype(value.x)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = static_cast<decltype(value.y)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.rot = static_cast<decltype(value.rot)>(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::nbt_blob value_0{};
      if (auto decoded = ::refrain::net::decode_nbt(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.name = std::move(value_0);
    } else {
      value.name.reset();
    }
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
    out.write_u8(static_cast<std::uint8_t>(value.y_rot));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.x_rot));
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
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = static_cast<decltype(value.y_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
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

[[nodiscard]] inline auto encode_wire_packed(::refrain::net::writer &out, const wire_packed &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &item_0 : value.entries) {
      if (auto written = ::refrain::net::encode_message_signature_packed(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_packed(::refrain::net::reader &in, wire_packed &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.entries.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::net::message_signature_packed value_0{};
      if (auto decoded = ::refrain::net::decode_message_signature_packed(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.entries.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_body(::refrain::net::writer &out, const wire_body &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.content, 256); !written) {
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
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_packed(out, value.last_seen); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_body(::refrain::net::reader &in, wire_body &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(256);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.content = std::move(*value_0);
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
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_packed(in, value.last_seen); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_entry_2(::refrain::net::writer &out, const wire_entry_2 &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_recipe_display_entry(out, value.contents); !written) {
      return written;
    }
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.flags_));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_entry_2(::refrain::net::reader &in, wire_entry_2 &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_recipe_display_entry(in, value.contents); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags_ = static_cast<decltype(value.flags_)>(*value_0);
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
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.crafting); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.furnace); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.blast_furnace); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_type_settings(out, value.smoker); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_book_settings(::refrain::net::reader &in, wire_book_settings &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.crafting); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.furnace); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.blast_furnace); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_type_settings(in, value.smoker); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_network_payload(::refrain::net::writer &out, const wire_network_payload &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.tags.size()));
    for (const auto &entry_0 : value.tags) {
      if (auto written = out.write_identifier(entry_0.first); !written) {
        return written;
      }
      out.write_varint(static_cast<std::uint32_t>(entry_0.second.size()));
      for (const auto &item_1 : entry_0.second) {
        out.write_varint(item_1);
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_network_payload(::refrain::net::reader &in, wire_network_payload &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.tags.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location map_key_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      map_key_0 = std::move(*value_1);
      std::vector<std::uint32_t> map_value_0{};
      map_value_0.clear();
      auto count_2 = in.read_varint();
      if (!count_2) return std::unexpected{std::move(count_2.error())};
      for (std::uint32_t index_2 = 0; index_2 < *count_2; ++index_2) {
        std::uint32_t value_2{};
        auto value_3 = in.read_varint();
        if (!value_3) return std::unexpected{std::move(value_3.error())};
        value_2 = *value_3;
        map_value_0.push_back(std::move(value_2));
      }
      value.tags.emplace(std::move(map_key_0), std::move(map_value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_swing_animation(::refrain::net::writer &out, const wire_swing_animation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.type_);
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
    value.type_ = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.duration = static_cast<decltype(value.duration)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_advancement_holder(::refrain::net::writer &out, const wire_advancement_holder &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.id); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::net::encode_advancement(out, value.value); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_advancement_holder(::refrain::net::reader &in, wire_advancement_holder &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = std::move(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_advancement(in, value.value); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_added(::refrain::net::writer &out, const wire_added &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_advancement_holder(out, value.advancement); !written) {
      return written;
    }
  }
  {
    out.write_f32(value.x);
  }
  {
    out.write_f32(value.y);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_added(::refrain::net::reader &in, wire_added &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_advancement_holder(in, value.advancement); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_wire_weighted_entry(::refrain::net::writer &out, const wire_weighted_entry &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.weight);
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_explosion_particle_info(out, value.value); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_wire_weighted_entry(::refrain::net::reader &in, wire_weighted_entry &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.weight = *value_0;
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_explosion_particle_info(in, value.value); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
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
