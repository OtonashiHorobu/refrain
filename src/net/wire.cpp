// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/wire.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <fmt/format.h>

namespace refrain::net {
namespace {

[[nodiscard]] constexpr auto is_bmp_text(std::string_view text) noexcept
    -> bool {
    for (const char character : text) {
        const auto byte{static_cast<unsigned char>(character)};
        if (byte == 0U || byte >= 0xF0U) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto text_failure(std::string_view detail) -> status {
    return std::unexpected{
        make_error(error_code::parse_error, std::string{detail})};
}

} // namespace

namespace {

constexpr double lp_abs_max{1.7179869183E10};
constexpr double lp_abs_min{3.051944088384301E-5};

[[nodiscard]] auto sanitize_component(double value) noexcept -> double {
    if (std::isnan(value)) {
        return 0.0;
    }
    return std::clamp(value, -lp_abs_max, lp_abs_max);
}

[[nodiscard]] auto pack_component(double value) noexcept -> std::uint64_t {
    const auto rounded{std::round((value * 0.5 + 0.5) * 32766.0)};
    return static_cast<std::uint64_t>(rounded);
}

[[nodiscard]] auto unpack_component(std::uint64_t value) noexcept -> double {
    const auto quantized{
        static_cast<double>(std::min<std::uint64_t>(value & 32767U, 32766U))};
    return quantized * 2.0 / 32766.0 - 1.0;
}

} // namespace

namespace {

constexpr std::uint32_t nbt_max_depth{512};

void append_bytes(std::vector<std::byte> &out,
                  std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] auto copy_next(reader &in, std::size_t count,
                             std::vector<std::byte> &out) -> status {
    if (count > refrain::net::max_frame_length || count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error, "nbt: truncated payload")};
    }
    auto bytes{in.read_bytes(count)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    append_bytes(out, *bytes);
    return status_ok();
}

[[nodiscard]] auto read_nbt_payload(reader &in, std::uint8_t type,
                                    std::vector<std::byte> &out,
                                    std::uint32_t depth) -> status;

[[nodiscard]] auto read_nbt_tag(reader &in, std::vector<std::byte> &out,
                                std::uint32_t depth) -> status {
    if (depth > nbt_max_depth) {
        return std::unexpected{
            make_error(error_code::parse_error, "nbt: nesting is too deep")};
    }
    auto type{in.read_u8()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    out.push_back(static_cast<std::byte>(*type));
    if (*type == 0U) {
        return status_ok();
    }
    auto name_length{in.read_u16()};
    if (!name_length) {
        return std::unexpected{std::move(name_length.error())};
    }
    out.push_back(static_cast<std::byte>((*name_length >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(*name_length & 0xFFU));
    if (auto status{copy_next(in, *name_length, out)}; !status) {
        return status;
    }
    return read_nbt_payload(in, *type, out, depth);
}

[[nodiscard]] auto read_nbt_list(reader &in, std::vector<std::byte> &out,
                                 std::uint32_t depth) -> status {
    auto element{in.read_u8()};
    if (!element) {
        return std::unexpected{std::move(element.error())};
    }
    out.push_back(static_cast<std::byte>(*element));
    auto count{in.read_i32()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count < 0) {
        return std::unexpected{
            make_error(error_code::parse_error, "nbt: negative list length")};
    }
    const auto value_count{static_cast<std::uint32_t>(*count)};
    out.push_back(static_cast<std::byte>((value_count >> 24U) & 0xFFU));
    out.push_back(static_cast<std::byte>((value_count >> 16U) & 0xFFU));
    out.push_back(static_cast<std::byte>((value_count >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(value_count & 0xFFU));
    if (value_count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error, "nbt: truncated list")};
    }
    for (std::uint32_t index{0}; index < value_count; ++index) {
        if (auto status{read_nbt_payload(in, *element, out, depth + 1)};
            !status) {
            return status;
        }
    }
    return status_ok();
}

[[nodiscard]] auto read_nbt_compound(reader &in, std::vector<std::byte> &out,
                                     std::uint32_t depth) -> status {
    while (true) {
        if (depth > nbt_max_depth) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "nbt: nesting is too deep")};
        }
        auto type{in.read_u8()};
        if (!type) {
            return std::unexpected{std::move(type.error())};
        }
        out.push_back(static_cast<std::byte>(*type));
        if (*type == 0U) {
            return status_ok();
        }
        auto name_length{in.read_u16()};
        if (!name_length) {
            return std::unexpected{std::move(name_length.error())};
        }
        out.push_back(static_cast<std::byte>((*name_length >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(*name_length & 0xFFU));
        if (auto status{copy_next(in, *name_length, out)}; !status) {
            return status;
        }
        if (auto status{read_nbt_payload(in, *type, out, depth + 1)}; !status) {
            return status;
        }
    }
}

[[nodiscard]] auto read_nbt_payload(reader &in, std::uint8_t type,
                                    std::vector<std::byte> &out,
                                    std::uint32_t depth) -> status {
    switch (type) {
    case 0:
        return status_ok();
    case 1:
        return copy_next(in, 1, out);
    case 2:
        return copy_next(in, 2, out);
    case 3:
    case 5:
        return copy_next(in, 4, out);
    case 4:
    case 6:
        return copy_next(in, 8, out);
    case 7: {
        auto length{in.read_i32()};
        if (!length) {
            return std::unexpected{std::move(length.error())};
        }
        if (*length < 0) {
            return std::unexpected{make_error(
                error_code::parse_error, "nbt: negative byte array length")};
        }
        const auto count{static_cast<std::uint32_t>(*length)};
        out.push_back(static_cast<std::byte>((count >> 24U) & 0xFFU));
        out.push_back(static_cast<std::byte>((count >> 16U) & 0xFFU));
        out.push_back(static_cast<std::byte>((count >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(count & 0xFFU));
        return copy_next(in, count, out);
    }
    case 8: {
        auto length{in.read_u16()};
        if (!length) {
            return std::unexpected{std::move(length.error())};
        }
        out.push_back(static_cast<std::byte>((*length >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(*length & 0xFFU));
        return copy_next(in, *length, out);
    }
    case 9:
        return read_nbt_list(in, out, depth);
    case 10:
        return read_nbt_compound(in, out, depth);
    case 11: {
        auto length{in.read_i32()};
        if (!length) {
            return std::unexpected{std::move(length.error())};
        }
        if (*length < 0) {
            return std::unexpected{make_error(
                error_code::parse_error, "nbt: negative int array length")};
        }
        const auto count{static_cast<std::uint32_t>(*length)};
        out.push_back(static_cast<std::byte>((count >> 24U) & 0xFFU));
        out.push_back(static_cast<std::byte>((count >> 16U) & 0xFFU));
        out.push_back(static_cast<std::byte>((count >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(count & 0xFFU));
        return copy_next(in, static_cast<std::size_t>(count) * 4U, out);
    }
    case 12: {
        auto length{in.read_i32()};
        if (!length) {
            return std::unexpected{std::move(length.error())};
        }
        if (*length < 0) {
            return std::unexpected{make_error(
                error_code::parse_error, "nbt: negative long array length")};
        }
        const auto count{static_cast<std::uint32_t>(*length)};
        out.push_back(static_cast<std::byte>((count >> 24U) & 0xFFU));
        out.push_back(static_cast<std::byte>((count >> 16U) & 0xFFU));
        out.push_back(static_cast<std::byte>((count >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(count & 0xFFU));
        return copy_next(in, static_cast<std::size_t>(count) * 8U, out);
    }
    default:
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("nbt: unknown tag type {}", type))};
    }
}

} // namespace

auto encode_nbt(writer &out, const nbt_blob &value) -> status {
    if (value.bytes.empty()) {
        out.write_u8(0);
        return status_ok();
    }
    out.write_bytes(value.bytes);
    return status_ok();
}

auto decode_nbt(reader &in, nbt_blob &value) -> status {
    std::vector<std::byte> bytes;
    if (auto status{read_nbt_tag(in, bytes, 0)}; !status) {
        return status;
    }
    if (bytes.size() == 1U && bytes.front() == std::byte{0}) {
        bytes.clear();
    }
    value.bytes = std::move(bytes);
    return status_ok();
}

auto encode_lp_vec3(writer &out, const lp_vec3 &value) -> status {
    const auto x{sanitize_component(value.x)};
    const auto y{sanitize_component(value.y)};
    const auto z{sanitize_component(value.z)};
    const auto chessboard{
        std::max(std::abs(x), std::max(std::abs(y), std::abs(z)))};
    if (chessboard < lp_abs_min) {
        out.write_u8(0);
        return status_ok();
    }
    const auto scale{static_cast<std::uint64_t>(std::ceil(chessboard))};
    const bool partial{(scale & 3U) != scale};
    const auto markers{partial ? ((scale & 3U) | 4U) : scale};
    const auto buffer{markers |
                      (pack_component(x / static_cast<double>(scale)) << 3U) |
                      (pack_component(y / static_cast<double>(scale)) << 18U) |
                      (pack_component(z / static_cast<double>(scale)) << 33U)};
    out.write_u8(static_cast<std::uint8_t>(buffer & 0xFFU));
    out.write_u8(static_cast<std::uint8_t>((buffer >> 8U) & 0xFFU));
    out.write_i32(
        static_cast<std::int32_t>(static_cast<std::uint32_t>(buffer >> 16U)));
    if (partial) {
        out.write_varint(static_cast<std::uint32_t>(scale >> 2U));
    }
    return status_ok();
}

auto decode_lp_vec3(reader &in, lp_vec3 &value) -> status {
    auto lowest{in.read_u8()};
    if (!lowest) {
        return std::unexpected{std::move(lowest.error())};
    }
    if (*lowest == 0U) {
        value = lp_vec3{};
        return status_ok();
    }
    auto middle{in.read_u8()};
    if (!middle) {
        return std::unexpected{std::move(middle.error())};
    }
    auto highest{in.read_i32()};
    if (!highest) {
        return std::unexpected{std::move(highest.error())};
    }
    const auto buffer{
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(*highest))
         << 16U) |
        (static_cast<std::uint64_t>(*middle) << 8U) | *lowest};
    auto scale{static_cast<std::uint64_t>(*lowest & 3U)};
    if ((*lowest & 4U) != 0U) {
        auto extension{in.read_varint()};
        if (!extension) {
            return std::unexpected{std::move(extension.error())};
        }
        scale |= static_cast<std::uint64_t>(*extension) << 2U;
    }
    const auto factor{static_cast<double>(scale)};
    value.x = unpack_component(buffer >> 3U) * factor;
    value.y = unpack_component(buffer >> 18U) * factor;
    value.z = unpack_component(buffer >> 33U) * factor;
    return status_ok();
}

auto encode_message_signature_packed(writer &out,
                                     const message_signature_packed &value)
    -> status {
    if (value.full) {
        out.write_varint(0);
        out.write_bytes(value.signature);
        return status_ok();
    }
    out.write_varint(value.id + 1U);
    return status_ok();
}

auto decode_message_signature_packed(reader &in,
                                     message_signature_packed &value)
    -> status {
    auto header{in.read_varint()};
    if (!header) {
        return std::unexpected{std::move(header.error())};
    }
    if (*header == 0U) {
        auto signature{in.read_bytes(256)};
        if (!signature) {
            return std::unexpected{std::move(signature.error())};
        }
        std::copy(signature->begin(), signature->end(),
                  value.signature.begin());
        value.full = true;
        value.id = 0;
        return status_ok();
    }
    value.full = false;
    value.id = *header - 1U;
    return status_ok();
}

auto encode_sound_selector(writer &out, const sound_selector &value) -> status {
    if (value.flags > 3U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "sound_selector: flags are out of range")};
    }
    out.write_u8(static_cast<std::uint8_t>(value.flags));
    if ((value.flags & 1U) != 0U) {
        out.write_varint(value.source);
    }
    if ((value.flags & 2U) != 0U) {
        return out.write_identifier(value.name);
    }
    return status_ok();
}

auto decode_sound_selector(reader &in, sound_selector &value) -> status {
    auto flags{in.read_u8()};
    if (!flags) {
        return std::unexpected{std::move(flags.error())};
    }
    if (*flags > 3U) {
        return std::unexpected{make_error(
            error_code::parse_error, "sound_selector: flags are out of range")};
    }
    value.flags = *flags;
    if ((*flags & 1U) != 0U) {
        auto source{in.read_varint()};
        if (!source) {
            return std::unexpected{std::move(source.error())};
        }
        value.source = *source;
    }
    if ((*flags & 2U) != 0U) {
        auto name{in.read_identifier()};
        if (!name) {
            return std::unexpected{std::move(name.error())};
        }
        value.name = std::move(*name);
    }
    return status_ok();
}

auto encode_objective_update(writer &out, const objective_update &value)
    -> status {
    if (value.method > 2U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "objective_update: method is out of range")};
    }
    if (auto written{out.write_string(value.name)}; !written) {
        return written;
    }
    out.write_u8(value.method);
    if (value.method == 0U || value.method == 2U) {
        if (auto written{encode_text_component(out, value.display)}; !written) {
            return written;
        }
        out.write_varint(value.render_type);
        out.write_bool(false); // number format is not implemented
    }
    return status_ok();
}

auto decode_objective_update(reader &in, objective_update &value) -> status {
    auto name{in.read_string()};
    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    value.name = std::move(*name);
    auto method{in.read_u8()};
    if (!method) {
        return std::unexpected{std::move(method.error())};
    }
    if (*method > 2U) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "objective_update: method is out of range")};
    }
    value.method = *method;
    if (*method == 0U || *method == 2U) {
        if (auto decoded{decode_text_component(in, value.display)}; !decoded) {
            return decoded;
        }
        auto render_type{in.read_varint()};
        if (!render_type) {
            return std::unexpected{std::move(render_type.error())};
        }
        value.render_type = *render_type;
        auto has_format{in.read_bool()};
        if (!has_format) {
            return std::unexpected{std::move(has_format.error())};
        }
        if (*has_format) {
            return std::unexpected{make_error(
                error_code::unsupported,
                "objective_update: number formats are not implemented")};
        }
    }
    return status_ok();
}

auto encode_team_parameters(writer &out, const team_parameters &value)
    -> status {
    for (const auto *component :
         {&value.display, &value.prefix, &value.suffix}) {
        if (auto written{encode_text_component(out, *component)}; !written) {
            return written;
        }
    }
    out.write_varint(value.visibility);
    out.write_varint(value.collision);
    out.write_bool(value.color.has_value());
    if (value.color.has_value()) {
        out.write_varint(*value.color);
    }
    out.write_u8(value.options);
    return status_ok();
}

auto decode_team_parameters(reader &in, team_parameters &value) -> status {
    for (auto *component : {&value.display, &value.prefix, &value.suffix}) {
        if (auto decoded{decode_text_component(in, *component)}; !decoded) {
            return decoded;
        }
    }
    auto visibility{in.read_varint()};
    auto collision{in.read_varint()};
    if (!visibility || !collision) {
        return std::unexpected{
            make_error(error_code::parse_error, "team_update: truncated")};
    }
    value.visibility = *visibility;
    value.collision = *collision;
    value.color.reset();
    auto has_color{in.read_bool()};
    if (!has_color) {
        return std::unexpected{std::move(has_color.error())};
    }
    if (*has_color) {
        auto color{in.read_varint()};
        if (!color) {
            return std::unexpected{std::move(color.error())};
        }
        value.color = *color;
    }
    auto options{in.read_u8()};
    if (!options) {
        return std::unexpected{std::move(options.error())};
    }
    value.options = *options;
    return status_ok();
}

auto encode_team_update(writer &out, const team_update &value) -> status {
    if (value.method > 4U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "team_update: method is out of range")};
    }
    if (auto written{out.write_string(value.name)}; !written) {
        return written;
    }
    out.write_u8(value.method);
    const bool has_parameters{value.method == 0U || value.method == 2U};
    const bool has_players{value.method == 0U || value.method == 3U ||
                           value.method == 4U};
    if (has_parameters) {
        if (!value.parameters.has_value()) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                "team_update: parameters are required for this method")};
        }
        if (auto written{encode_team_parameters(out, *value.parameters)};
            !written) {
            return written;
        }
    }
    if (has_players) {
        out.write_varint(static_cast<std::uint32_t>(value.players.size()));
        for (const auto &player : value.players) {
            if (auto written{out.write_string(player)}; !written) {
                return written;
            }
        }
    }
    return status_ok();
}

