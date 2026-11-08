// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8a20087cf2517c49

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_HANDSHAKE_SERVERBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_HANDSHAKE_SERVERBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

namespace refrain::protocol::v777::handshake::serverbound {

struct intention {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "intention";

    // attributes: #[varint]
    std::uint32_t protocol;
    // attributes: #[max(255)]
    std::string host;
    std::uint16_t port;
    // attributes: #[varint]
    std::uint32_t next;
};
[[nodiscard]] inline auto encode_intention(::refrain::net::writer &out, const intention &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.protocol));
  }
  {
    if (auto written = out.write_string(value.host, 255); !written) {
      return written;
    }
  }
  {
    out.write_u16(value.port);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.next));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_intention(::refrain::net::reader &in, intention &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.protocol = static_cast<decltype(value.protocol)>(*value_0);
  }
  {
    auto value_0 = in.read_string(255);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.host = std::move(*value_0);
  }
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.port = static_cast<decltype(value.port)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.next = static_cast<decltype(value.next)>(*value_0);
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::handshake::serverbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_HANDSHAKE_SERVERBOUND_HPP
