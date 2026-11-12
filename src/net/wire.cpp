// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/wire.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

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

/// Decode mapping for data component type ids. The server installs it once
/// while it starts, before any connection is accepted; the atomic snapshot
/// lets a reader on a connection thread keep the mapping it started with.
std::atomic<std::shared_ptr<const item_component_ids>> g_item_component_ids{};

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

namespace {

[[nodiscard]] auto write_byte_array(writer &out,
                                    const std::vector<std::byte> &bytes)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(bytes.size()));
    out.write_bytes(bytes);
    return status_ok();
}

[[nodiscard]] auto read_byte_array(reader &in, std::vector<std::byte> &out,
                                   std::uint32_t limit) -> status {
    auto length{in.read_varint()};
    if (!length) {
        return std::unexpected{std::move(length.error())};
    }
    if (*length > limit) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "wire: byte array exceeds its declared maximum")};
    }
    auto bytes{in.read_bytes(*length)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    out.assign(bytes->begin(), bytes->end());
    return status_ok();
}

[[nodiscard]] auto read_optional_string(reader &in,
                                        std::optional<std::string> &out,
                                        std::size_t limit) -> status {
    auto present{in.read_bool()};
    if (!present) {
        return std::unexpected{std::move(present.error())};
    }
    out.reset();
    if (!*present) {
        return status_ok();
    }
    auto text{in.read_string(limit)};
    if (!text) {
        return std::unexpected{std::move(text.error())};
    }
    out = std::move(*text);
    return status_ok();
}

} // namespace

auto encode_player_info_update(writer &out, const player_info_update &value)
    -> status {
    out.write_u8(value.actions);
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &entry : value.entries) {
        if (auto written{out.write_uuid(entry.id)}; !written) {
            return written;
        }
        if ((value.actions & 1U) != 0U) {
            if (auto written{out.write_string(entry.name, 16)}; !written) {
                return written;
            }
            out.write_varint(
                static_cast<std::uint32_t>(entry.properties.size()));
            for (const auto &property : entry.properties) {
                if (auto written{out.write_string(property.name, 64)};
                    !written) {
                    return written;
                }
                if (auto written{out.write_string(property.value)}; !written) {
                    return written;
                }
                out.write_bool(property.signature.has_value());
                if (property.signature.has_value()) {
                    if (auto written{
                            out.write_string(*property.signature, 1024)};
                        !written) {
                        return written;
                    }
                }
            }
        }
        if ((value.actions & 2U) != 0U) {
            out.write_bool(entry.chat.present);
            if (entry.chat.present) {
                if (auto written{out.write_uuid(entry.chat.session_id)};
                    !written) {
                    return written;
                }
                out.write_i64(entry.chat.expires_at);
                if (auto written{write_byte_array(out, entry.chat.key)};
                    !written) {
                    return written;
                }
                if (auto written{
                        write_byte_array(out, entry.chat.key_signature)};
                    !written) {
                    return written;
                }
            }
        }
        if ((value.actions & 4U) != 0U) {
            out.write_varint(entry.game_mode.value_or(0));
        }
        if ((value.actions & 8U) != 0U) {
            out.write_bool(entry.listed.value_or(false));
        }
        if ((value.actions & 16U) != 0U) {
            out.write_varint(entry.latency.value_or(0));
        }
        if ((value.actions & 32U) != 0U) {
            out.write_bool(entry.has_display_name);
            if (entry.has_display_name) {
                if (auto written{
                        encode_text_component(out, entry.display_name)};
                    !written) {
                    return written;
                }
            }
        }
        if ((value.actions & 64U) != 0U) {
            out.write_varint(entry.list_order.value_or(0));
        }
        if ((value.actions & 128U) != 0U) {
            out.write_bool(entry.show_hat.value_or(false));
        }
    }
    return status_ok();
}

auto decode_player_info_update(reader &in, player_info_update &value)
    -> status {
    auto actions{in.read_u8()};
    if (!actions) {
        return std::unexpected{std::move(actions.error())};
    }
    value.actions = *actions;
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (static_cast<std::size_t>(*count) * 16U > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "player_info_update: entry list exceeds the payload")};
    }
    value.entries.clear();
    value.entries.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        player_info_entry entry{};
        auto id{in.read_uuid()};
        if (!id) {
            return std::unexpected{std::move(id.error())};
        }
        entry.id = *id;
        if ((value.actions & 1U) != 0U) {
            auto name{in.read_string(16)};
            if (!name) {
                return std::unexpected{std::move(name.error())};
            }
            entry.name = std::move(*name);
            auto property_count{in.read_varint()};
            if (!property_count) {
                return std::unexpected{std::move(property_count.error())};
            }
            if (*property_count > in.remaining()) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    "player_info_update: property list exceeds the payload")};
            }
            entry.properties.reserve(*property_count);
            for (std::uint32_t property_index{0};
                 property_index < *property_count; ++property_index) {
                profile_property property{};
                auto property_name{in.read_string(64)};
                auto property_value{in.read_string()};
                if (!property_name || !property_value) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "player_info_update: truncated property")};
                }
                property.name = std::move(*property_name);
                property.value = std::move(*property_value);
                if (auto status{
                        read_optional_string(in, property.signature, 1024)};
                    !status) {
                    return status;
                }
                entry.properties.push_back(std::move(property));
            }
        }
        if ((value.actions & 2U) != 0U) {
            auto present{in.read_bool()};
            if (!present) {
                return std::unexpected{std::move(present.error())};
            }
            entry.chat.present = *present;
            if (*present) {
                auto session{in.read_uuid()};
                auto expires{in.read_i64()};
                if (!session || !expires) {
                    return std::unexpected{make_error(
                        error_code::parse_error,
                        "player_info_update: truncated chat session")};
                }
                entry.chat.session_id = *session;
                entry.chat.expires_at = *expires;
                if (auto status{read_byte_array(in, entry.chat.key, 512)};
                    !status) {
                    return status;
                }
                if (auto status{
                        read_byte_array(in, entry.chat.key_signature, 4096)};
                    !status) {
                    return status;
                }
            }
        }
        if ((value.actions & 4U) != 0U) {
            auto mode{in.read_varint()};
            if (!mode) {
                return std::unexpected{std::move(mode.error())};
            }
            entry.game_mode = *mode;
        }
        if ((value.actions & 8U) != 0U) {
            auto listed{in.read_bool()};
            if (!listed) {
                return std::unexpected{std::move(listed.error())};
            }
            entry.listed = *listed;
        }
        if ((value.actions & 16U) != 0U) {
            auto latency{in.read_varint()};
            if (!latency) {
                return std::unexpected{std::move(latency.error())};
            }
            entry.latency = *latency;
        }
        if ((value.actions & 32U) != 0U) {
            auto has_name{in.read_bool()};
            if (!has_name) {
                return std::unexpected{std::move(has_name.error())};
            }
            entry.has_display_name = *has_name;
            if (*has_name) {
                if (auto status{decode_text_component(in, entry.display_name)};
                    !status) {
                    return status;
                }
            }
        }
        if ((value.actions & 64U) != 0U) {
            auto order{in.read_varint()};
            if (!order) {
                return std::unexpected{std::move(order.error())};
            }
            entry.list_order = *order;
        }
        if ((value.actions & 128U) != 0U) {
            auto hat{in.read_bool()};
            if (!hat) {
                return std::unexpected{std::move(hat.error())};
            }
            entry.show_hat = *hat;
        }
        value.entries.push_back(std::move(entry));
    }
    return status_ok();
}

auto encode_tracked_waypoint(writer &out, const tracked_waypoint &value)
    -> status {
    if (value.operation > 2U || value.type > 3U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "tracked_waypoint: enum value is out of range")};
    }
    out.write_varint(value.operation);
    out.write_bool(value.has_uuid);
    if (value.has_uuid) {
        if (auto written{out.write_uuid(value.id)}; !written) {
            return written;
        }
    } else if (auto written{out.write_string(value.name)}; !written) {
        return written;
    }
    if (auto written{out.write_identifier(value.style)}; !written) {
        return written;
    }
    out.write_bool(value.color.has_value());
    if (value.color.has_value()) {
        out.write_u8(static_cast<std::uint8_t>((*value.color >> 16U) & 0xFFU));
        out.write_u8(static_cast<std::uint8_t>((*value.color >> 8U) & 0xFFU));
        out.write_u8(static_cast<std::uint8_t>(*value.color & 0xFFU));
    }
    out.write_varint(value.type);
    switch (value.type) {
    case 0:
        break;
    case 1:
        out.write_varint(static_cast<std::uint32_t>(value.x));
        out.write_varint(static_cast<std::uint32_t>(value.y));
        out.write_varint(static_cast<std::uint32_t>(value.z));
        break;
    case 2:
        out.write_varint(static_cast<std::uint32_t>(value.x));
        out.write_varint(static_cast<std::uint32_t>(value.z));
        break;
    case 3:
        out.write_f32(value.angle);
        break;
    default:
        break;
    }
    return status_ok();
}