auto decode_team_update(reader &in, team_update &value) -> status {
    auto name{in.read_string()};
    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    value.name = std::move(*name);
    auto method{in.read_u8()};
    if (!method) {
        return std::unexpected{std::move(method.error())};
    }
    if (*method > 4U) {
        return std::unexpected{make_error(
            error_code::parse_error, "team_update: method is out of range")};
    }
    value.method = *method;
    value.parameters.reset();
    value.players.clear();
    if (*method == 0U || *method == 2U) {
        team_parameters parameters{};
        if (auto decoded{decode_team_parameters(in, parameters)}; !decoded) {
            return decoded;
        }
        value.parameters = std::move(parameters);
    }
    if (*method == 0U || *method == 3U || *method == 4U) {
        auto count{in.read_varint()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > in.remaining()) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "team_update: player list exceeds the payload")};
        }
        value.players.reserve(*count);
        for (std::uint32_t index{0}; index < *count; ++index) {
            auto player{in.read_string()};
            if (!player) {
                return std::unexpected{std::move(player.error())};
            }
            value.players.push_back(std::move(*player));
        }
    }
    return status_ok();
}

auto encode_vec_delta(writer &out, const vec_delta &value) -> status {
    if (value.steps.size() > 0x7FFFFFFFU) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "vec_delta: too many steps")};
    }
    const auto step_count{static_cast<std::uint32_t>(value.steps.size())};
    out.write_varint((value.on_ground ? 1U : 0U) | (step_count << 1U));
    if (value.steps.empty()) {
        out.write_u16(static_cast<std::uint16_t>(value.x));
        out.write_u16(static_cast<std::uint16_t>(value.y));
        out.write_u16(static_cast<std::uint16_t>(value.z));
        return status_ok();
    }
    for (const auto &step : value.steps) {
        out.write_varint(step.ticks);
        out.write_u16(static_cast<std::uint16_t>(step.x));
        out.write_u16(static_cast<std::uint16_t>(step.y));
        out.write_u16(static_cast<std::uint16_t>(step.z));
    }
    return status_ok();
}

