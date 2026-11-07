// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 42ffb9d36f8b9f98

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_SERVERBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_SERVERBOUND_HPP

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

namespace refrain::protocol::v777::configuration::serverbound {

struct client_information {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "client_information";

    ::refrain::protocol::v777::common::client_information_data information;
};

struct cookie_response {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "cookie_response";

    ::refrain::data::resource_location key;
    std::optional<std::vector<std::byte>> payload;
};

struct custom_payload {
    static constexpr std::uint16_t packet_id = 0x2;
    static constexpr std::string_view packet_name = "custom_payload";

    ::refrain::data::resource_location channel;
    // attributes: #[rest]
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

struct pong {
    static constexpr std::uint16_t packet_id = 0x5;
    static constexpr std::string_view packet_name = "pong";

    // attributes: #[fixed]
    std::int32_t id;
};

struct resource_pack {
    static constexpr std::uint16_t packet_id = 0x6;
    static constexpr std::string_view packet_name = "resource_pack";

    ::refrain::net::uuid id;
    ::refrain::protocol::v777::common::resource_pack_action action;
};

struct select_known_packs {
    static constexpr std::uint16_t packet_id = 0x7;
    static constexpr std::string_view packet_name = "select_known_packs";

    std::vector<::refrain::protocol::v777::common::known_pack> packs;
};

struct accept_code_of_conduct {
    static constexpr std::uint16_t packet_id = 0x9;
    static constexpr std::string_view packet_name = "accept_code_of_conduct";

};

struct custom_click_action {
    static constexpr std::uint16_t packet_id = 0x8;
    static constexpr std::string_view packet_name = "custom_click_action";

    ::refrain::data::resource_location id;
    ::refrain::net::nbt_blob payload;
};
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

[[nodiscard]] inline auto encode_resource_pack(::refrain::net::writer &out, const resource_pack &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_uuid(value.id); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.action));
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
    value.action = static_cast<decltype(value.action)>(*value_0);
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

[[nodiscard]] inline auto encode_accept_code_of_conduct(::refrain::net::writer &out, const accept_code_of_conduct &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_accept_code_of_conduct(::refrain::net::reader &in, accept_code_of_conduct &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
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

} // namespace refrain::protocol::v777::configuration::serverbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_SERVERBOUND_HPP