auto decode_tracked_waypoint(reader &in, tracked_waypoint &value) -> status {
    auto operation{in.read_varint()};
    if (!operation) {
        return std::unexpected{std::move(operation.error())};
    }
    if (*operation > 2U) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "tracked_waypoint: operation is out of range")};
    }
    value.operation = *operation;
    auto has_uuid{in.read_bool()};
    if (!has_uuid) {
        return std::unexpected{std::move(has_uuid.error())};
    }
    value.has_uuid = *has_uuid;
    if (*has_uuid) {
        auto id{in.read_uuid()};
        if (!id) {
            return std::unexpected{std::move(id.error())};
        }
        value.id = *id;
    } else {
        auto name{in.read_string()};
        if (!name) {
            return std::unexpected{std::move(name.error())};
        }
        value.name = std::move(*name);
    }
    auto style{in.read_identifier()};
    if (!style) {
        return std::unexpected{std::move(style.error())};
    }
    value.style = std::move(*style);
    value.color.reset();
    auto has_color{in.read_bool()};
    if (!has_color) {
        return std::unexpected{std::move(has_color.error())};
    }
    if (*has_color) {
        auto red{in.read_u8()};
        auto green{in.read_u8()};
        auto blue{in.read_u8()};
        if (!red || !green || !blue) {
            return std::unexpected{make_error(
                error_code::parse_error, "tracked_waypoint: truncated colour")};
        }
        value.color = (static_cast<std::uint32_t>(*red) << 16U) |
                      (static_cast<std::uint32_t>(*green) << 8U) |
                      static_cast<std::uint32_t>(*blue);
    }
    auto type{in.read_varint()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    if (*type > 3U) {
        return std::unexpected{make_error(
            error_code::parse_error, "tracked_waypoint: type is out of range")};
    }
    value.type = *type;
    switch (*type) {
    case 0:
        break;
    case 1: {
        for (auto *axis : {&value.x, &value.y, &value.z}) {
            auto number{in.read_varint()};
            if (!number) {
                return std::unexpected{std::move(number.error())};
            }
            *axis = static_cast<std::int32_t>(*number);
        }
        break;
    }
    case 2: {
        auto x{in.read_varint()};
        auto z{in.read_varint()};
        if (!x || !z) {
            return std::unexpected{make_error(
                error_code::parse_error, "tracked_waypoint: truncated chunk")};
        }
        value.x = static_cast<std::int32_t>(*x);
        value.z = static_cast<std::int32_t>(*z);
        break;
    }
    case 3: {
        auto angle{in.read_f32()};
        if (!angle) {
            return std::unexpected{std::move(angle.error())};
        }
        value.angle = *angle;
        break;
    }
    default:
        break;
    }
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

namespace {

constexpr std::uint32_t recipe_max_depth{64};

[[nodiscard]] auto encode_slot_display_at(writer &out,
                                          const slot_display &value,
                                          std::uint32_t depth) -> status;
[[nodiscard]] auto decode_slot_display_at(reader &in, slot_display &value,
                                          std::uint32_t depth) -> status;

[[nodiscard]] auto
encode_slot_children(writer &out, const std::vector<slot_display> &children,
                     std::uint32_t expected, std::uint32_t depth) -> status {
    if (children.size() != expected) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "slot_display: unexpected child count")};
    }
    for (const auto &child : children) {
        if (auto status{encode_slot_display_at(out, child, depth + 1)};
            !status) {
            return status;
        }
    }
    return status_ok();
}

[[nodiscard]] auto encode_slot_display_at(writer &out,
                                          const slot_display &value,
                                          std::uint32_t depth) -> status {
    if (depth > recipe_max_depth) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "slot_display: nesting is too deep")};
    }
    if (value.type > 10U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "slot_display: type is out of range")};
    }
    out.write_varint(value.type);
    switch (value.type) {
    case 0:
    case 1:
        return status_ok();
    case 2:
        return encode_slot_children(out, value.children, 1, depth);
    case 3:
        if (auto status{encode_slot_children(out, value.children, 1, depth)};
            !status) {
            return status;
        }
        out.write_varint(value.component_type);
        return status_ok();
    case 4:
        out.write_varint(value.item_id);
        return status_ok();
    case 5:
        return encode_item_stack_template(out, value.stack);
    case 6:
        return encode_holder_set(out, value.tag);
    case 7:
    case 9:
        return encode_slot_children(out, value.children, 2, depth);
    case 8:
        if (auto status{encode_slot_children(out, value.children, 2, depth)};
            !status) {
            return status;
        }
        out.write_varint(value.pattern_id);
        return status_ok();
    case 10:
        out.write_varint(static_cast<std::uint32_t>(value.children.size()));
        for (const auto &child : value.children) {
            if (auto status{encode_slot_display_at(out, child, depth + 1)};
                !status) {
                return status;
            }
        }
        return status_ok();
    default:
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "slot_display: type is out of range")};
    }
}

[[nodiscard]] auto decode_slot_children(reader &in,
                                        std::vector<slot_display> &children,
                                        std::uint32_t count,
                                        std::uint32_t depth) -> status {
    children.clear();
    children.reserve(count);
    for (std::uint32_t index{0}; index < count; ++index) {
        slot_display child{};
        if (auto status{decode_slot_display_at(in, child, depth + 1)};
            !status) {
            return status;
        }
        children.push_back(std::move(child));
    }
    return status_ok();
}

[[nodiscard]] auto decode_slot_display_at(reader &in, slot_display &value,
                                          std::uint32_t depth) -> status {
    if (depth > recipe_max_depth) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "slot_display: nesting is too deep")};
    }
    value = slot_display{};
    auto type{in.read_varint()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    if (*type > 10U) {
        return std::unexpected{make_error(
            error_code::parse_error, "slot_display: type is out of range")};
    }
    value.type = *type;
    switch (*type) {
    case 0:
    case 1:
        return status_ok();
    case 2:
        return decode_slot_children(in, value.children, 1, depth);
    case 3: {
        if (auto status{decode_slot_children(in, value.children, 1, depth)};
            !status) {
            return status;
        }
        auto component{in.read_varint()};
        if (!component) {
            return std::unexpected{std::move(component.error())};
        }
        value.component_type = *component;
        return status_ok();
    }
    case 4: {
        auto item{in.read_varint()};
        if (!item) {
            return std::unexpected{std::move(item.error())};
        }
        value.item_id = *item;
        return status_ok();
    }
    case 5:
        return decode_item_stack_template(in, value.stack);
    case 6:
        return decode_holder_set(in, value.tag);
    case 7:
    case 9:
        return decode_slot_children(in, value.children, 2, depth);
    case 8: {
        if (auto status{decode_slot_children(in, value.children, 2, depth)};
            !status) {
            return status;
        }
        auto pattern{in.read_varint()};
        if (!pattern) {
            return std::unexpected{std::move(pattern.error())};
        }
        value.pattern_id = *pattern;
        return status_ok();
    }
    case 10: {
        auto count{in.read_varint()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > in.remaining()) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "slot_display: composite list exceeds the payload")};
        }
        return decode_slot_children(in, value.children, *count, depth);
    }
    default:
        return status_ok();
    }
}

[[nodiscard]] auto encode_recipe_slots(writer &out,
                                       const std::vector<slot_display> &slots,
                                       std::uint32_t expected,
                                       std::uint32_t depth) -> status {
    if (slots.size() != expected) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "recipe_display: unexpected slot count")};
    }
    for (const auto &slot : slots) {
        if (auto status{encode_slot_display_at(out, slot, depth + 1)};
            !status) {
            return status;
        }
    }
    return status_ok();
}

[[nodiscard]] auto decode_recipe_slots(reader &in,
                                       std::vector<slot_display> &slots,
                                       std::uint32_t count, std::uint32_t depth)
    -> status {
    slots.clear();
    slots.reserve(count);
    for (std::uint32_t index{0}; index < count; ++index) {
        slot_display slot{};
        if (auto status{decode_slot_display_at(in, slot, depth + 1)}; !status) {
            return status;
        }
        slots.push_back(std::move(slot));
    }
    return status_ok();
}

} // namespace

auto encode_holder_set(writer &out, const item_holder_set &value) -> status {
    if (value.named) {
        out.write_varint(0);
        return out.write_identifier(value.tag);
    }
    out.write_varint(static_cast<std::uint32_t>(value.ids.size()) + 1U);
    for (const auto id : value.ids) {
        out.write_varint(id);
    }
    return status_ok();
}

auto decode_holder_set(reader &in, item_holder_set &value) -> status {
    auto header{in.read_varint()};
    if (!header) {
        return std::unexpected{std::move(header.error())};
    }
    value = item_holder_set{};
    if (*header == 0U) {
        auto tag{in.read_identifier()};
        if (!tag) {
            return std::unexpected{std::move(tag.error())};
        }
        value.named = true;
        value.tag = std::move(*tag);
        return status_ok();
    }
    const auto count{*header - 1U};
    if (count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "holder_set: id list exceeds the payload")};
    }
    value.ids.reserve(count);
    for (std::uint32_t index{0}; index < count; ++index) {
        auto id{in.read_varint()};
        if (!id) {
            return std::unexpected{std::move(id.error())};
        }
        value.ids.push_back(*id);
    }
    return status_ok();
}

auto encode_slot_display(writer &out, const slot_display &value) -> status {
    return encode_slot_display_at(out, value, 0);
}

auto decode_slot_display(reader &in, slot_display &value) -> status {
    return decode_slot_display_at(in, value, 0);
}

auto encode_recipe_display(writer &out, const recipe_display &value) -> status {
    if (value.type > 4U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "recipe_display: type is out of range")};
    }
    out.write_varint(value.type);
    switch (value.type) {
    case 0: {
        out.write_varint(static_cast<std::uint32_t>(value.slots.size()));
        for (const auto &slot : value.slots) {
            if (auto status{encode_slot_display_at(out, slot, 1)}; !status) {
                return status;
            }
        }
        break;
    }
    case 1: {
        out.write_varint(value.width);
        out.write_varint(value.height);
        out.write_varint(static_cast<std::uint32_t>(value.slots.size()));
        for (const auto &slot : value.slots) {
            if (auto status{encode_slot_display_at(out, slot, 1)}; !status) {
                return status;
            }
        }
        break;
    }
    case 2: {
        if (auto status{encode_recipe_slots(out, value.slots, 2, 1)}; !status) {
            return status;
        }
        break;
    }
    case 3: {
        if (auto status{encode_recipe_slots(out, value.slots, 1, 1)}; !status) {
            return status;
        }
        break;
    }
    case 4: {
        if (auto status{encode_recipe_slots(out, value.slots, 3, 1)}; !status) {
            return status;
        }
        break;
    }
    default:
        break;
    }
    if (auto status{encode_slot_display_at(out, value.result, 1)}; !status) {
        return status;
    }
    if (auto status{encode_slot_display_at(out, value.station, 1)}; !status) {
        return status;
    }
    if (value.type == 2U) {
        out.write_varint(value.duration);
        out.write_f32(value.experience);
    }
    return status_ok();
}