auto decode_vec_delta(reader &in, vec_delta &value) -> status {
    auto properties{in.read_varint()};
    if (!properties) {
        return std::unexpected{std::move(properties.error())};
    }
    value.on_ground = (*properties & 1U) != 0U;
    const auto step_count{*properties >> 1U};
    value.steps.clear();
    if (step_count == 0U) {
        auto x{in.read_u16()};
        auto y{in.read_u16()};
        auto z{in.read_u16()};
        if (!x || !y || !z) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "vec_delta: truncated delta")};
        }
        value.x = static_cast<std::int16_t>(*x);
        value.y = static_cast<std::int16_t>(*y);
        value.z = static_cast<std::int16_t>(*z);
        return status_ok();
    }
    if (step_count > 4096U ||
        static_cast<std::size_t>(step_count) * 7U > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "vec_delta: stepped list exceeds the payload")};
    }
    value.steps.reserve(step_count);
    for (std::uint32_t index{0}; index < step_count; ++index) {
        move_step step{};
        auto ticks{in.read_varint()};
        if (!ticks) {
            return std::unexpected{std::move(ticks.error())};
        }
        step.ticks = *ticks;
        auto x{in.read_u16()};
        auto y{in.read_u16()};
        auto z{in.read_u16()};
        if (!x || !y || !z) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "vec_delta: truncated step")};
        }
        step.x = static_cast<std::int16_t>(*x);
        step.y = static_cast<std::int16_t>(*y);
        step.z = static_cast<std::int16_t>(*z);
        value.steps.push_back(step);
    }
    return status_ok();
}

