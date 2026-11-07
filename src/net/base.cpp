// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/base.hpp>

#include <bit>
#include <cstring>
#include <limits>
#include <utility>

#include <fmt/format.h>

namespace refrain::net {
namespace {

[[nodiscard]] auto parse_failure(std::string_view message) -> error {
    return make_error(error_code::parse_error, message);
}

[[nodiscard]] auto read_big_endian_u64(reader &in) -> result<std::uint64_t> {
    auto value{in.read_i64()};
    if (!value) {
        return std::unexpected{value.error()};
    }
    return static_cast<std::uint64_t>(*value);
}

/// UTF-16 code-unit length of well-formed UTF-8. Vanilla field limits count
/// Java `String.length()`, so a supplementary code point counts as two units.
[[nodiscard]] auto utf16_length(std::string_view text) noexcept -> std::size_t {
    std::size_t units{0};
    for (std::size_t index{0}; index < text.size();) {
        const auto byte{static_cast<unsigned char>(text[index])};
        if (byte < 0x80) {
            index += 1;
        } else if ((byte & 0xE0) == 0xC0) {
            index += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            index += 3;
        } else {
            index += 4;
            units += 1; // the surrogate pair contributes two units
        }
        units += 1;
    }
    return units;
}

} // namespace

auto to_string(connection_state state) noexcept -> std::string_view {
    switch (state) {
    case connection_state::handshake:
        return "handshake";
    case connection_state::status:
        return "status";
    case connection_state::login:
        return "login";
    case connection_state::configuration:
        return "configuration";
    case connection_state::play:
        return "play";
    }
    return "unknown";
}

auto to_string(packet_flow flow) noexcept -> std::string_view {
    return flow == packet_flow::serverbound ? "serverbound" : "clientbound";
}

auto is_valid_utf8(std::string_view text) noexcept -> bool {
    std::size_t index{0};
    while (index < text.size()) {
        const auto first{static_cast<unsigned char>(text[index])};
        std::size_t length{0};
        std::uint32_t code_point{0};
        if (first < 0x80) {
            index += 1;
            continue;
        }
        if ((first & 0xE0) == 0xC0) {
            length = 2;
            code_point = first & 0x1FU;
        } else if ((first & 0xF0) == 0xE0) {
            length = 3;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8) == 0xF0) {
            length = 4;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset{1}; offset < length; ++offset) {
            const auto next{static_cast<unsigned char>(text[index + offset])};
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        const bool overlong{(length == 2 && code_point < 0x80) ||
                            (length == 3 && code_point < 0x800) ||
                            (length == 4 && code_point < 0x10000)};
        const bool surrogate{code_point >= 0xD800 && code_point <= 0xDFFF};
        if (overlong || surrogate || code_point > 0x10FFFF) {
            return false;
        }
        index += length;
    }
    return true;
}

auto uuid::to_string() const -> std::string {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string text;
    text.reserve(36);
    for (std::size_t index{0}; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text.push_back('-');
        }
        const auto byte{bytes[index]};
        text.push_back(digits[(byte >> 4U) & 0x0FU]);
        text.push_back(digits[byte & 0x0FU]);
    }
    return text;
}

auto uuid::parse(std::string_view text) -> result<uuid> {
    if (text.size() != 36) {
        return std::unexpected{parse_failure(
            fmt::format("uuid has {} characters, expected 36", text.size()))};
    }
    uuid value{};
    std::size_t position{0};
    for (std::size_t index{0}; index < value.bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            if (text[position] != '-') {
                return std::unexpected{
                    parse_failure("uuid is missing a separator")};
            }
            ++position;
        }
        const auto nibble = [](char character) -> int {
            if (character >= '0' && character <= '9') {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f') {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F') {
                return character - 'A' + 10;
            }
            return -1;
        };
        const int high{nibble(text[position])};
        const int low{nibble(text[position + 1])};
        if (high < 0 || low < 0) {
            return std::unexpected{
                parse_failure("uuid contains a non-hex character")};
        }
        value.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
        position += 2;
    }
    return value;
}

auto reader::need(std::size_t count) -> status {
    if (count > remaining()) {
        return std::unexpected{parse_failure(
            fmt::format("packet truncated: need {} byte(s), {} remain", count,
                        remaining()))};
    }
    return status_ok();
}