auto decode_recipe_display(reader &in, recipe_display &value) -> status {
    value = recipe_display{};
    auto type{in.read_varint()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    if (*type > 4U) {
        return std::unexpected{make_error(
            error_code::parse_error, "recipe_display: type is out of range")};
    }
    value.type = *type;
    switch (*type) {
    case 0: {
        auto count{in.read_varint()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > in.remaining()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "recipe_display: ingredient list exceeds the payload")};
        }
        if (auto status{decode_recipe_slots(in, value.slots, *count, 1)};
            !status) {
            return status;
        }
        break;
    }
    case 1: {
        auto width{in.read_varint()};
        auto height{in.read_varint()};
        auto count{in.read_varint()};
        if (!width || !height || !count) {
            return std::unexpected{make_error(
                error_code::parse_error, "recipe_display: truncated shaped")};
        }
        if (*count > in.remaining()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "recipe_display: ingredient list exceeds the payload")};
        }
        value.width = *width;
        value.height = *height;
        if (auto status{decode_recipe_slots(in, value.slots, *count, 1)};
            !status) {
            return status;
        }
        break;
    }
    case 2:
        if (auto status{decode_recipe_slots(in, value.slots, 2, 1)}; !status) {
            return status;
        }
        break;
    case 3:
        if (auto status{decode_recipe_slots(in, value.slots, 1, 1)}; !status) {
            return status;
        }
        break;
    case 4:
        if (auto status{decode_recipe_slots(in, value.slots, 3, 1)}; !status) {
            return status;
        }
        break;
    default:
        break;
    }
    if (auto status{decode_slot_display_at(in, value.result, 1)}; !status) {
        return status;
    }
    if (auto status{decode_slot_display_at(in, value.station, 1)}; !status) {
        return status;
    }
    if (*type == 2U) {
        auto duration{in.read_varint()};
        auto experience{in.read_f32()};
        if (!duration || !experience) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "recipe_display: truncated furnace fields")};
        }
        value.duration = *duration;
        value.experience = *experience;
    }
    return status_ok();
}

auto encode_recipe_display_entry(writer &out, const recipe_display_entry &value)
    -> status {
    out.write_varint(value.id);
    if (auto status{encode_recipe_display(out, value.display)}; !status) {
        return status;
    }
    out.write_varint(value.group.has_value() ? *value.group + 1U : 0U);
    out.write_varint(value.category);
    out.write_bool(value.has_requirements);
    if (value.has_requirements) {
        out.write_varint(static_cast<std::uint32_t>(value.requirements.size()));
        for (const auto &requirement : value.requirements) {
            if (auto status{encode_holder_set(out, requirement)}; !status) {
                return status;
            }
        }
    }
    return status_ok();
}

auto decode_recipe_display_entry(reader &in, recipe_display_entry &value)
    -> status {
    auto id{in.read_varint()};
    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    value.id = *id;
    if (auto status{decode_recipe_display(in, value.display)}; !status) {
        return status;
    }
    auto group{in.read_varint()};
    if (!group) {
        return std::unexpected{std::move(group.error())};
    }
    value.group.reset();
    if (*group != 0U) {
        value.group = *group - 1U;
    }
    auto category{in.read_varint()};
    if (!category) {
        return std::unexpected{std::move(category.error())};
    }
    value.category = *category;
    auto has_requirements{in.read_bool()};
    if (!has_requirements) {
        return std::unexpected{std::move(has_requirements.error())};
    }
    value.has_requirements = *has_requirements;
    value.requirements.clear();
    if (*has_requirements) {
        auto count{in.read_varint()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > in.remaining()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "recipe_display_entry: requirement list exceeds the payload")};
        }
        value.requirements.reserve(*count);
        for (std::uint32_t index{0}; index < *count; ++index) {
            item_holder_set requirement{};
            if (auto status{decode_holder_set(in, requirement)}; !status) {
                return status;
            }
            value.requirements.push_back(std::move(requirement));
        }
    }
    return status_ok();
}

auto encode_single_input_set(writer &out, const single_input_set &value)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &entry : value.entries) {
        if (auto status{encode_holder_set(out, entry.input)}; !status) {
            return status;
        }
        if (auto status{encode_slot_display(out, entry.display)}; !status) {
            return status;
        }
    }
    return status_ok();
}

auto decode_single_input_set(reader &in, single_input_set &value) -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "single_input_set: entry list exceeds the payload")};
    }
    value.entries.clear();
    value.entries.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        selectable_recipe_entry entry{};
        if (auto status{decode_holder_set(in, entry.input)}; !status) {
            return status;
        }
        if (auto status{decode_slot_display(in, entry.display)}; !status) {
            return status;
        }
        value.entries.push_back(std::move(entry));
    }
    return status_ok();
}

namespace {

enum class particle_kind : std::uint8_t {
    simple,
    block,
    geyser,
    geyser_base,
    power,
    dust,
    dust_transition,
    spell,
    color,
    sculk_charge,
    item,
    vibration,
    trail,
    shriek,
};

// Payload kind per particle type id, following the protocol's particle
// registry order. Types not listed here are simple particles.
constexpr auto particle_kinds = [] {
    std::array<particle_kind, 128> kinds{};
    for (auto &kind : kinds) {
        kind = particle_kind::simple;
    }
    for (const auto id : {1, 2, 36, 121, 125}) {
        kinds[static_cast<std::size_t>(id)] = particle_kind::block;
    }
    for (const auto id : {7, 10}) {
        kinds[static_cast<std::size_t>(id)] = particle_kind::geyser;
    }
    for (const auto id : {8, 9}) {
        kinds[static_cast<std::size_t>(id)] = particle_kind::geyser_base;
    }
    kinds[15] = particle_kind::power;
    kinds[21] = particle_kind::dust;
    kinds[22] = particle_kind::dust_transition;
    for (const auto id : {23, 56}) {
        kinds[static_cast<std::size_t>(id)] = particle_kind::spell;
    }
    for (const auto id : {28, 46, 52}) {
        kinds[static_cast<std::size_t>(id)] = particle_kind::color;
    }
    kinds[48] = particle_kind::sculk_charge;
    kinds[57] = particle_kind::item;
    kinds[58] = particle_kind::vibration;
    kinds[59] = particle_kind::trail;
    kinds[115] = particle_kind::shriek;
    return kinds;
}();

[[nodiscard]] auto particle_kind_of(std::uint32_t type, bool &known)
    -> particle_kind {
    if (type >= particle_kinds.size()) {
        known = false;
        return particle_kind::simple;
    }
    known = true;
    return particle_kinds[type];
}

} // namespace

namespace {

[[nodiscard]] auto write_position_list(writer &out,
                                       const std::vector<std::int64_t> &values)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(values.size()));
    for (const auto value : values) {
        out.write_i64(value);
    }
    return status_ok();
}

[[nodiscard]] auto read_position_list(reader &in,
                                      std::vector<std::int64_t> &values)
    -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (static_cast<std::size_t>(*count) * 8U > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "debug_value: position list exceeds the payload")};
    }
    values.clear();
    values.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        auto value{in.read_i64()};
        if (!value) {
            return std::unexpected{std::move(value.error())};
        }
        values.push_back(*value);
    }
    return status_ok();
}

[[nodiscard]] auto write_string_list(writer &out,
                                     const std::vector<std::string> &values)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(values.size()));
    for (const auto &value : values) {
        if (auto written{out.write_string(value)}; !written) {
            return written;
        }
    }
    return status_ok();
}

[[nodiscard]] auto read_string_list(reader &in,
                                    std::vector<std::string> &values)
    -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "debug_value: string list exceeds the payload")};
    }
    values.clear();
    values.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        auto value{in.read_string()};
        if (!value) {
            return std::unexpected{std::move(value.error())};
        }
        values.push_back(std::move(*value));
    }
    return status_ok();
}

[[nodiscard]] auto write_path_node(writer &out, const debug_path_node &node)
    -> status {
    out.write_i32(node.x);
    out.write_i32(node.y);
    out.write_i32(node.z);
    out.write_f32(node.walked_distance);
    out.write_f32(node.cost_malus);
    out.write_bool(node.closed);
    out.write_varint(node.type);
    out.write_f32(node.f);
    return status_ok();
}

[[nodiscard]] auto read_path_node(reader &in, debug_path_node &node) -> status {
    auto x{in.read_i32()};
    auto y{in.read_i32()};
    auto z{in.read_i32()};
    auto walked{in.read_f32()};
    auto cost{in.read_f32()};
    auto closed{in.read_bool()};
    auto type{in.read_varint()};
    auto f{in.read_f32()};
    if (!x || !y || !z || !walked || !cost || !closed || !type || !f) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "debug_value: truncated path node")};
    }
    node.x = *x;
    node.y = *y;
    node.z = *z;
    node.walked_distance = *walked;
    node.cost_malus = *cost;
    node.closed = *closed;
    node.type = *type;
    node.f = *f;
    return status_ok();
}

[[nodiscard]] auto write_node_list(writer &out,
                                   const std::vector<debug_path_node> &nodes)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(nodes.size()));
    for (const auto &node : nodes) {
        if (auto status{write_path_node(out, node)}; !status) {
            return status;
        }
    }
    return status_ok();
}

[[nodiscard]] auto read_node_list(reader &in,
                                  std::vector<debug_path_node> &nodes)
    -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (static_cast<std::size_t>(*count) * 24U > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "debug_value: node list exceeds the payload")};
    }
    nodes.clear();
    nodes.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        debug_path_node node{};
        if (auto status{read_path_node(in, node)}; !status) {
            return status;
        }
        nodes.push_back(node);
    }
    return status_ok();
}