auto encode_advancement_seen(writer &out, const advancement_seen &value)
    -> status {
    if (value.action > 1U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "advancement_seen: action is out of range")};
    }
    out.write_varint(value.action);
    if (value.action == 0U) {
        return out.write_identifier(value.tab);
    }
    return status_ok();
}

auto decode_advancement_seen(reader &in, advancement_seen &value) -> status {
    auto action{in.read_varint()};
    if (!action) {
        return std::unexpected{std::move(action.error())};
    }
    if (*action > 1U) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "advancement_seen: action is out of range")};
    }
    value.action = *action;
    if (*action == 0U) {
        auto tab{in.read_identifier()};
        if (!tab) {
            return std::unexpected{std::move(tab.error())};
        }
        value.tab = std::move(*tab);
    }
    return status_ok();
}

auto encode_stat_entry(writer &out, const stat_entry &value) -> status {
    out.write_varint(value.type);
    out.write_varint(value.value);
    return status_ok();
}

auto decode_stat_entry(reader &in, stat_entry &value) -> status {
    auto type{in.read_varint()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    auto stat_value{in.read_varint()};
    if (!stat_value) {
        return std::unexpected{std::move(stat_value.error())};
    }
    value.type = *type;
    value.value = *stat_value;
    return status_ok();
}

auto encode_position_path(writer &out, const position_path &value) -> status {
    if (value.type == 0U) {
        out.write_varint(0);
        out.write_f64(value.end[0]);
        out.write_f64(value.end[1]);
        out.write_f64(value.end[2]);
        return status_ok();
    }
    if (value.type != 1U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "position_path: type is out of range")};
    }
    out.write_varint(1);
    out.write_varint(static_cast<std::uint32_t>(value.steps.size()));
    for (const auto &step : value.steps) {
        out.write_f64(step.x);
        out.write_f64(step.y);
        out.write_f64(step.z);
        out.write_varint(step.tick_offset);
    }
    return status_ok();
}

