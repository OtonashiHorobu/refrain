// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: d4cf695ff961cab1

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_SERVERBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_SERVERBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>

namespace refrain::protocol::v777::login::serverbound {

struct hello {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "hello";

    // attributes: #[max(16)]
    std::string name;
    std::int64_t profile_high;
    std::int64_t profile_low;
};

struct key {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "key";

    std::vector<std::byte> keybytes;
    std::vector<std::byte> encrypted_challenge;
};

struct login_acknowledged {
    static constexpr std::uint16_t packet_id = 0x3;
    static constexpr std::string_view packet_name = "login_acknowledged";

};

struct custom_query_answer {
    static constexpr std::uint16_t packet_id = 0x2;
    static constexpr std::string_view packet_name = "custom_query_answer";

    std::uint32_t transaction_id;
    bool has_payload;
    // attributes: #[rest]
    std::vector<std::byte> data;
};

struct cookie_response {
    static constexpr std::uint16_t packet_id = 0x4;
    static constexpr std::string_view packet_name = "cookie_response";

    ::refrain::data::resource_location key;
    std::optional<std::vector<std::byte>> payload;
};
[[nodiscard]] inline auto encode_hello(::refrain::net::writer &out, const hello &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.name, 16); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.profile_high));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.profile_low));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_hello(::refrain::net::reader &in, hello &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(16);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.profile_high = static_cast<decltype(value.profile_high)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.profile_low = static_cast<decltype(value.profile_low)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_key(::refrain::net::writer &out, const key &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.keybytes.size()));
    out.write_bytes(std::span<const std::byte>{value.keybytes });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.encrypted_challenge.size()));
    out.write_bytes(std::span<const std::byte>{value.encrypted_challenge });
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_key(::refrain::net::reader &in, key &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.keybytes = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.encrypted_challenge = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_login_acknowledged(::refrain::net::writer &out, const login_acknowledged &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_login_acknowledged(::refrain::net::reader &in, login_acknowledged &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_custom_query_answer(::refrain::net::writer &out, const custom_query_answer &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.transaction_id);
  }
  {
    out.write_bool(value.has_payload);
  }
  {
    out.write_bytes(value.data);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_custom_query_answer(::refrain::net::reader &in, custom_query_answer &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.transaction_id = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.has_payload = *value_0;
  }
  {
    auto value_0 = in.read_bytes(in.remaining());
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = std::vector<std::byte>{value_0->begin(), value_0->end()};
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

} // namespace refrain::protocol::v777::login::serverbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_SERVERBOUND_HPP