[[nodiscard]] auto write_debug_value(writer &out, std::uint32_t subscription,
                                     const debug_value &value) -> status {
    switch (subscription) {
    case 1: // bees
        out.write_bool(value.has_a);
        if (value.has_a) {
            out.write_i64(value.pos_a);
        }
        out.write_bool(value.has_b);
        if (value.has_b) {
            out.write_i64(value.pos_b);
        }
        out.write_varint(value.u_a);
        return write_position_list(out, value.positions_a);
    case 2: // brains
        for (const auto &text : {value.s_a, value.s_b}) {
            if (auto written{out.write_string(text)}; !written) {
                return written;
            }
        }
        out.write_i32(value.i_a);
        out.write_f32(value.f_a);
        out.write_f32(value.f_b);
        if (auto written{out.write_string(value.s_c)}; !written) {
            return written;
        }
        out.write_bool(value.flag_a);
        out.write_i32(value.i_b);
        for (const auto *list : {&value.strings_a, &value.strings_b,
                                 &value.strings_c, &value.strings_d}) {
            if (auto status{write_string_list(out, *list)}; !status) {
                return status;
            }
        }
        for (const auto *list : {&value.positions_a, &value.positions_b}) {
            if (auto status{write_position_list(out, *list)}; !status) {
                return status;
            }
        }
        return status_ok();
    case 3: // breezes
        out.write_bool(value.has_a);
        if (value.has_a) {
            out.write_varint(value.u_a);
        }
        out.write_bool(value.has_b);
        if (value.has_b) {
            out.write_i64(value.pos_b);
        }
        return status_ok();
    case 4: // goals
        out.write_varint(static_cast<std::uint32_t>(value.goals.size()));
        for (const auto &goal : value.goals) {
            out.write_varint(goal.priority);
            out.write_bool(goal.running);
            if (auto written{out.write_string(goal.name, 255)}; !written) {
                return written;
            }
        }
        return status_ok();
    case 5: // entity paths
        out.write_bool(value.flag_a);
        out.write_i32(value.i_a);
        out.write_i64(value.pos_a);
        for (const auto *list :
             {&value.nodes_a, &value.nodes_b, &value.nodes_c, &value.nodes_d}) {
            if (auto status{write_node_list(out, *list)}; !status) {
                return status;
            }
        }
        out.write_f32(value.f_a);
        return status_ok();
    case 6: // entity block intersections
        out.write_varint(value.u_a);
        return status_ok();
    case 7: // bee hives
        out.write_varint(value.u_a);
        out.write_varint(value.u_b);
        out.write_varint(value.u_c);
        out.write_bool(value.flag_a);
        return status_ok();
    case 8: // pois
        out.write_i64(value.pos_a);
        out.write_varint(value.u_a);
        out.write_varint(value.u_b);
        return status_ok();
    case 9: // redstone wire orientations
        out.write_varint(value.u_a);
        return status_ok();
    case 10: // village sections (unit)
        return status_ok();
    case 11: // raids
        return write_position_list(out, value.positions_a);
    case 12: // structures
        out.write_i64(value.pos_a);
        out.write_i64(value.pos_b);
        out.write_varint(static_cast<std::uint32_t>(value.pieces.size()));
        for (const auto &piece : value.pieces) {
            out.write_i64(piece.min_pos);
            out.write_i64(piece.max_pos);
            out.write_bool(piece.start);
        }
        return status_ok();
    case 13: // game event listeners
        out.write_varint(value.u_a);
        return status_ok();
    case 14: // neighbor updates
        out.write_i64(value.pos_a);
        return status_ok();
    case 15: // game events
        out.write_varint(value.u_a);
        out.write_f64(value.d_a);
        out.write_f64(value.d_b);
        out.write_f64(value.d_c);
        return status_ok();
    default:
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "debug_value: subscription has no value codec")};
    }
}

[[nodiscard]] auto read_debug_value(reader &in, std::uint32_t subscription,
                                    debug_value &value) -> status {
    value = debug_value{};
    switch (subscription) {
    case 1: { // bees
        auto has_hive{in.read_bool()};
        if (!has_hive) {
            return std::unexpected{std::move(has_hive.error())};
        }
        value.has_a = *has_hive;
        if (*has_hive) {
            auto pos{in.read_i64()};
            if (!pos) {
                return std::unexpected{std::move(pos.error())};
            }
            value.pos_a = *pos;
        }
        auto has_flower{in.read_bool()};
        if (!has_flower) {
            return std::unexpected{std::move(has_flower.error())};
        }
        value.has_b = *has_flower;
        if (*has_flower) {
            auto pos{in.read_i64()};
            if (!pos) {
                return std::unexpected{std::move(pos.error())};
            }
            value.pos_b = *pos;
        }
        auto travel{in.read_varint()};
        if (!travel) {
            return std::unexpected{std::move(travel.error())};
        }
        value.u_a = *travel;
        return read_position_list(in, value.positions_a);
    }
    case 2: { // brains
        auto name{in.read_string()};
        auto profession{in.read_string()};
        if (!name || !profession) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "debug_value: truncated brain")};
        }
        value.s_a = std::move(*name);
        value.s_b = std::move(*profession);
        auto xp{in.read_i32()};
        auto health{in.read_f32()};
        auto max_health{in.read_f32()};
        if (!xp || !health || !max_health) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "debug_value: truncated brain")};
        }
        value.i_a = *xp;
        value.f_a = *health;
        value.f_b = *max_health;
        auto inventory{in.read_string()};
        auto wants_golem{in.read_bool()};
        auto anger{in.read_i32()};
        if (!inventory || !wants_golem || !anger) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "debug_value: truncated brain")};
        }
        value.s_c = std::move(*inventory);
        value.flag_a = *wants_golem;
        value.i_b = *anger;
        for (auto *list : {&value.strings_a, &value.strings_b, &value.strings_c,
                           &value.strings_d}) {
            if (auto status{read_string_list(in, *list)}; !status) {
                return status;
            }
        }
        for (auto *list : {&value.positions_a, &value.positions_b}) {
            if (auto status{read_position_list(in, *list)}; !status) {
                return status;
            }
        }
        return status_ok();
    }
    case 3: { // breezes
        auto has_target{in.read_bool()};
        if (!has_target) {
            return std::unexpected{std::move(has_target.error())};
        }
        value.has_a = *has_target;
        if (*has_target) {
            auto target{in.read_varint()};
            if (!target) {
                return std::unexpected{std::move(target.error())};
            }
            value.u_a = *target;
        }
        auto has_jump{in.read_bool()};
        if (!has_jump) {
            return std::unexpected{std::move(has_jump.error())};
        }
        value.has_b = *has_jump;
        if (*has_jump) {
            auto pos{in.read_i64()};
            if (!pos) {
                return std::unexpected{std::move(pos.error())};
            }
            value.pos_b = *pos;
        }
        return status_ok();
    }
    case 4: { // goals
        auto count{in.read_varint()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > in.remaining()) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "debug_value: goal list exceeds the payload")};
        }
        value.goals.clear();
        value.goals.reserve(*count);
        for (std::uint32_t index{0}; index < *count; ++index) {
            debug_goal goal{};
            auto priority{in.read_varint()};
            auto running{in.read_bool()};
            auto name{in.read_string(255)};
            if (!priority || !running || !name) {
                return std::unexpected{make_error(
                    error_code::parse_error, "debug_value: truncated goal")};
            }
            goal.priority = *priority;
            goal.running = *running;
            goal.name = std::move(*name);
            value.goals.push_back(std::move(goal));
        }
        return status_ok();
    }
    case 5: { // entity paths
        auto reached{in.read_bool()};
        auto next{in.read_i32()};
        auto target{in.read_i64()};
        if (!reached || !next || !target) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "debug_value: truncated path")};
        }
        value.flag_a = *reached;
        value.i_a = *next;
        value.pos_a = *target;
        for (auto *list :
             {&value.nodes_a, &value.nodes_b, &value.nodes_c, &value.nodes_d}) {
            if (auto status{read_node_list(in, *list)}; !status) {
                return status;
            }
        }
        auto distance{in.read_f32()};
        if (!distance) {
            return std::unexpected{std::move(distance.error())};
        }
        value.f_a = *distance;
        return status_ok();
    }
    case 6:
    case 9:
    case 13:
    case 15: {
        auto number{in.read_varint()};
        if (!number) {
            return std::unexpected{std::move(number.error())};
        }
        value.u_a = *number;
        if (subscription == 15U) {
            auto x{in.read_f64()};
            auto y{in.read_f64()};
            auto z{in.read_f64()};
            if (!x || !y || !z) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "debug_value: truncated game event")};
            }
            value.d_a = *x;
            value.d_b = *y;
            value.d_c = *z;
        }
        return status_ok();
    }
    case 7: {
        auto block{in.read_varint()};
        auto occupants{in.read_varint()};
        auto honey{in.read_varint()};
        auto sedated{in.read_bool()};
        if (!block || !occupants || !honey || !sedated) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "debug_value: truncated hive")};
        }
        value.u_a = *block;
        value.u_b = *occupants;
        value.u_c = *honey;
        value.flag_a = *sedated;
        return status_ok();
    }
    case 8: {
        auto pos{in.read_i64()};
        auto type{in.read_varint()};
        auto free{in.read_varint()};
        if (!pos || !type || !free) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "debug_value: truncated poi")};
        }
        value.pos_a = *pos;
        value.u_a = *type;
        value.u_b = *free;
        return status_ok();
    }
    case 10:
        return status_ok();
    case 11:
        return read_position_list(in, value.positions_a);
    case 12: {
        auto min{in.read_i64()};
        auto max{in.read_i64()};
        auto count{in.read_varint()};
        if (!min || !max || !count) {
            return std::unexpected{make_error(
                error_code::parse_error, "debug_value: truncated structure")};
        }
        value.pos_a = *min;
        value.pos_b = *max;
        if (static_cast<std::size_t>(*count) * 17U > in.remaining()) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "debug_value: structure pieces exceed the payload")};
        }
        value.pieces.clear();
        value.pieces.reserve(*count);
        for (std::uint32_t index{0}; index < *count; ++index) {
            debug_structure_piece piece{};
            auto piece_min{in.read_i64()};
            auto piece_max{in.read_i64()};
            auto start{in.read_bool()};
            if (!piece_min || !piece_max || !start) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "debug_value: truncated structure piece")};
            }
            piece.min_pos = *piece_min;
            piece.max_pos = *piece_max;
            piece.start = *start;
            value.pieces.push_back(piece);
        }
        return status_ok();
    }
    case 14: {
        auto pos{in.read_i64()};
        if (!pos) {
            return std::unexpected{std::move(pos.error())};
        }
        value.pos_a = *pos;
        return status_ok();
    }
    default:
        return std::unexpected{make_error(error_code::parse_error,
                                          "debug_value: unknown subscription")};
    }
}

} // namespace

namespace {

[[nodiscard]] auto write_command_properties(writer &out,
                                            const command_node &node)
    -> status {
    switch (node.parser_id) {
    case 0:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
    case 39:
    case 40:
    case 41:
    case 42:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return status_ok();
    case 1: { // brigadier:float
        const auto has_min{(node.number_flags & 1U) != 0U};
        const auto has_max{(node.number_flags & 2U) != 0U};
        out.write_u8(static_cast<std::uint8_t>((has_min ? 1U : 0U) |
                                               (has_max ? 2U : 0U)));
        if (has_min) {
            out.write_f32(static_cast<float>(node.number_min));
        }
        if (has_max) {
            out.write_f32(static_cast<float>(node.number_max));
        }
        return status_ok();
    }
    case 2: { // brigadier:double
        const auto has_min{(node.number_flags & 1U) != 0U};
        const auto has_max{(node.number_flags & 2U) != 0U};
        out.write_u8(static_cast<std::uint8_t>((has_min ? 1U : 0U) |
                                               (has_max ? 2U : 0U)));
        if (has_min) {
            out.write_f64(node.number_min);
        }
        if (has_max) {
            out.write_f64(node.number_max);
        }
        return status_ok();
    }
    case 3: { // brigadier:integer
        const auto has_min{(node.number_flags & 1U) != 0U};
        const auto has_max{(node.number_flags & 2U) != 0U};
        out.write_u8(static_cast<std::uint8_t>((has_min ? 1U : 0U) |
                                               (has_max ? 2U : 0U)));
        if (has_min) {
            out.write_i32(static_cast<std::int32_t>(node.integer_min));
        }
        if (has_max) {
            out.write_i32(static_cast<std::int32_t>(node.integer_max));
        }
        return status_ok();
    }
    case 4: { // brigadier:long
        const auto has_min{(node.number_flags & 1U) != 0U};
        const auto has_max{(node.number_flags & 2U) != 0U};
        out.write_u8(static_cast<std::uint8_t>((has_min ? 1U : 0U) |
                                               (has_max ? 2U : 0U)));
        if (has_min) {
            out.write_i64(node.integer_min);
        }
        if (has_max) {
            out.write_i64(node.integer_max);
        }
        return status_ok();
    }
    case 5: // brigadier:string
        out.write_varint(node.string_type);
        return status_ok();
    case 6:  // entity
    case 31: // score_holder
        out.write_u8(node.entity_flags);
        return status_ok();
    case 43: // time
        out.write_i32(node.time_min);
        return status_ok();
    case 44: // resource_or_tag
    case 45: // resource_or_tag_key
    case 46: // resource
    case 47: // resource_key
    case 48: // resource_selector
        return out.write_identifier(node.registry_key);
    default:
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "command_tree: unknown argument parser id")};
    }
}

