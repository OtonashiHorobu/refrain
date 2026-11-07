// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: c23e841bac0cde6b

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_SERVERBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_SERVERBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

namespace refrain::protocol::v777::status::serverbound {

/// Empty status request.
struct status_request {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "status_request";

};

/// Latency probe payload.
struct ping_request {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "ping_request";

    std::int64_t time;
};
[[nodiscard]] inline auto encode_status_request(::refrain::net::writer &out, const status_request &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_status_request(::refrain::net::reader &in, status_request &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_ping_request(::refrain::net::writer &out, const ping_request &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.time));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_ping_request(::refrain::net::reader &in, ping_request &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.time = static_cast<decltype(value.time)>(*value_0);
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::status::serverbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_STATUS_SERVERBOUND_HPP