auto decode_position_path(reader &in, position_path &value) -> status {
    auto type{in.read_varint()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    if (*type > 1U) {
        return std::unexpected{make_error(
            error_code::parse_error, "position_path: type is out of range")};
    }
    value.type = *type;
    value.steps.clear();
    if (*type == 0U) {
        for (auto &axis : value.end) {
            auto coordinate{in.read_f64()};
            if (!coordinate) {
                return std::unexpected{std::move(coordinate.error())};
            }
            axis = *coordinate;
        }
        return status_ok();
    }
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count > 4096U ||
        static_cast<std::size_t>(*count) * 25U > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "position_path: stepped list exceeds the payload")};
    }
    value.steps.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        position_step step{};
        for (double *axis : {&step.x, &step.y, &step.z}) {
            auto coordinate{in.read_f64()};
            if (!coordinate) {
                return std::unexpected{std::move(coordinate.error())};
            }
            *axis = *coordinate;
        }
        auto offset{in.read_varint()};
        if (!offset) {
            return std::unexpected{std::move(offset.error())};
        }
        step.tick_offset = *offset;
        value.steps.push_back(step);
    }
    return status_ok();
}

auto encode_map_patch(writer &out, const map_patch &value) -> status {
    out.write_u8(value.width);
    if (value.width == 0U) {
        if (!value.colors.empty()) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "map_patch: an empty patch carries no colours")};
        }
        return status_ok();
    }
    out.write_u8(value.height);
    out.write_u8(value.start_x);
    out.write_u8(value.start_y);
    out.write_varint(static_cast<std::uint32_t>(value.colors.size()));
    out.write_bytes(value.colors);
    return status_ok();
}

