// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::sim {

/// One tick-indexed console command in a replay script.
///
/// File grammar (UTF-8, one command per line):
///
///     # comment
///     0 move 1 0 0
///     5 dig 64 73 -48
///     10 place 63 73 -48 minecraft:dirt
///     20 hash
///
/// Ticks must be non-decreasing, so the execution order is explicit and does
/// not depend on file-system iteration or wall-clock scheduling.
struct replay_step {
    std::int64_t tick{0};
    std::string command{};
};

inline constexpr std::size_t max_replay_steps = 100'000;
inline constexpr std::size_t max_replay_line_bytes = 8192;

/// Parse a replay script. Comments (`#`) and blank lines are ignored.
[[nodiscard]] auto parse_replay_script(std::string_view text)
    -> result<std::vector<replay_step>>;

} // namespace refrain::sim
