// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace refrain::net {

struct bot_options {
    endpoint target{};
    std::string username{"Bot"};
    std::chrono::milliseconds timeout{10'000};
    std::uint8_t view_distance{8};
    /// Finish only after this many `level_chunk_with_light` packets; 0 stops
    /// at the play login.
    std::size_t wait_chunks{0};
    /// Keep the connection in play for this long after the join condition is
    /// met (useful for multi-client tests).
    std::chrono::milliseconds linger{0};
};

struct bot_report {
    std::size_t packets{0};
    std::size_t chunks{0};
    std::string name{};
};

/// Offline-mode protocol bot: handshake -> login -> configuration -> play.
/// Online-mode servers report the encryption request as unsupported.
[[nodiscard]] auto run_bot(const bot_options &options) -> result<bot_report>;

} // namespace refrain::net
