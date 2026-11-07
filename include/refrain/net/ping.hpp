// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/status.hpp>
#include <refrain/net/transport.hpp>

#include <chrono>
#include <cstdint>

namespace refrain::net {

struct ping_result {
    status_info status{};
    std::chrono::milliseconds latency{0};
};

/// Connect, perform the handshake/status/ping exchange and close. The timeout
/// covers the whole exchange; a connection failure or a malformed response is
/// returned as an error.
[[nodiscard]] auto
ping(const endpoint &target,
     std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    -> result<ping_result>;

} // namespace refrain::net