auto decode_map_patch(reader &in, map_patch &value) -> status {
    auto width{in.read_u8()};
    if (!width) {
        return std::unexpected{std::move(width.error())};
    }
    value.width = *width;
    value.height = 0;
    value.start_x = 0;
    value.start_y = 0;
    value.colors.clear();
    if (*width == 0U) {
        return status_ok();
    }
    auto height{in.read_u8()};
    auto start_x{in.read_u8()};
    auto start_y{in.read_u8()};
    if (!height || !start_x || !start_y) {
        return std::unexpected{
            make_error(error_code::parse_error, "map_patch: truncated header")};
    }
    value.height = *height;
    value.start_x = *start_x;
    value.start_y = *start_y;
    auto length{in.read_varint()};
    if (!length) {
        return std::unexpected{std::move(length.error())};
    }
    auto colors{in.read_bytes(*length)};
    if (!colors) {
        return std::unexpected{std::move(colors.error())};
    }
    value.colors.assign(colors->begin(), colors->end());
    return status_ok();
}

auto encode_filter_mask(writer &out, const filter_mask &value) -> status {
    if (value.type > 2U) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "filter_mask: type is out of range")};
    }
    out.write_varint(value.type);
    if (value.type == 2U) {
        out.write_varint(static_cast<std::uint32_t>(value.mask.size()));
        out.write_bytes(value.mask);
    }
    return status_ok();
}