auto reader::read_u8() -> result<std::uint8_t> {
    if (auto checked{need(1)}; !checked) {
        return std::unexpected{checked.error()};
    }
    return static_cast<std::uint8_t>(data_[position_++]);
}

auto reader::read_u16() -> result<std::uint16_t> {
    if (auto checked{need(2)}; !checked) {
        return std::unexpected{checked.error()};
    }
    const auto high{static_cast<std::uint16_t>(data_[position_])};
    const auto low{static_cast<std::uint16_t>(data_[position_ + 1])};
    position_ += 2;
    return static_cast<std::uint16_t>((high << 8U) | low);
}

auto reader::read_i32() -> result<std::int32_t> {
    if (auto checked{need(4)}; !checked) {
        return std::unexpected{checked.error()};
    }
    std::uint32_t value{0};
    for (unsigned index{0}; index < 4; ++index) {
        value =
            (value << 8U) | static_cast<std::uint8_t>(data_[position_ + index]);
    }
    position_ += 4;
    return std::bit_cast<std::int32_t>(value);
}

auto reader::read_i64() -> result<std::int64_t> {
    if (auto checked{need(8)}; !checked) {
        return std::unexpected{checked.error()};
    }
    std::uint64_t value{0};
    for (unsigned index{0}; index < 8; ++index) {
        value =
            (value << 8U) | static_cast<std::uint8_t>(data_[position_ + index]);
    }
    position_ += 8;
    return std::bit_cast<std::int64_t>(value);
}

auto reader::read_f32() -> result<float> {
    auto raw{read_i32()};
    if (!raw) {
        return std::unexpected{raw.error()};
    }
    return std::bit_cast<float>(*raw);
}

auto reader::read_f64() -> result<double> {
    auto raw{read_i64()};
    if (!raw) {
        return std::unexpected{raw.error()};
    }
    return std::bit_cast<double>(*raw);
}

auto reader::read_bool() -> result<bool> {
    auto value{read_u8()};
    if (!value) {
        return std::unexpected{value.error()};
    }
    if (*value > 1) {
        return std::unexpected{parse_failure(
            fmt::format("boolean byte is {}, expected 0 or 1", *value))};
    }
    return *value == 1;
}

auto reader::read_varint() -> result<std::uint32_t> {
    std::uint32_t value{0};
    for (unsigned index{0}; index < 5; ++index) {
        auto byte{read_u8()};
        if (!byte) {
            return std::unexpected{byte.error()};
        }
        const auto payload{static_cast<std::uint32_t>(*byte & 0x7FU)};
        if (index == 4 && payload > 0x0FU) {
            return std::unexpected{
                parse_failure("VarInt exceeds the 32-bit range")};
        }
        value |= payload << (7U * index);
        if ((*byte & 0x80U) == 0) {
            return value;
        }
    }
    return std::unexpected{parse_failure("VarInt is longer than five bytes")};
}

auto reader::read_varlong() -> result<std::uint64_t> {
    std::uint64_t value{0};
    for (unsigned index{0}; index < 10; ++index) {
        auto byte{read_u8()};
        if (!byte) {
            return std::unexpected{byte.error()};
        }
        const auto payload{static_cast<std::uint64_t>(*byte & 0x7FU)};
        if (index == 9 && payload > 0x01U) {
            return std::unexpected{
                parse_failure("VarLong exceeds the 64-bit range")};
        }
        value |= payload << (7U * index);
        if ((*byte & 0x80U) == 0) {
            return value;
        }
    }
    return std::unexpected{parse_failure("VarLong is longer than ten bytes")};
}

auto reader::read_string(std::size_t max_characters) -> result<std::string> {
    if (max_characters > std::numeric_limits<std::size_t>::max() / 4U) {
        return std::unexpected{
            parse_failure("string limit is too large to validate")};
    }
    auto length{read_varint()};
    if (!length) {
        return std::unexpected{length.error()};
    }
    const auto bytes{static_cast<std::size_t>(*length)};
    if (bytes > max_characters * 4U) {
        return std::unexpected{parse_failure(
            fmt::format("string is {} byte(s), limit is {} character(s)", bytes,
                        max_characters))};
    }
    auto payload{read_bytes(bytes)};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    const std::string_view text{reinterpret_cast<const char *>(payload->data()),
                                payload->size()};
    if (!is_valid_utf8(text)) {
        return std::unexpected{parse_failure("string is not valid UTF-8")};
    }
    if (utf16_length(text) > max_characters) {
        return std::unexpected{parse_failure(fmt::format(
            "string is longer than {} character(s)", max_characters))};
    }
    return std::string{text};
}

