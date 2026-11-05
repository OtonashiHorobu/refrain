// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/replay.hpp>

#include <fmt/format.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::sim {
namespace {

[[nodiscard]] auto trim(std::string_view text) noexcept -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

auto parse_replay_script(std::string_view text)
    -> result<std::vector<replay_step>> {
    std::vector<replay_step> steps{};
    std::int64_t previous_tick{-1};
    std::size_t line_number{0};
    std::size_t start{0};
    while (start <= text.size()) {
        const auto newline{text.find('\n', start)};
        const auto end{newline == std::string_view::npos ? text.size()
                                                         : newline};
        const auto raw{text.substr(start, end - start)};
        start =
            newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++line_number;
        if (raw.size() > max_replay_line_bytes) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           fmt::format("replay line {} is longer than {} bytes",
                                       line_number, max_replay_line_bytes))};
        }
        const auto line{trim(raw)};
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (steps.size() >= max_replay_steps) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("replay script has more than {} commands",
                            max_replay_steps))};
        }
        const auto separator{line.find_first_of(" \t")};
        if (separator == std::string_view::npos) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("replay line {}: expected '<tick> <command>'",
                            line_number))};
        }
        const auto tick_text{line.substr(0, separator)};
        const auto command{trim(line.substr(separator + 1))};
        if (command.empty()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("replay line {}: command is empty", line_number))};
        }
        std::int64_t tick{0};
        const auto parsed{std::from_chars(
            tick_text.data(), tick_text.data() + tick_text.size(), tick)};
        if (parsed.ec != std::errc{} ||
            parsed.ptr != tick_text.data() + tick_text.size() || tick < 0) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           fmt::format("replay line {}: invalid tick '{}'",
                                       line_number, tick_text))};
        }
        if (tick < previous_tick) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("replay line {}: tick {} is before the previous "
                            "tick {} (ticks must be non-decreasing)",
                            line_number, tick, previous_tick))};
        }
        previous_tick = tick;
        steps.push_back(
            replay_step{.tick = tick, .command = std::string{command}});
    }
    return steps;
}

} // namespace refrain::sim
