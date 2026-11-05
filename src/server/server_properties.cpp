// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/server_properties.hpp>

#include <fmt/format.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace refrain::server {
namespace {

constexpr std::size_t max_lines = 4096;
constexpr std::size_t max_key_bytes = 128;
constexpr std::size_t max_value_bytes = 4096;

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

/// Java-properties unescape for the characters that appear in real
/// server.properties files. Unknown escapes are preserved without the
/// backslash (matching `java.util.Properties` for a single unknown character is
/// more permissive than the reference; the difference is diagnostic only).
[[nodiscard]] auto unescape(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index{0}; index < text.size(); ++index) {
        if (text[index] != '\\' || index + 1 >= text.size()) {
            out.push_back(text[index]);
            continue;
        }
        const char next{text[++index]};
        switch (next) {
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'f':
            out.push_back('\f');
            break;
        default:
            out.push_back(next);
            break;
        }
    }
    return out;
}

[[nodiscard]] auto parse_int(std::string_view text, std::int32_t &out) -> bool {
    std::int32_t value{0};
    const auto *begin{text.data()};
    const auto *end{text.data() + text.size()};
    const auto parsed{std::from_chars(begin, end, value)};
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] auto parse_bool(std::string_view text, bool &out) -> bool {
    if (text == "true") {
        out = true;
        return true;
    }
    if (text == "false") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

auto parse_server_properties(std::string_view text)
    -> result<server_properties> {
    server_properties properties{};
    std::size_t line_number{0};
    std::size_t start{0};
    while (start <= text.size()) {
        const auto newline{text.find('\n', start)};
        const auto raw{text.substr(start, newline == std::string_view::npos
                                              ? std::string_view::npos
                                              : newline - start)};
        start =
            newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++line_number;
        if (line_number > max_lines) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           fmt::format("server.properties: more than {} lines",
                                       max_lines))};
        }
        const auto line{trim(raw)};
        if (line.empty() || line.front() == '#' || line.front() == '!') {
            continue;
        }
        const auto separator{line.find_first_of("=:")};
        if (separator == std::string_view::npos) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("server.properties line {}: expected key=value",
                            line_number))};
        }
        const auto key{trim(line.substr(0, separator))};
        const auto raw_value{trim(line.substr(separator + 1))};
        if (key.empty() || key.size() > max_key_bytes ||
            raw_value.size() > max_value_bytes) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("server.properties line {}: key/value is empty or "
                            "too long",
                            line_number))};
        }
        const auto value{unescape(raw_value)};
        if (key == "level-name") {
            properties.level_name = value;
        } else if (key == "motd") {
            properties.motd = value;
        } else if (key == "max-players") {
            if (!parse_int(value, properties.max_players)) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               fmt::format("server.properties line {}: invalid "
                                           "max-players '{}'",
                                           line_number, value))};
            }
        } else if (key == "view-distance") {
            if (!parse_int(value, properties.view_distance)) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               fmt::format("server.properties line {}: invalid "
                                           "view-distance '{}'",
                                           line_number, value))};
            }
        } else if (key == "simulation-distance") {
            if (!parse_int(value, properties.simulation_distance)) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               fmt::format("server.properties line {}: invalid "
                                           "simulation-distance '{}'",
                                           line_number, value))};
            }
        } else if (key == "online-mode") {
            if (!parse_bool(value, properties.online_mode)) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               fmt::format("server.properties line {}: invalid "
                                           "online-mode '{}'",
                                           line_number, value))};
            }
        } else if (key == "allow-nether") {
            if (!parse_bool(value, properties.allow_nether)) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               fmt::format("server.properties line {}: invalid "
                                           "allow-nether '{}'",
                                           line_number, value))};
            }
        } else if (key == "enable-status") {
            if (!parse_bool(value, properties.enable_status)) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               fmt::format("server.properties line {}: invalid "
                                           "enable-status '{}'",
                                           line_number, value))};
            }
        }
    }
    return properties;
}

} // namespace refrain::server
