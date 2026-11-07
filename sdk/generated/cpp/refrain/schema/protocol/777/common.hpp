// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: d4cf695ff961cab1

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_COMMON_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

namespace refrain::protocol::v777::common {

enum class chat_visibility : std::uint32_t {
    full = 0,
    system = 1,
    hidden = 2,
};

enum class humanoid_arm : std::uint32_t {
    left = 0,
    right = 1,
};

enum class particle_status : std::uint32_t {
    all = 0,
    decreased = 1,
    minimal = 2,
};

/// Client acknowledgement state for a pushed resource pack.
enum class resource_pack_action : std::uint32_t {
    successfully_loaded = 0,
    declined = 1,
    failed_download = 2,
    accepted = 3,
    downloaded = 4,
    invalid_url = 5,
    failed_reload = 6,
    discarded = 7,
};

/// One entry of the client/server known-pack exchange.
struct known_pack {
    std::string pack_namespace;
    std::string id;
    std::string version;
};

/// The client's display and accessibility settings. The same payload is sent
/// during configuration and again from the play stage when settings change.
struct client_information_data {
    // attributes: #[max(16)]
    std::string language;
    std::uint8_t view_distance;
    ::refrain::protocol::v777::common::chat_visibility visibility;
    bool chat_colors;
    std::uint8_t model_customization;
    ::refrain::protocol::v777::common::humanoid_arm main_hand;
    bool text_filtering;
    bool allows_listing;
    ::refrain::protocol::v777::common::particle_status particles;
};
[[nodiscard]] inline auto encode_known_pack(::refrain::net::writer &out, const known_pack &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.pack_namespace, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.id, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.version, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_known_pack(::refrain::net::reader &in, known_pack &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pack_namespace = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.version = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_client_information_data(::refrain::net::writer &out, const client_information_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.language, 16); !written) {
      return written;
    }
  }
  {
    out.write_u8(value.view_distance);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.visibility));
  }
  {
    out.write_bool(value.chat_colors);
  }
  {
    out.write_u8(value.model_customization);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.main_hand));
  }
  {
    out.write_bool(value.text_filtering);
  }
  {
    out.write_bool(value.allows_listing);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.particles));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_client_information_data(::refrain::net::reader &in, client_information_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(16);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.language = std::move(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.view_distance = static_cast<decltype(value.view_distance)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.visibility = static_cast<decltype(value.visibility)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.chat_colors = *value_0;
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.model_customization = static_cast<decltype(value.model_customization)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.main_hand = static_cast<decltype(value.main_hand)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.text_filtering = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.allows_listing = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.particles = static_cast<decltype(value.particles)>(*value_0);
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::common

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_COMMON_HPP