auto reader::read_identifier() -> result<data::resource_location> {
    auto text{read_string(32'767)};
    if (!text) {
        return std::unexpected{text.error()};
    }
    auto location{data::resource_location::parse(*text)};
    if (!location) {
        return std::unexpected{std::move(location.error())};
    }
    return std::move(*location);
}

auto reader::read_uuid() -> result<uuid> {
    uuid value{};
    for (std::size_t half{0}; half < 2; ++half) {
        auto part{read_big_endian_u64(*this)};
        if (!part) {
            return std::unexpected{part.error()};
        }
        for (unsigned index{0}; index < 8; ++index) {
            value.bytes[half * 8 + index] = static_cast<std::uint8_t>(
                (*part >> (56U - index * 8U)) & 0xFFU);
        }
    }
    return value;
}

auto reader::read_bytes(std::size_t count)
    -> result<std::span<const std::byte>> {
    if (auto checked{need(count)}; !checked) {
        return std::unexpected{checked.error()};
    }
    const auto view{data_.subspan(position_, count)};
    position_ += count;
    return view;
}

auto reader::skip(std::size_t count) -> status {
    if (auto checked{need(count)}; !checked) {
        return checked;
    }
    position_ += count;
    return status_ok();
}

void writer::write_u8(std::uint8_t value) {
    out_->push_back(static_cast<std::byte>(value));
}

void writer::write_u16(std::uint16_t value) {
    write_u8(static_cast<std::uint8_t>(value >> 8U));
    write_u8(static_cast<std::uint8_t>(value & 0xFFU));
}

void writer::write_i32(std::int32_t value) {
    const auto raw{std::bit_cast<std::uint32_t>(value)};
    for (int shift{24}; shift >= 0; shift -= 8) {
        write_u8(static_cast<std::uint8_t>((raw >> shift) & 0xFFU));
    }
}

void writer::write_i64(std::int64_t value) {
    const auto raw{std::bit_cast<std::uint64_t>(value)};
    for (int shift{56}; shift >= 0; shift -= 8) {
        write_u8(static_cast<std::uint8_t>((raw >> shift) & 0xFFU));
    }
}

void writer::write_f32(float value) {
    write_i32(std::bit_cast<std::int32_t>(value));
}

void writer::write_f64(double value) {
    write_i64(std::bit_cast<std::int64_t>(value));
}

void writer::write_bool(bool value) { write_u8(value ? 1 : 0); }

void writer::write_varint(std::uint32_t value) {
    while ((value & ~0x7FU) != 0) {
        write_u8(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    write_u8(static_cast<std::uint8_t>(value));
}

void writer::write_varlong(std::uint64_t value) {
    while ((value & ~0x7FULL) != 0) {
        write_u8(static_cast<std::uint8_t>((value & 0x7FULL) | 0x80U));
        value >>= 7U;
    }
    write_u8(static_cast<std::uint8_t>(value));
}

auto writer::write_string(std::string_view text, std::size_t max_characters)
    -> status {
    if (!is_valid_utf8(text)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "string is not valid UTF-8")};
    }
    if (utf16_length(text) > max_characters) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("string exceeds {} character(s)", max_characters))};
    }
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "string exceeds the VarInt length")};
    }
    write_varint(static_cast<std::uint32_t>(text.size()));
    out_->insert(out_->end(), reinterpret_cast<const std::byte *>(text.data()),
                 reinterpret_cast<const std::byte *>(text.data()) +
                     text.size());
    return status_ok();
}

auto writer::write_identifier(const data::resource_location &value) -> status {
    return write_string(value.to_string(), 32'767);
}

auto writer::write_uuid(const uuid &value) -> status {
    for (std::size_t half{0}; half < 2; ++half) {
        std::uint64_t part{0};
        for (unsigned index{0}; index < 8; ++index) {
            part = (part << 8U) | value.bytes[half * 8 + index];
        }
        write_i64(std::bit_cast<std::int64_t>(part));
    }
    return status_ok();
}

void writer::write_bytes(std::span<const std::byte> bytes) {
    out_->insert(out_->end(), bytes.begin(), bytes.end());
}

} // namespace refrain::net