[[nodiscard]] auto read_command_properties(reader &in, command_node &node)
    -> status {
    switch (node.parser_id) {
    case 0:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
    case 39:
    case 40:
    case 41:
    case 42:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return status_ok();
    case 1: {
        auto flags{in.read_u8()};
        if (!flags || (*flags & ~3U) != 0U) {
            return std::unexpected{make_error(
                error_code::parse_error, "command_tree: invalid float flags")};
        }
        node.number_flags = *flags;
        if ((*flags & 1U) != 0U) {
            auto value{in.read_f32()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.number_min = *value;
        }
        if ((*flags & 2U) != 0U) {
            auto value{in.read_f32()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.number_max = *value;
        }
        return status_ok();
    }
    case 2: {
        auto flags{in.read_u8()};
        if (!flags || (*flags & ~3U) != 0U) {
            return std::unexpected{make_error(
                error_code::parse_error, "command_tree: invalid double flags")};
        }
        node.number_flags = *flags;
        if ((*flags & 1U) != 0U) {
            auto value{in.read_f64()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.number_min = *value;
        }
        if ((*flags & 2U) != 0U) {
            auto value{in.read_f64()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.number_max = *value;
        }
        return status_ok();
    }
    case 3: {
        auto flags{in.read_u8()};
        if (!flags || (*flags & ~3U) != 0U) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "command_tree: invalid integer flags")};
        }
        node.number_flags = *flags;
        if ((*flags & 1U) != 0U) {
            auto value{in.read_i32()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.integer_min = *value;
        }
        if ((*flags & 2U) != 0U) {
            auto value{in.read_i32()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.integer_max = *value;
        }
        return status_ok();
    }
    case 4: {
        auto flags{in.read_u8()};
        if (!flags || (*flags & ~3U) != 0U) {
            return std::unexpected{make_error(
                error_code::parse_error, "command_tree: invalid long flags")};
        }
        node.number_flags = *flags;
        if ((*flags & 1U) != 0U) {
            auto value{in.read_i64()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.integer_min = *value;
        }
        if ((*flags & 2U) != 0U) {
            auto value{in.read_i64()};
            if (!value) {
                return std::unexpected{std::move(value.error())};
            }
            node.integer_max = *value;
        }
        return status_ok();
    }
    case 5: {
        auto type{in.read_varint()};
        if (!type) {
            return std::unexpected{std::move(type.error())};
        }
        node.string_type = *type;
        return status_ok();
    }
    case 6:
    case 31: {
        auto flags{in.read_u8()};
        if (!flags) {
            return std::unexpected{std::move(flags.error())};
        }
        node.entity_flags = *flags;
        return status_ok();
    }
    case 43: {
        auto value{in.read_i32()};
        if (!value) {
            return std::unexpected{std::move(value.error())};
        }
        node.time_min = *value;
        return status_ok();
    }
    case 44:
    case 45:
    case 46:
    case 47:
    case 48: {
        auto key{in.read_identifier()};
        if (!key) {
            return std::unexpected{std::move(key.error())};
        }
        node.registry_key = std::move(*key);
        return status_ok();
    }
    default:
        return std::unexpected{
            make_error(error_code::parse_error,
                       "command_tree: unknown argument parser id")};
    }
}

} // namespace

namespace {

[[nodiscard]] auto skip_hashed_components(reader &in) -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "container_click_data: bad component map")};
    }
    for (std::uint32_t index{0}; index < *count; ++index) {
        auto key{in.read_varint()};
        auto value{in.read_i32()};
        if (!key || !value) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "container_click_data: truncated component map")};
        }
    }
    auto removed{in.read_varint()};
    if (!removed) {
        return std::unexpected{std::move(removed.error())};
    }
    if (*removed > in.remaining()) {
        return std::unexpected{make_error(
            error_code::parse_error, "container_click_data: bad removed list")};
    }
    for (std::uint32_t index{0}; index < *removed; ++index) {
        if (auto id{in.read_varint()}; !id) {
            return std::unexpected{std::move(id.error())};
        }
    }
    return status_ok();
}

[[nodiscard]] auto skip_hashed_item(reader &in) -> status {
    auto item{in.read_varint()};
    auto count{in.read_varint()};
    if (!item || !count) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "container_click_data: truncated hashed item")};
    }
    return skip_hashed_components(in);
}

} // namespace

auto encode_container_click_data(writer &out, const container_click_data &value)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
    out.write_varint(static_cast<std::uint32_t>(value.state_id));
    out.write_u16(static_cast<std::uint16_t>(value.slot_num));
    out.write_u8(static_cast<std::uint8_t>(value.button_num));
    out.write_varint(value.container_input);
    out.write_varint(0);   // changed slots
    out.write_bool(false); // carried item
    return status_ok();
}

auto decode_container_click_data(reader &in, container_click_data &value)
    -> status {
    auto container_id{in.read_varint()};
    auto state_id{in.read_varint()};
    auto slot{in.read_u16()};
    auto button{in.read_u8()};
    auto input{in.read_varint()};
    if (!container_id || !state_id || !slot || !button || !input) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "container_click_data: truncated")};
    }
    value.container_id = static_cast<std::int32_t>(*container_id);
    value.state_id = static_cast<std::int32_t>(*state_id);
    value.slot_num = static_cast<std::int16_t>(*slot);
    value.button_num = static_cast<std::int8_t>(*button);
    value.container_input = *input;

    auto changed{in.read_varint()};
    if (!changed) {
        return std::unexpected{std::move(changed.error())};
    }
    if (*changed > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "container_click_data: bad changed-slot count")};
    }
    for (std::uint32_t index{0}; index < *changed; ++index) {
        auto key{in.read_u16()};
        auto present{in.read_bool()};
        if (!key || !present) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "container_click_data: truncated changed slot")};
        }
        if (*present) {
            if (auto status{skip_hashed_item(in)}; !status) {
                return status;
            }
        }
    }
    auto carried{in.read_bool()};
    if (!carried) {
        return std::unexpected{std::move(carried.error())};
    }
    if (*carried) {
        if (auto status{skip_hashed_item(in)}; !status) {
            return status;
        }
    }
    return status_ok();
}

auto encode_command_tree(writer &out, const command_tree &value) -> status {
    out.write_varint(static_cast<std::uint32_t>(value.nodes.size()));
    for (const auto &node : value.nodes) {
        if (node.flags & ~0x3FU) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "command_tree: flags are out of range")};
        }
        out.write_u8(node.flags);
        out.write_varint(static_cast<std::uint32_t>(node.children.size()));
        for (const auto child : node.children) {
            out.write_varint(child);
        }
        if ((node.flags & 8U) != 0U) {
            out.write_varint(node.redirect);
        }
        const auto type{static_cast<std::uint8_t>(node.flags & 3U)};
        if (type == 1U) {
            if (auto written{out.write_string(node.name)}; !written) {
                return written;
            }
        } else if (type == 2U) {
            if (auto written{out.write_string(node.name)}; !written) {
                return written;
            }
            out.write_varint(node.parser_id);
            if (auto status{write_command_properties(out, node)}; !status) {
                return status;
            }
            if ((node.flags & 16U) != 0U) {
                if (!node.has_suggestions) {
                    return std::unexpected{make_error(
                        error_code::invalid_argument,
                        "command_tree: suggestion flag without a value")};
                }
                if (auto written{out.write_identifier(node.suggestions)};
                    !written) {
                    return written;
                }
            }
        }
    }
    out.write_varint(value.root_index);
    return status_ok();
}

auto decode_command_tree(reader &in, command_tree &value) -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "command_tree: node list exceeds the payload")};
    }
    value.nodes.clear();
    value.nodes.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        command_node node{};
        auto flags{in.read_u8()};
        if (!flags || (*flags & ~0x3FU) != 0U) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "command_tree: flags are out of range")};
        }
        node.flags = *flags;
        auto child_count{in.read_varint()};
        if (!child_count) {
            return std::unexpected{std::move(child_count.error())};
        }
        if (*child_count > in.remaining()) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "command_tree: child list exceeds the payload")};
        }
        node.children.reserve(*child_count);
        for (std::uint32_t child_index{0}; child_index < *child_count;
             ++child_index) {
            auto child{in.read_varint()};
            if (!child) {
                return std::unexpected{std::move(child.error())};
            }
            node.children.push_back(*child);
        }
        if ((*flags & 8U) != 0U) {
            auto redirect{in.read_varint()};
            if (!redirect) {
                return std::unexpected{std::move(redirect.error())};
            }
            node.redirect = *redirect;
        }
        const auto type{static_cast<std::uint8_t>(*flags & 3U)};
        if (type == 1U) {
            auto name{in.read_string()};
            if (!name) {
                return std::unexpected{std::move(name.error())};
            }
            node.name = std::move(*name);
        } else if (type == 2U) {
            auto name{in.read_string()};
            auto parser{in.read_varint()};
            if (!name || !parser) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "command_tree: truncated argument node")};
            }
            node.name = std::move(*name);
            node.parser_id = *parser;
            if (auto status{read_command_properties(in, node)}; !status) {
                return status;
            }
            if ((*flags & 16U) != 0U) {
                auto suggestions{in.read_identifier()};
                if (!suggestions) {
                    return std::unexpected{std::move(suggestions.error())};
                }
                node.has_suggestions = true;
                node.suggestions = std::move(*suggestions);
            }
        } else if (type == 3U) {
            return std::unexpected{make_error(
                error_code::parse_error, "command_tree: unknown node type")};
        }
        value.nodes.push_back(std::move(node));
    }
    auto root{in.read_varint()};
    if (!root) {
        return std::unexpected{std::move(root.error())};
    }
    if (*root >= value.nodes.size()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "command_tree: root index is out of range")};
    }
    value.root_index = *root;
    return status_ok();
}

