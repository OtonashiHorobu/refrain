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
