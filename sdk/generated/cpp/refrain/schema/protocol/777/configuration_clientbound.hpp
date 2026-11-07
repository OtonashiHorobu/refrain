// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 7cfeb406836d8806

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_CLIENTBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_CLIENTBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/schema/protocol/777/common.hpp>

namespace refrain::protocol::v777::configuration::clientbound {

struct custom_payload {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "custom_payload";

    ::refrain::data::resource_location channel;
    std::vector<std::byte> data;
};

struct finish_configuration {
    static constexpr std::uint16_t packet_id = 0x3;
    static constexpr std::string_view packet_name = "finish_configuration";

};

struct keep_alive {
    static constexpr std::uint16_t packet_id = 0x4;
    static constexpr std::string_view packet_name = "keep_alive";

    std::int64_t id;
};

struct ping {
    static constexpr std::uint16_t packet_id = 0x5;
    static constexpr std::string_view packet_name = "ping";

    // attributes: #[fixed]
    std::int32_t id;
};

struct update_enabled_features {
    static constexpr std::uint16_t packet_id = 0xD;
    static constexpr std::string_view packet_name = "update_enabled_features";

    std::vector<::refrain::data::resource_location> features;
};

struct select_known_packs {
    static constexpr std::uint16_t packet_id = 0xF;
    static constexpr std::string_view packet_name = "select_known_packs";

    std::vector<::refrain::protocol::v777::common::known_pack> packs;
};
[[nodiscard]] inline auto encode_custom_payload(::refrain::net::writer &out, const custom_payload &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.channel); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.data.size()));
    out.write_bytes(std::span<const std::byte>{value.data });
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
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_finish_configuration(::refrain::net::writer &out, const finish_configuration &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_finish_configuration(::refrain::net::reader &in, finish_configuration &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
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

[[nodiscard]] inline auto encode_ping(::refrain::net::writer &out, const ping &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_ping(::refrain::net::reader &in, ping &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_update_enabled_features(::refrain::net::writer &out, const update_enabled_features &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.features.size()));
    for (const auto &item_0 : value.features) {
      if (auto written = out.write_identifier(item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_update_enabled_features(::refrain::net::reader &in, update_enabled_features &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.features.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.features.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_select_known_packs(::refrain::net::writer &out, const select_known_packs &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.packs.size()));
    for (const auto &item_0 : value.packs) {
      if (auto written = ::refrain::protocol::v777::common::encode_known_pack(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_select_known_packs(::refrain::net::reader &in, select_known_packs &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.packs.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::common::known_pack value_0{};
      if (auto decoded = ::refrain::protocol::v777::common::decode_known_pack(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.packs.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::configuration::clientbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_CLIENTBOUND_HPP
