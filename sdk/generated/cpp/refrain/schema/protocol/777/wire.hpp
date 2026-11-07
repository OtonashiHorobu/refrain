// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: b600db2aac55a885

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_WIRE_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/net/wire.hpp>

namespace refrain::protocol::v777::wire {

struct wire_entry {
    // attributes: #[max(32767)]
    std::string text;
    std::optional<::refrain::net::text_component> tooltip;
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

struct wire_vec3i {
    // attributes: #[varint]
    std::int32_t x;
    // attributes: #[varint]
    std::int32_t y;
    // attributes: #[varint]
    std::int32_t z;
};
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

} // namespace refrain::protocol::v777::wire

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_WIRE_HPP
