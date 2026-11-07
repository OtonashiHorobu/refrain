// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::net {

struct status_sample {
    std::string name{};
    uuid id{};
};

/// Values needed by the status response and the server list ping. The
/// description is plain text; legacy formatting codes are passed through as
/// text until the component encoder lands.
struct status_info {
    std::string description{"A Refrain Server"};
    std::string version_name{"26.3"};
    std::int32_t protocol{777};
    std::int32_t max_players{20};
    std::int32_t online_players{0};
    std::vector<status_sample> sample{};
    std::string favicon{};
    bool enforces_secure_chat{false};
    bool replies_to_status{true};
};

/// Serialize the `status_response` JSON payload (without the packet id and
/// string prefix).
[[nodiscard]] auto build_status_json(const status_info &info) -> std::string;

/// Parse a status response payload. Unknown component shapes are kept as their
/// compact JSON text in `description`; a malformed document is `parse_error`.
[[nodiscard]] auto parse_status_json(std::string_view json)
    -> result<status_info>;

/// Handshake `intention` packet body (packet id excluded).
struct handshake_intention {
    std::int32_t protocol{777};
    std::string host{};
    std::uint16_t port{0};
    std::int32_t next{0};
};

[[nodiscard]] auto encode_handshake(const handshake_intention &intention)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto decode_handshake(std::span<const std::byte> body)
    -> result<handshake_intention>;

/// Client-to-server status packets.
[[nodiscard]] auto encode_status_request() -> std::vector<std::byte>;
[[nodiscard]] auto encode_ping_request(std::int64_t time)
    -> std::vector<std::byte>;
[[nodiscard]] auto encode_status_response(std::string_view json)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode_pong_response(std::int64_t time)
    -> std::vector<std::byte>;

} // namespace refrain::net
