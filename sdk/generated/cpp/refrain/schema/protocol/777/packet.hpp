// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 4d4424e4f6b78480

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PACKET_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PACKET_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <refrain/net/base.hpp>

namespace refrain::protocol::v777 {

/// One packet declared by a protocol schema. `id` is
/// unique within a (state, flow) pair.
struct packet_descriptor {
    ::refrain::net::connection_state state;
    ::refrain::net::packet_flow flow;
    std::uint16_t id;
    std::string_view name;
};

/// Locate a packet by wire id inside one stage table.
template <std::size_t N>
[[nodiscard]] constexpr auto find_packet(const std::array<packet_descriptor, N> &table,
                                      ::refrain::net::packet_flow flow,
                                      std::uint16_t id) noexcept
    -> const packet_descriptor * {
  for (const auto &entry : table) {
    if (entry.flow == flow && entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

/// Locate a packet by declared name inside one stage table.
template <std::size_t N>
[[nodiscard]] constexpr auto find_packet(const std::array<packet_descriptor, N> &table,
                                      ::refrain::net::packet_flow flow,
                                      std::string_view name) noexcept
    -> const packet_descriptor * {
  for (const auto &entry : table) {
    if (entry.flow == flow && entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

} // namespace refrain::protocol::v777

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PACKET_HPP
