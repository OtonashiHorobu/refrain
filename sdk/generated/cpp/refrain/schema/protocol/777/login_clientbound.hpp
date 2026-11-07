// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 65507ac10423ea04

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_CLIENTBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_CLIENTBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>

namespace refrain::protocol::v777::login::clientbound {

struct profile_property {
    std::string name;
    std::string value;
    std::optional<std::string> signature;
};

struct login_disconnect {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "login_disconnect";

    std::string reason;
};

struct hello {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "hello";

    // attributes: #[max(20)]
    std::string server_id;
    std::vector<std::byte> public_key;
    std::vector<std::byte> challenge;
    bool should_authenticate;
};

struct login_finished {
    static constexpr std::uint16_t packet_id = 0x2;
    static constexpr std::string_view packet_name = "login_finished";

    std::int64_t profile_high;
    std::int64_t profile_low;
    // attributes: #[max(16)]
    std::string name;
    std::vector<::refrain::protocol::v777::login::clientbound::profile_property> properties;
    std::int64_t session_high;
    std::int64_t session_low;
};

struct login_compression {
    static constexpr std::uint16_t packet_id = 0x3;
    static constexpr std::string_view packet_name = "login_compression";

    // attributes: #[varint]
    std::uint32_t threshold;
};

struct cookie_request {
    static constexpr std::uint16_t packet_id = 0x5;
    static constexpr std::string_view packet_name = "cookie_request";

    ::refrain::data::resource_location key;
};
[[nodiscard]] inline auto encode_profile_property(::refrain::net::writer &out, const profile_property &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.name, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.value, 32767); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.signature.has_value());
    if (value.signature.has_value()) {
      if (auto written = out.write_string(value.signature.value(), 32767); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_profile_property(::refrain::net::reader &in, profile_property &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.value = std::move(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::string value_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.signature = std::move(value_0);
    } else {
      value.signature.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_login_disconnect(::refrain::net::writer &out, const login_disconnect &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.reason, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_login_disconnect(::refrain::net::reader &in, login_disconnect &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.reason = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_hello(::refrain::net::writer &out, const hello &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.server_id, 20); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.public_key.size()));
    out.write_bytes(std::span<const std::byte>{value.public_key });
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.challenge.size()));
    out.write_bytes(std::span<const std::byte>{value.challenge });
  }
  {
    out.write_bool(value.should_authenticate);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_hello(::refrain::net::reader &in, hello &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(20);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.server_id = std::move(*value_0);
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.public_key = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.challenge = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.should_authenticate = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_login_finished(::refrain::net::writer &out, const login_finished &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.profile_high));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.profile_low));
  }
  {
    if (auto written = out.write_string(value.name, 16); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.properties.size()));
    for (const auto &item_0 : value.properties) {
      if (auto written = ::refrain::protocol::v777::login::clientbound::encode_profile_property(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.session_high));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.session_low));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_login_finished(::refrain::net::reader &in, login_finished &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
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
  {
    auto value_0 = in.read_string(16);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.name = std::move(*value_0);
  }
  {
    value.properties.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::login::clientbound::profile_property value_0{};
      if (auto decoded = ::refrain::protocol::v777::login::clientbound::decode_profile_property(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.properties.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.session_high = static_cast<decltype(value.session_high)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.session_low = static_cast<decltype(value.session_low)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_login_compression(::refrain::net::writer &out, const login_compression &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.threshold));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_login_compression(::refrain::net::reader &in, login_compression &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.threshold = static_cast<decltype(value.threshold)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_cookie_request(::refrain::net::writer &out, const cookie_request &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.key); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_cookie_request(::refrain::net::reader &in, cookie_request &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::login::clientbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_LOGIN_CLIENTBOUND_HPP