auto encode_debug_subscription_update(writer &out,
                                      const debug_subscription_update &value)
    -> status {
    if (value.subscription > 15U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "debug_subscription_update: unknown subscription")};
    }
    out.write_varint(value.subscription);
    out.write_bool(value.has_value);
    if (!value.has_value) {
        return status_ok();
    }
    return write_debug_value(out, value.subscription, value.value);
}

auto decode_debug_subscription_update(reader &in,
                                      debug_subscription_update &value)
    -> status {
    auto subscription{in.read_varint()};
    if (!subscription) {
        return std::unexpected{std::move(subscription.error())};
    }
    if (*subscription > 15U) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "debug_subscription_update: unknown subscription")};
    }
    value.subscription = *subscription;
    auto has_value{in.read_bool()};
    if (!has_value) {
        return std::unexpected{std::move(has_value.error())};
    }
    value.has_value = *has_value;
    if (!*has_value) {
        value.value = debug_value{};
        return status_ok();
    }
    return read_debug_value(in, value.subscription, value.value);
}

auto encode_debug_subscription_event(writer &out,
                                     const debug_subscription_event &value)
    -> status {
    if (value.subscription == 0U || value.subscription > 15U) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "debug_subscription_event: unknown subscription")};
    }
    out.write_varint(value.subscription);
    return write_debug_value(out, value.subscription, value.value);
}

auto decode_debug_subscription_event(reader &in,
                                     debug_subscription_event &value)
    -> status {
    auto subscription{in.read_varint()};
    if (!subscription) {
        return std::unexpected{std::move(subscription.error())};
    }
    if (*subscription == 0U || *subscription > 15U) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "debug_subscription_event: unknown subscription")};
    }
    value.subscription = *subscription;
    return read_debug_value(in, value.subscription, value.value);
}

auto encode_particle_options(writer &out, const particle_options &value)
    -> status {
    bool known{false};
    const auto kind{particle_kind_of(value.type, known)};
    if (!known) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "particle_options: unknown particle type")};
    }
    out.write_varint(value.type);
    switch (kind) {
    case particle_kind::simple:
        break;
    case particle_kind::block:
        out.write_varint(value.block_state);
        break;
    case particle_kind::geyser:
        out.write_i32(value.water_blocks);
        break;
    case particle_kind::geyser_base:
        out.write_i32(value.water_blocks);
        out.write_f32(value.burst_impulse);
        break;
    case particle_kind::power:
        out.write_f32(value.power);
        break;
    case particle_kind::dust:
        out.write_i32(value.color);
        out.write_f32(value.scale);
        break;
    case particle_kind::dust_transition:
        out.write_i32(value.from_color);
        out.write_i32(value.to_color);
        out.write_f32(value.scale);
        break;
    case particle_kind::spell:
        out.write_i32(value.color);
        out.write_f32(value.power);
        break;
    case particle_kind::color:
        out.write_i32(value.color);
        break;
    case particle_kind::sculk_charge:
        out.write_f32(value.roll);
        break;
    case particle_kind::item:
        return encode_item_stack_template(out, value.stack);
    case particle_kind::vibration:
        out.write_varint(value.entity_source ? 1U : 0U);
        if (value.entity_source) {
            out.write_varint(static_cast<std::uint32_t>(value.entity_id));
            out.write_f32(value.y_offset);
        } else {
            out.write_i64(value.source_pos);
        }
        out.write_varint(static_cast<std::uint32_t>(value.arrival_ticks));
        break;
    case particle_kind::trail:
        out.write_f64(value.x);
        out.write_f64(value.y);
        out.write_f64(value.z);
        out.write_i32(value.color);
        out.write_varint(static_cast<std::uint32_t>(value.duration));
        break;
    case particle_kind::shriek:
        out.write_varint(static_cast<std::uint32_t>(value.delay));
        break;
    }
    return status_ok();
}

auto decode_particle_options(reader &in, particle_options &value) -> status {
    auto type{in.read_varint()};
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    bool known{false};
    const auto kind{particle_kind_of(*type, known)};
    if (!known) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "particle_options: unknown particle type")};
    }
    value = particle_options{};
    value.type = *type;
    switch (kind) {
    case particle_kind::simple:
        return status_ok();
    case particle_kind::block: {
        auto state{in.read_varint()};
        if (!state) {
            return std::unexpected{std::move(state.error())};
        }
        value.block_state = *state;
        return status_ok();
    }
    case particle_kind::geyser: {
        auto blocks{in.read_i32()};
        if (!blocks) {
            return std::unexpected{std::move(blocks.error())};
        }
        value.water_blocks = *blocks;
        return status_ok();
    }
    case particle_kind::geyser_base: {
        auto blocks{in.read_i32()};
        auto impulse{in.read_f32()};
        if (!blocks || !impulse) {
            return std::unexpected{make_error(
                error_code::parse_error, "particle_options: truncated geyser")};
        }
        value.water_blocks = *blocks;
        value.burst_impulse = *impulse;
        return status_ok();
    }
    case particle_kind::power: {
        auto power{in.read_f32()};
        if (!power) {
            return std::unexpected{std::move(power.error())};
        }
        value.power = *power;
        return status_ok();
    }
    case particle_kind::dust: {
        auto color{in.read_i32()};
        auto scale{in.read_f32()};
        if (!color || !scale) {
            return std::unexpected{make_error(
                error_code::parse_error, "particle_options: truncated dust")};
        }
        value.color = *color;
        value.scale = *scale;
        return status_ok();
    }
    case particle_kind::dust_transition: {
        auto from{in.read_i32()};
        auto to{in.read_i32()};
        auto scale{in.read_f32()};
        if (!from || !to || !scale) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "particle_options: truncated dust transition")};
        }
        value.from_color = *from;
        value.to_color = *to;
        value.scale = *scale;
        return status_ok();
    }
    case particle_kind::spell: {
        auto color{in.read_i32()};
        auto power{in.read_f32()};
        if (!color || !power) {
            return std::unexpected{make_error(
                error_code::parse_error, "particle_options: truncated spell")};
        }
        value.color = *color;
        value.power = *power;
        return status_ok();
    }
    case particle_kind::color: {
        auto color{in.read_i32()};
        if (!color) {
            return std::unexpected{std::move(color.error())};
        }
        value.color = *color;
        return status_ok();
    }
    case particle_kind::sculk_charge: {
        auto roll{in.read_f32()};
        if (!roll) {
            return std::unexpected{std::move(roll.error())};
        }
        value.roll = *roll;
        return status_ok();
    }
    case particle_kind::item:
        return decode_item_stack_template(in, value.stack);
    case particle_kind::vibration: {
        auto source{in.read_varint()};
        if (!source || *source > 1U) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "particle_options: invalid vibration source")};
        }
        value.entity_source = *source == 1U;
        if (value.entity_source) {
            auto id{in.read_varint()};
            auto offset{in.read_f32()};
            if (!id || !offset) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "particle_options: truncated vibration")};
            }
            value.entity_id = static_cast<std::int32_t>(*id);
            value.y_offset = *offset;
        } else {
            auto pos{in.read_i64()};
            if (!pos) {
                return std::unexpected{std::move(pos.error())};
            }
            value.source_pos = *pos;
        }
        auto ticks{in.read_varint()};
        if (!ticks) {
            return std::unexpected{std::move(ticks.error())};
        }
        value.arrival_ticks = static_cast<std::int32_t>(*ticks);
        return status_ok();
    }
    case particle_kind::trail: {
        auto x{in.read_f64()};
        auto y{in.read_f64()};
        auto z{in.read_f64()};
        auto color{in.read_i32()};
        auto duration{in.read_varint()};
        if (!x || !y || !z || !color || !duration) {
            return std::unexpected{make_error(
                error_code::parse_error, "particle_options: truncated trail")};
        }
        value.x = *x;
        value.y = *y;
        value.z = *z;
        value.color = *color;
        value.duration = static_cast<std::int32_t>(*duration);
        return status_ok();
    }
    case particle_kind::shriek: {
        auto delay{in.read_varint()};
        if (!delay) {
            return std::unexpected{std::move(delay.error())};
        }
        value.delay = static_cast<std::int32_t>(*delay);
        return status_ok();
    }
    }
    return status_ok();
}

auto encode_item_stack_template(writer &out, const item_stack_template &value)
    -> status {
    out.write_varint(value.item_id);
    out.write_varint(value.count);
    // DataComponentPatch: added and removed counts, both zero.
    out.write_varint(0);
    out.write_varint(0);
    return status_ok();
}

auto decode_item_stack_template(reader &in, item_stack_template &value)
    -> status {
    auto item{in.read_varint()};
    auto count{in.read_varint()};
    auto positive{in.read_varint()};
    auto negative{in.read_varint()};
    if (!item || !count || !positive || !negative) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "item_stack_template: truncated")};
    }
    if (*positive != 0U || *negative != 0U) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "item_stack_template: non-empty patches are not implemented")};
    }
    value.item_id = *item;
    value.count = *count;
    return status_ok();
}

auto encode_advancement_display(writer &out, const advancement_display &value)
    -> status {
    for (const auto *component : {&value.title, &value.description}) {
        if (auto written{encode_text_component(out, *component)}; !written) {
            return written;
        }
    }
    if (auto written{encode_item_stack_template(out, value.icon)}; !written) {
        return written;
    }
    out.write_varint(value.frame);
    out.write_i32(value.flags);
    if ((value.flags & 1) != 0) {
        if (!value.background.has_value()) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                "advancement_display: background flag without a background")};
        }
        return out.write_identifier(*value.background);
    }
    return status_ok();
}

