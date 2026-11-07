// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 4cc80e4fd118f796

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_CLIENTBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_CLIENTBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

namespace refrain::protocol::v777::status::clientbound {

/// Server list response: a JSON document carried as one length-prefixed string.
struct status_response {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "status_response";

    std::string json;
};

/// Echo of the client's ping payload.
struct pong_response {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "pong_response";

    std::int64_t time;
};
[[nodiscard]] inline auto encode_status_response(::refrain::net::writer &out, const status_response &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.json, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_status_response(::refrain::net::reader &in, status_response &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.json = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_pong_response(::refrain::net::writer &out, const pong_response &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.time));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_pong_response(::refrain::net::reader &in, pong_response &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.time = static_cast<decltype(value.time)>(*value_0);
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::status::clientbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_CLIENTBOUND_HPP