auto decode_filter_mask(reader &in, filter_mask &value) -> status {
    auto type{in.read_varint()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    if (*type > 2U) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "filter_mask: type is out of range")};
    }
    value.type = *type;
    value.mask.clear();
    if (*type == 2U) {
        auto length{in.read_varint()};
        if (!length) {
            return std::unexpected{std::move(length.error())};
        }
        auto bytes{in.read_bytes(*length)};
        if (!bytes) {
            return std::unexpected{std::move(bytes.error())};
        }
        value.mask.assign(bytes->begin(), bytes->end());
    }
    return status_ok();
}

auto encode_item_stack(writer &out, const item_stack &value) -> status {
    if (value.count != 0) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "item_stack: non-empty stacks need the data-component codec")};
    }
    out.write_varint(0);
    return status_ok();
}

auto decode_item_stack(reader &in, item_stack &value) -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count != 0) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "item_stack: non-empty stacks need the data-component codec")};
    }
    value.count = 0;
    return status_ok();
}

auto encode_text_component(writer &out, const text_component &value) -> status {
    if (!is_valid_utf8(value.text)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "text_component: text is not valid UTF-8")};
    }
    if (!is_bmp_text(value.text)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "text_component: text contains NUL or a supplementary character")};
    }
    if (value.text.size() > 0xFFFFU) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "text_component: text exceeds 65535 bytes")};
    }
    out.write_u8(8);  // TAG_String
    out.write_u16(0); // empty root name
    out.write_u16(static_cast<std::uint16_t>(value.text.size()));
    out.write_bytes(std::as_bytes(std::span{value.text}));
    return status_ok();
}

auto decode_text_component(reader &in, text_component &value) -> status {
    auto tag{in.read_u8()};
    if (!tag) {
        return std::unexpected{std::move(tag.error())};
    }
    auto name_length{in.read_u16()};
    if (!name_length) {
        return std::unexpected{std::move(name_length.error())};
    }
    if (auto skipped{in.skip(*name_length)}; !skipped) {
        return skipped;
    }
    if (*tag == 0U) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "text_component: tag_end is not a component")};
    }
    if (*tag != 8U) {
        return std::unexpected{make_error(
            error_code::unsupported,
            fmt::format("text_component: tag type {} is not supported", *tag))};
    }
    auto length{in.read_u16()};
    if (!length) {
        return std::unexpected{std::move(length.error())};
    }
    auto bytes{in.read_bytes(*length)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    std::string text(reinterpret_cast<const char *>(bytes->data()),
                     bytes->size());
    if (!is_valid_utf8(text) || !is_bmp_text(text)) {
        return text_failure("text_component: string is not valid BMP UTF-8");
    }
    value.text = std::move(text);
    return status_ok();
}

} // namespace refrain::net