auto decode_advancement_display(reader &in, advancement_display &value)
    -> status {
    for (auto *component : {&value.title, &value.description}) {
        if (auto decoded{decode_text_component(in, *component)}; !decoded) {
            return decoded;
        }
    }
    if (auto status{decode_item_stack_template(in, value.icon)}; !status) {
        return status;
    }
    auto frame{in.read_varint()};
    auto flags{in.read_i32()};
    if (!frame || !flags) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "advancement_display: truncated")};
    }
    value.frame = *frame;
    value.flags = *flags;
    value.background.reset();
    if ((*flags & 1) != 0) {
        auto background{in.read_identifier()};
        if (!background) {
            return std::unexpected{std::move(background.error())};
        }
        value.background = std::move(*background);
    }
    return status_ok();
}

auto encode_advancement_requirements(writer &out,
                                     const advancement_requirements &value)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(value.groups.size()));
    for (const auto &group : value.groups) {
        out.write_varint(static_cast<std::uint32_t>(group.size()));
        for (const auto &criterion : group) {
            if (auto written{out.write_string(criterion)}; !written) {
                return written;
            }
        }
    }
    return status_ok();
}

auto decode_advancement_requirements(reader &in,
                                     advancement_requirements &value)
    -> status {
    auto group_count{in.read_varint()};
    if (!group_count) {
        return std::unexpected{std::move(group_count.error())};
    }
    if (*group_count > in.remaining()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "advancement_requirements: group list exceeds the payload")};
    }
    value.groups.clear();
    value.groups.reserve(*group_count);
    for (std::uint32_t group_index{0}; group_index < *group_count;
         ++group_index) {
        auto count{in.read_varint()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > in.remaining()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "advancement_requirements: criterion list exceeds the "
                "payload")};
        }
        std::vector<std::string> group;
        group.reserve(*count);
        for (std::uint32_t criterion_index{0}; criterion_index < *count;
             ++criterion_index) {
            auto criterion{in.read_string()};
            if (!criterion) {
                return std::unexpected{std::move(criterion.error())};
            }
            group.push_back(std::move(*criterion));
        }
        value.groups.push_back(std::move(group));
    }
    return status_ok();
}

auto encode_advancement(writer &out, const advancement &value) -> status {
    out.write_bool(value.parent.has_value());
    if (value.parent.has_value()) {
        if (auto written{out.write_identifier(*value.parent)}; !written) {
            return written;
        }
    }
    out.write_bool(value.has_display);
    if (value.has_display) {
        if (auto written{encode_advancement_display(out, value.display)};
            !written) {
            return written;
        }
    }
    if (auto written{encode_advancement_requirements(out, value.requirements)};
        !written) {
        return written;
    }
    out.write_bool(value.sends_telemetry);
    return status_ok();
}

auto decode_advancement(reader &in, advancement &value) -> status {
    value.parent.reset();
    auto has_parent{in.read_bool()};
    if (!has_parent) {
        return std::unexpected{std::move(has_parent.error())};
    }
    if (*has_parent) {
        auto parent{in.read_identifier()};
        if (!parent) {
            return std::unexpected{std::move(parent.error())};
        }
        value.parent = std::move(*parent);
    }
    auto has_display{in.read_bool()};
    if (!has_display) {
        return std::unexpected{std::move(has_display.error())};
    }
    value.has_display = *has_display;
    if (*has_display) {
        if (auto status{decode_advancement_display(in, value.display)};
            !status) {
            return status;
        }
    }
    if (auto status{decode_advancement_requirements(in, value.requirements)};
        !status) {
        return status;
    }
    auto telemetry{in.read_bool()};
    if (!telemetry) {
        return std::unexpected{std::move(telemetry.error())};
    }
    value.sends_telemetry = *telemetry;
    return status_ok();
}

auto encode_advancement_progress(writer &out, const advancement_progress &value)
    -> status {
    out.write_varint(static_cast<std::uint32_t>(value.criteria.size()));
    for (const auto &entry : value.criteria) {
        if (auto written{out.write_string(entry.criterion)}; !written) {
            return written;
        }
        out.write_bool(entry.obtained.has_value());
        if (entry.obtained.has_value()) {
            out.write_i64(*entry.obtained);
        }
    }
    return status_ok();
}

auto decode_advancement_progress(reader &in, advancement_progress &value)
    -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "advancement_progress: entry list exceeds the payload")};
    }
    value.criteria.clear();
    value.criteria.reserve(*count);
    for (std::uint32_t index{0}; index < *count; ++index) {
        criterion_progress entry{};
        auto criterion{in.read_string()};
        auto present{in.read_bool()};
        if (!criterion || !present) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "advancement_progress: truncated entry")};
        }
        entry.criterion = std::move(*criterion);
        if (*present) {
            auto obtained{in.read_i64()};
            if (!obtained) {
                return std::unexpected{std::move(obtained.error())};
            }
            entry.obtained = *obtained;
        }
        value.criteria.push_back(std::move(entry));
    }
    return status_ok();
}

namespace {

[[nodiscard]] auto write_metadata_value(writer &out,
                                        const metadata_value &value) -> status {
    out.write_u8(value.index);
    out.write_varint(value.serializer);
    switch (value.serializer) {
    case 0:
        out.write_u8(static_cast<std::uint8_t>(value.integer));
        break;
    case 1:
        out.write_varint(static_cast<std::uint32_t>(value.integer));
        break;
    case 2:
        out.write_varlong(static_cast<std::uint64_t>(value.integer));
        break;
    case 3:
        out.write_f32(static_cast<float>(value.floating));
        break;
    case 4:
        return out.write_string(value.text);
    case 5:
        return encode_text_component(out, value.component);
    case 6:
        out.write_bool(value.flag);
        if (value.flag) {
            return encode_text_component(out, value.component);
        }
        break;
    case 7:
        return encode_item_stack(out, value.stack);
    case 8:
        out.write_bool(value.flag);
        break;
    case 9:
    case 39:
        out.write_f32(static_cast<float>(value.vector[0]));
        out.write_f32(static_cast<float>(value.vector[1]));
        out.write_f32(static_cast<float>(value.vector[2]));
        break;
    case 40:
        for (const auto component : value.vector) {
            out.write_f32(static_cast<float>(component));
        }
        break;
    case 10:
        out.write_i64(value.integer);
        break;
    case 11:
        out.write_bool(value.flag);
        if (value.flag) {
            out.write_i64(value.integer);
        }
        break;
    case 12:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
    case 42:
    case 43:
        out.write_varint(static_cast<std::uint32_t>(value.integer));
        break;
    case 13:
        out.write_bool(value.flag);
        if (value.flag) {
            out.write_varint(static_cast<std::uint32_t>(value.integer));
        }
        break;
    case 15:
        out.write_varint(static_cast<std::uint32_t>(value.integer));
        break;
    case 18:
        for (const auto id : value.ids) {
            out.write_varint(id);
        }
        break;
    case 19:
        out.write_varint(
            value.flag ? static_cast<std::uint32_t>(value.integer) + 1U : 0U);
        break;
    case 33:
        out.write_bool(value.flag);
        if (value.flag) {
            if (auto written{out.write_identifier(value.dimension)}; !written) {
                return written;
            }
            out.write_i64(value.position);
        }
        break;
    case 16:
    case 17:
    case 41:
        return std::unexpected{
            make_error(error_code::unsupported,
                       "entity_metadata: serializer is not implemented")};
    default:
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "entity_metadata: unknown serializer id")};
    }
    return status_ok();
}

[[nodiscard]] auto read_metadata_value(reader &in, std::uint8_t index,
                                       metadata_value &value) -> status {
    value = metadata_value{};
    value.index = index;
    auto serializer{in.read_varint()};
    if (!serializer) {
        return std::unexpected{std::move(serializer.error())};
    }
    value.serializer = *serializer;
    switch (*serializer) {
    case 0: {
        auto byte{in.read_u8()};
        if (!byte) {
            return std::unexpected{std::move(byte.error())};
        }
        value.integer = static_cast<std::int8_t>(*byte);
        return status_ok();
    }
    case 1:
    case 12:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
    case 42:
    case 43: {
        auto number{in.read_varint()};
        if (!number) {
            return std::unexpected{std::move(number.error())};
        }
        value.integer = *number;
        return status_ok();
    }
    case 2: {
        auto number{in.read_varlong()};
        if (!number) {
            return std::unexpected{std::move(number.error())};
        }
        value.integer = static_cast<std::int64_t>(*number);
        return status_ok();
    }
    case 3: {
        auto number{in.read_f32()};
        if (!number) {
            return std::unexpected{std::move(number.error())};
        }
        value.floating = *number;
        return status_ok();
    }
    case 4: {
        auto text{in.read_string()};
        if (!text) {
            return std::unexpected{std::move(text.error())};
        }
        value.text = std::move(*text);
        return status_ok();
    }
    case 5:
        return decode_text_component(in, value.component);
    case 6: {
        auto present{in.read_bool()};
        if (!present) {
            return std::unexpected{std::move(present.error())};
        }
        value.flag = *present;
        if (*present) {
            return decode_text_component(in, value.component);
        }
        return status_ok();
    }
    case 7:
        return decode_item_stack(in, value.stack);
    case 8: {
        auto flag{in.read_bool()};
        if (!flag) {
            return std::unexpected{std::move(flag.error())};
        }
        value.flag = *flag;
        return status_ok();
    }
    case 9:
    case 39:
    case 40: {
        const auto count{*serializer == 40U ? 4U : 3U};
        for (std::uint32_t component_index{0}; component_index < count;
             ++component_index) {
            auto component{in.read_f32()};
            if (!component) {
                return std::unexpected{std::move(component.error())};
            }
            value.vector[component_index] = *component;
        }
        return status_ok();
    }
    case 10: {
        auto number{in.read_i64()};
        if (!number) {
            return std::unexpected{std::move(number.error())};
        }
        value.integer = *number;
        return status_ok();
    }
    case 11: {
        auto present{in.read_bool()};
        if (!present) {
            return std::unexpected{std::move(present.error())};
        }
        value.flag = *present;
        if (*present) {
            auto number{in.read_i64()};
            if (!number) {
                return std::unexpected{std::move(number.error())};
            }
            value.integer = *number;
        }
        return status_ok();
    }
    case 13: {
        auto present{in.read_bool()};
        if (!present) {
            return std::unexpected{std::move(present.error())};
        }
        value.flag = *present;
        if (*present) {
            auto number{in.read_varint()};
            if (!number) {
                return std::unexpected{std::move(number.error())};
            }
            value.integer = *number;
        }
        return status_ok();
    }
    case 15: {
        auto number{in.read_varint()};
        if (!number) {
            return std::unexpected{std::move(number.error())};
        }
        value.integer = *number;
        value.flag = *number != 0U;
        return status_ok();
    }
    case 18: {
        for (auto &id : value.ids) {
            auto number{in.read_varint()};
            if (!number) {
                return std::unexpected{std::move(number.error())};
            }
            id = *number;
        }
        return status_ok();
    }
    case 19: {
        auto number{in.read_varint()};
        if (!number) {
            return std::unexpected{std::move(number.error())};
        }
        value.flag = *number != 0U;
        value.integer =
            value.flag ? static_cast<std::int64_t>(*number - 1U) : 0;
        return status_ok();
    }
    case 33: {
        auto present{in.read_bool()};
        if (!present) {
            return std::unexpected{std::move(present.error())};
        }
        value.flag = *present;
        if (*present) {
            auto dimension{in.read_identifier()};
            auto position{in.read_i64()};
            if (!dimension || !position) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "entity_metadata: truncated global position")};
            }
            value.dimension = std::move(*dimension);
            value.position = *position;
        }
        return status_ok();
    }
    case 16:
    case 17:
    case 41:
        return std::unexpected{
            make_error(error_code::unsupported,
                       "entity_metadata: serializer is not implemented")};
    default:
        return std::unexpected{make_error(
            error_code::parse_error, "entity_metadata: unknown serializer id")};
    }
}

} // namespace

auto encode_entity_metadata(writer &out, const entity_metadata &value)
    -> status {
    for (const auto &entry : value.values) {
        if (auto status{write_metadata_value(out, entry)}; !status) {
            return status;
        }
    }
    out.write_u8(255);
    return status_ok();
}

auto decode_entity_metadata(reader &in, entity_metadata &value) -> status {
    value.values.clear();
    while (true) {
        auto index{in.read_u8()};
        if (!index) {
            return std::unexpected{std::move(index.error())};
        }
        if (*index == 255U) {
            return status_ok();
        }
        metadata_value entry{};
        if (auto status{read_metadata_value(in, *index, entry)}; !status) {
            return status;
        }
        value.values.push_back(std::move(entry));
    }
}

auto encode_component_predicate(writer &out, const component_predicate &)
    -> status {
    out.write_varint(0); // empty predicate
    return status_ok();
}

auto decode_component_predicate(reader &in, component_predicate &) -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count != 0U) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "component_predicate: non-empty predicates are not implemented")};
    }
    return status_ok();
}

namespace {

void write_item_cost(writer &out, const item_cost &cost) {
    out.write_varint(cost.item_id);
    out.write_varint(cost.count);
    out.write_varint(0); // empty predicate
}

[[nodiscard]] auto read_item_cost(reader &in, item_cost &cost) -> status {
    auto item{in.read_varint()};
    auto count{in.read_varint()};
    auto predicate{in.read_varint()};
    if (!item || !count || !predicate) {
        return std::unexpected{
            make_error(error_code::parse_error, "item_cost: truncated")};
    }
    if (*predicate != 0U) {
        return std::unexpected{
            make_error(error_code::unsupported,
                       "item_cost: non-empty predicates are not implemented")};
    }
    cost.item_id = *item;
    cost.count = *count;
    return status_ok();
}

} // namespace

auto encode_merchant_offer(writer &out, const merchant_offer &value) -> status {
    write_item_cost(out, value.cost_a);
    if (auto written{encode_item_stack(out, value.result)}; !written) {
        return written;
    }
    out.write_bool(value.cost_b.has_value());
    if (value.cost_b.has_value()) {
        write_item_cost(out, *value.cost_b);
    }
    out.write_bool(value.out_of_stock);
    out.write_i32(value.uses);
    out.write_i32(value.max_uses);
    out.write_i32(value.xp);
    out.write_i32(value.special_price_diff);
    out.write_f32(value.price_multiplier);
    out.write_i32(value.demand);
    return status_ok();
}

auto decode_merchant_offer(reader &in, merchant_offer &value) -> status {
    if (auto status{read_item_cost(in, value.cost_a)}; !status) {
        return status;
    }
    if (auto status{decode_item_stack(in, value.result)}; !status) {
        return status;
    }
    value.cost_b.reset();
    auto has_cost_b{in.read_bool()};
    if (!has_cost_b) {
        return std::unexpected{std::move(has_cost_b.error())};
    }
    if (*has_cost_b) {
        item_cost cost{};
        if (auto status{read_item_cost(in, cost)}; !status) {
            return status;
        }
        value.cost_b = cost;
    }
    auto out_of_stock{in.read_bool()};
    auto uses{in.read_i32()};
    auto max_uses{in.read_i32()};
    auto xp{in.read_i32()};
    auto special_price_diff{in.read_i32()};
    auto price_multiplier{in.read_f32()};
    auto demand{in.read_i32()};
    if (!out_of_stock || !uses || !max_uses || !xp || !special_price_diff ||
        !price_multiplier || !demand) {
        return std::unexpected{
            make_error(error_code::parse_error, "merchant_offer: truncated")};
    }
    value.out_of_stock = *out_of_stock;
    value.uses = *uses;
    value.max_uses = *max_uses;
    value.xp = *xp;
    value.special_price_diff = *special_price_diff;
    value.price_multiplier = *price_multiplier;
    value.demand = *demand;
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

namespace {

[[nodiscard]] auto encode_item_component(writer &out,
                                         const item_component &component)
    -> status {
    return std::visit(
        [&out](const auto &value) -> status {
            using type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<type, item_component_damage>) {
                out.write_varint(value.amount);
                return status_ok();
            } else if constexpr (std::is_same_v<type,
                                                item_component_custom_name>) {
                return encode_text_component(out, value.name);
            } else {
                out.write_varint(
                    static_cast<std::uint32_t>(value.entries.size()));
                for (const auto &entry : value.entries) {
                    if (entry.level < 1U || entry.level > 255U) {
                        return std::unexpected{make_error(
                            error_code::invalid_argument,
                            "item_stack: enchantment level out of range")};
                    }
                    out.write_varint(entry.enchantment_id);
                    out.write_varint(entry.level);
                }
                return status_ok();
            }
        },
        component);
}

[[nodiscard]] auto decode_item_component(reader &in, std::uint32_t type,
                                         const item_component_ids &ids,
                                         item_component &out) -> status {
    if (type == ids.damage) {
        auto amount{in.read_varint()};
        if (!amount) {
            return std::unexpected{std::move(amount.error())};
        }
        out = item_component_damage{.amount = *amount};
        return status_ok();
    }
    if (type == ids.custom_name) {
        item_component_custom_name name{};
        if (auto status{decode_text_component(in, name.name)}; !status) {
            return status;
        }
        out = std::move(name);
        return status_ok();
    }
    if (type == ids.enchantments) {
        auto count{in.read_varint()};
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        // Each entry needs at least two bytes, so a count larger than the
        // remaining payload cannot be honest.
        if (*count > in.remaining()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "item_stack: enchantment count exceeds the payload")};
        }
        item_component_enchantments enchantments{};
        enchantments.entries.reserve(*count);
        for (std::uint32_t index{0}; index < *count; ++index) {
            auto enchantment{in.read_varint()};
            auto level{in.read_varint()};
            if (!enchantment || !level) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "item_stack: truncated enchantment entry")};
            }
            if (*level < 1U || *level > 255U) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "item_stack: enchantment level out of range")};
            }
            enchantments.entries.push_back(item_enchantment{
                .enchantment_id = *enchantment, .level = *level});
        }
        out = std::move(enchantments);
        return status_ok();
    }
    return std::unexpected{make_error(
        error_code::unsupported,
        fmt::format("item_stack: component type {} is not modelled", type))};
}

} // namespace

void set_item_component_ids(item_component_ids ids) {
    auto snapshot{std::make_shared<const item_component_ids>(ids)};
    g_item_component_ids.store(std::move(snapshot));
}

auto item_component_ids_current() noexcept
    -> std::shared_ptr<const item_component_ids> {
    return g_item_component_ids.load();
}

auto encode_item_stack(writer &out, const item_stack &value) -> status {
    out.write_varint(value.count);
    if (value.count == 0U) {
        return status_ok();
    }
    out.write_varint(value.item_id);
    // DataComponentPatch: both counts first, then the added entries, then the
    // removed type ids.
    out.write_varint(static_cast<std::uint32_t>(value.components.size()));
    out.write_varint(
        static_cast<std::uint32_t>(value.removed_components.size()));
    for (const auto &[type, component] : value.components) {
        out.write_varint(type);
        if (auto status{encode_item_component(out, component)}; !status) {
            return status;
        }
    }
    for (const auto type : value.removed_components) {
        out.write_varint(type);
    }
    return status_ok();
}

auto decode_item_stack(reader &in, item_stack &value) -> status {
    auto count{in.read_varint()};
    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    value = item_stack{.count = *count};
    if (*count == 0U) {
        return status_ok();
    }
    auto item{in.read_varint()};
    if (!item) {
        return std::unexpected{std::move(item.error())};
    }
    value.item_id = *item;
    auto positive{in.read_varint()};
    auto negative{in.read_varint()};
    if (!positive || !negative) {
        return std::unexpected{make_error(
            error_code::parse_error, "item_stack: truncated component patch")};
    }
    if (*positive > in.remaining() || *negative > in.remaining()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "item_stack: component counts exceed the payload")};
    }
    if (*positive != 0U) {
        const auto ids{item_component_ids_current()};
        if (ids == nullptr) {
            return std::unexpected{
                make_error(error_code::unsupported,
                           "item_stack: component ids are not installed")};
        }
        for (std::uint32_t index{0}; index < *positive; ++index) {
            auto type{in.read_varint()};
            if (!type) {
                return std::unexpected{std::move(type.error())};
            }
            item_component component{};
            if (auto status{decode_item_component(in, *type, *ids, component)};
                !status) {
                return status;
            }
            value.components.insert_or_assign(*type, std::move(component));
        }
    }
    for (std::uint32_t index{0}; index < *negative; ++index) {
        auto type{in.read_varint()};
        if (!type) {
            return std::unexpected{std::move(type.error())};
        }
        value.removed_components.insert(*type);
    }
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
    out.write_u8(8); // TAG_String, network NBT has no root name
    out.write_u16(static_cast<std::uint16_t>(value.text.size()));
    out.write_bytes(std::as_bytes(std::span{value.text}));
    return status_ok();
}

auto decode_text_component(reader &in, text_component &value) -> status {
    auto tag{in.read_u8()};
    if (!tag) {
        return std::unexpected{std::move(tag.error())};
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
