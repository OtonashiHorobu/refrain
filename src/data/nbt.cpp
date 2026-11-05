// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// NBT codec — the vanilla Java Edition binary format, including Java's
// modified UTF-8 strings. The hand-written codec is deliberate: libnbt++ is
// LGPL and therefore not an option for this project.

#include <refrain/data/nbt.hpp>

#include <refrain/core/io.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace refrain::data::nbt {

namespace {

// The wire format stores IEEE-754 binary32/binary64 big-endian. bit_cast based
// encoding preserves every bit (including NaN payloads) and is only meaningful
// when the host float types are the IEC 559 types the Java client uses.
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<double>::is_iec559);
static_assert(sizeof(float) == 4U);
static_assert(sizeof(double) == 8U);

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, message);
}

[[nodiscard]] constexpr auto tag_id(tag_type kind) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(kind);
}

[[nodiscard]] constexpr auto valid_tag_id(std::uint8_t id) noexcept -> bool {
    return id <= tag_id(tag_type::long_array);
}

// --- modified UTF-8 -------------------------------------------------------
//
// NBT strings use Java's `DataOutput.writeUTF` encoding ("modified UTF-8"):
// U+0000 is written as the two-byte overlong form C0 80 and supplementary code
// points are written as CESU-8 surrogate pairs (two 3-byte sequences) because
// Java's encoder operates on UTF-16 code units. Values are *stored* in the DOM
// as ordinary standard UTF-8, so decode/encode translate between the two.

/// Append one Unicode scalar value as standard UTF-8.
void append_utf8(std::string &out, std::uint32_t code) {
    if (code <= 0x7FU) {
        out += static_cast<char>(code);
    } else if (code <= 0x7FFU) {
        out += static_cast<char>(0xC0U | (code >> 6U));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else if (code <= 0xFFFFU) {
        out += static_cast<char>(0xE0U | (code >> 12U));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (code >> 18U));
        out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
}

/// Validate standard UTF-8 and re-encode it as NBT modified UTF-8.
[[nodiscard]] auto encode_modified_utf8(std::string_view text)
    -> result<std::string> {
    std::string out;
    out.reserve(text.size());
    std::size_t index{0};
    while (index < text.size()) {
        const auto lead{static_cast<std::uint8_t>(text[index])};
        std::uint32_t code{0};
        std::size_t length{0};
        if (lead < 0x80U) {
            code = lead;
            length = 1;
        } else if ((lead & 0xE0U) == 0xC0U) {
            code = lead & 0x1FU;
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            code = lead & 0x0FU;
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            code = lead & 0x07U;
            length = 4;
        } else {
            return std::unexpected{fail(error_code::invalid_argument,
                                        "nbt: string contains invalid UTF-8 "
                                        "(bad lead byte)")};
        }
        if (index + length > text.size()) {
            return std::unexpected{fail(error_code::invalid_argument,
                                        "nbt: string contains a truncated "
                                        "UTF-8 sequence")};
        }
        for (std::size_t extra{1}; extra < length; ++extra) {
            const auto continuation{
                static_cast<std::uint8_t>(text[index + extra])};
            if ((continuation & 0xC0U) != 0x80U) {
                return std::unexpected{fail(error_code::invalid_argument,
                                            "nbt: string contains an invalid "
                                            "UTF-8 continuation byte")};
            }
            code = (code << 6U) | (continuation & 0x3FU);
        }
        static constexpr std::array<std::uint32_t, 5> minimum{0U, 0U, 0x80U,
                                                              0x800U, 0x10000U};
        if (length >= 2 && code < minimum[length]) {
            return std::unexpected{fail(error_code::invalid_argument,
                                        "nbt: string contains an overlong "
                                        "UTF-8 sequence")};
        }
        if (code > 0x10FFFFU) {
            return std::unexpected{fail(error_code::invalid_argument,
                                        "nbt: string contains a code point "
                                        "above U+10FFFF")};
        }
        if (code >= 0xD800U && code <= 0xDFFFU) {
            return std::unexpected{fail(error_code::invalid_argument,
                                        "nbt: string is not valid UTF-8 "
                                        "(surrogate code point)")};
        }
        if (code == 0) {
            out += '\xC0'; // modified UTF-8: NUL is the overlong C0 80 pair
            out += '\x80';
        } else if (code <= 0x7FU) {
            out += static_cast<char>(code);
        } else if (code <= 0x7FFU) {
            out += static_cast<char>(0xC0U | (code >> 6U));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        } else if (code <= 0xFFFFU) {
            out += static_cast<char>(0xE0U | (code >> 12U));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        } else {
            // CESU-8 surrogate pair; each UTF-16 surrogate as a 3-byte form.
            const auto adjusted{code - 0x10000U};
            const std::array<std::uint32_t, 2> surrogates{
                static_cast<std::uint32_t>(0xD800U + (adjusted >> 10U)),
                static_cast<std::uint32_t>(0xDC00U + (adjusted & 0x3FFU))};
            for (const auto surrogate : surrogates) {
                out += static_cast<char>(0xE0U | (surrogate >> 12U));
                out += static_cast<char>(0x80U | ((surrogate >> 6U) & 0x3FU));
                out += static_cast<char>(0x80U | (surrogate & 0x3FU));
            }
        }
        index += length;
    }
    return out;
}

/// Decode NBT modified UTF-8 (accepting plain 4-byte UTF-8 for interoperability
/// with non-Java tooling) into standard UTF-8.
[[nodiscard]] auto decode_modified_utf8(std::span<const std::byte> raw)
    -> result<std::string> {
    std::string out;
    out.reserve(raw.size());
    const auto at = [raw](std::size_t index) {
        return std::to_integer<std::uint8_t>(raw[index]);
    };
    std::size_t index{0};
    while (index < raw.size()) {
        const auto lead{at(index)};
        if (lead < 0x80U) {
            // Java's readUTF accepts a raw single-byte 0x00 as U+0000; be as
            // lenient on read, but always re-encode NUL as C0 80 on write.
            out += static_cast<char>(lead);
            ++index;
            continue;
        }
        if (lead == 0xC0U) {
            if (index + 1 >= raw.size() || at(index + 1) != 0x80U) {
                return std::unexpected{
                    fail(error_code::parse_error,
                         "nbt: malformed modified UTF-8 "
                         "string (overlong 2-byte sequence)")};
            }
            out += '\0';
            index += 2;
            continue;
        }
        std::uint32_t code{0};
        std::size_t length{0};
        if ((lead & 0xE0U) == 0xC0U) {
            code = lead & 0x1FU;
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            code = lead & 0x0FU;
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            code = lead & 0x07U;
            length = 4;
        } else {
            return std::unexpected{
                fail(error_code::parse_error,
                     "nbt: malformed modified UTF-8 string (bad lead byte)")};
        }
        if (index + length > raw.size()) {
            return std::unexpected{
                fail(error_code::parse_error,
                     "nbt: truncated modified UTF-8 string")};
        }
        for (std::size_t extra{1}; extra < length; ++extra) {
            const auto continuation{at(index + extra)};
            if ((continuation & 0xC0U) != 0x80U) {
                return std::unexpected{fail(
                    error_code::parse_error,
                    "nbt: malformed modified UTF-8 string (bad continuation "
                    "byte)")};
            }
            code = (code << 6U) | (continuation & 0x3FU);
        }
        static constexpr std::array<std::uint32_t, 5> minimum{0U, 0U, 0x80U,
                                                              0x800U, 0x10000U};
        if (length >= 2 && code < minimum[length]) {
            return std::unexpected{fail(error_code::parse_error,
                                        "nbt: malformed modified UTF-8 string "
                                        "(overlong sequence)")};
        }
        if (code >= 0xD800U && code <= 0xDFFFU) {
            // CESU-8: a high surrogate must be immediately followed by a
            // low surrogate, also encoded as a 3-byte sequence. Combine the
            // pair into one supplementary code point.
            if (code > 0xDBFFU) {
                return std::unexpected{fail(error_code::parse_error,
                                            "nbt: lone low surrogate in "
                                            "modified UTF-8 string")};
            }
            if (index + 3U + 3U > raw.size()) {
                return std::unexpected{fail(error_code::parse_error,
                                            "nbt: truncated CESU-8 surrogate "
                                            "pair")};
            }
            const auto low_lead{at(index + 3U)};
            if ((low_lead & 0xF0U) != 0xE0U) {
                return std::unexpected{
                    fail(error_code::parse_error,
                         "nbt: lone high surrogate in modified UTF-8 string")};
            }
            const auto low{static_cast<std::uint32_t>(
                ((low_lead & 0x0FU) << 12U) | ((at(index + 4U) & 0x3FU) << 6U) |
                (at(index + 5U) & 0x3FU))};
            if ((at(index + 4U) & 0xC0U) != 0x80U ||
                (at(index + 5U) & 0xC0U) != 0x80U) {
                return std::unexpected{fail(error_code::parse_error,
                                            "nbt: invalid CESU-8 surrogate "
                                            "continuation byte")};
            }
            if (low < 0xDC00U || low > 0xDFFFU) {
                return std::unexpected{fail(error_code::parse_error,
                                            "nbt: lone high surrogate in "
                                            "modified UTF-8 string")};
            }
            code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
            index += 3U; // consume the low surrogate sequence as well
        }
        if (code > 0x10FFFFU) {
            return std::unexpected{fail(error_code::parse_error,
                                        "nbt: modified UTF-8 string decodes "
                                        "above U+10FFFF")};
        }
        append_utf8(out, code);
        index += length;
    }
    return out;
}

// --- big-endian reader ----------------------------------------------------

class byte_reader {
  public:
    byte_reader(std::span<const std::byte> data, const limits &bounds) noexcept
        : data_{data}, bounds_{bounds} {}

    [[nodiscard]] auto remaining() const noexcept -> std::size_t {
        return data_.size() - position_;
    }

    [[nodiscard]] auto u8() -> result<std::uint8_t> {
        const auto chunk{take(1)};
        if (!chunk) {
            return std::unexpected{chunk.error()};
        }
        return std::to_integer<std::uint8_t>((*chunk)[0]);
    }

    [[nodiscard]] auto u16() -> result<std::uint16_t> {
        const auto chunk{take(2)};
        if (!chunk) {
            return std::unexpected{chunk.error()};
        }
        return static_cast<std::uint16_t>(
            (std::to_integer<std::uint16_t>((*chunk)[0]) << 8U) |
            std::to_integer<std::uint16_t>((*chunk)[1]));
    }

    [[nodiscard]] auto u32() -> result<std::uint32_t> {
        const auto chunk{take(4)};
        if (!chunk) {
            return std::unexpected{chunk.error()};
        }
        std::uint32_t value{0};
        for (const auto item : *chunk) {
            value = (value << 8U) | std::to_integer<std::uint32_t>(item);
        }
        return value;
    }

    [[nodiscard]] auto u64() -> result<std::uint64_t> {
        const auto chunk{take(8)};
        if (!chunk) {
            return std::unexpected{chunk.error()};
        }
        std::uint64_t value{0};
        for (const auto item : *chunk) {
            value = (value << 8U) | std::to_integer<std::uint64_t>(item);
        }
        return value;
    }

    [[nodiscard]] auto i32() -> result<std::int32_t> {
        const auto raw{u32()};
        if (!raw) {
            return std::unexpected{raw.error()};
        }
        return std::bit_cast<std::int32_t>(*raw);
    }

    [[nodiscard]] auto i64() -> result<std::int64_t> {
        const auto raw{u64()};
        if (!raw) {
            return std::unexpected{raw.error()};
        }
        return std::bit_cast<std::int64_t>(*raw);
    }

    /// NBT string: u16 length + modified UTF-8 bytes (decoded to std::string).
    [[nodiscard]] auto string() -> result<std::string> {
        const auto raw_length{u16()};
        if (!raw_length) {
            return std::unexpected{raw_length.error()};
        }
        const auto length{static_cast<std::size_t>(*raw_length)};
        if (length > bounds_.max_string_bytes) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: string of {} bytes exceeds the configured "
                            "limit of {} bytes",
                            length, bounds_.max_string_bytes))};
        }
        const auto bytes{take(length)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        return decode_modified_utf8(*bytes);
    }

    /// Read `count` raw bytes as a NBT byte array (TAG_Byte_Array payload).
    [[nodiscard]] auto u8_array(std::size_t count) -> result<byte_array> {
        const auto chunk{take(count)};
        if (!chunk) {
            return std::unexpected{chunk.error()};
        }
        byte_array items(count);
        std::transform(
            chunk->begin(), chunk->end(), items.begin(),
            [](std::byte item) { return std::to_integer<std::uint8_t>(item); });
        return items;
    }

    [[nodiscard]] auto count_tag() -> status {
        ++tag_count_;
        if (tag_count_ > bounds_.max_tags) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format("nbt: document contains more than {} tags",
                                 bounds_.max_tags))};
        }
        return status_ok();
    }

  private:
    [[nodiscard]] auto take(std::size_t count)
        -> result<std::span<const std::byte>> {
        if (count > remaining()) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: truncated input (need {} byte(s) at offset "
                            "{}, only {} available)",
                            count, position_, remaining()))};
        }
        const auto chunk{data_.subspan(position_, count)};
        position_ += count;
        return chunk;
    }

    std::span<const std::byte> data_;
    const limits &bounds_;
    std::size_t position_{0};
    std::size_t tag_count_{0};
};

// --- parser ---------------------------------------------------------------

class parser {
  public:
    parser(std::span<const std::byte> data, const limits &bounds) noexcept
        : reader_{data, bounds}, bounds_{bounds} {}

    [[nodiscard]] auto parse_root() -> result<value> {
        const auto root_tag{reader_.u8()};
        if (!root_tag) {
            return std::unexpected{root_tag.error()};
        }
        if (*root_tag != tag_id(tag_type::compound)) {
            const auto name{
                valid_tag_id(*root_tag)
                    ? tag_type_name(static_cast<tag_type>(*root_tag))
                    : std::string_view{"unknown tag"}};
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: root tag must be TAG_Compound, got {} (id "
                            "{})",
                            name, *root_tag))};
        }
        // The root name is part of the wire format and validated as a string,
        // but it is not part of the value returned to callers (the file name is
        // the identity in the world/structure pipeline).
        auto root_name{reader_.string()};
        if (!root_name) {
            return std::unexpected{root_name.error()};
        }
        auto root{parse_payload(tag_type::compound, 1)};
        if (!root) {
            return std::unexpected{root.error()};
        }
        if (reader_.remaining() != 0) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: {} trailing byte(s) after the root compound",
                            reader_.remaining()))};
        }
        return root;
    }

  private:
    [[nodiscard]] auto parse_payload(tag_type kind, std::uint32_t depth)
        -> result<value> {
        if (auto counted{reader_.count_tag()}; !counted) {
            return std::unexpected{counted.error()};
        }
        switch (kind) {
        case tag_type::end:
            return std::unexpected{fail(error_code::parse_error,
                                        "nbt: TAG_End is not a valid tag "
                                        "payload")};
        case tag_type::byte: {
            const auto item{reader_.u8()};
            if (!item) {
                return std::unexpected{item.error()};
            }
            return make_byte(std::bit_cast<std::int8_t>(*item));
        }
        case tag_type::short_: {
            const auto item{reader_.u16()};
            if (!item) {
                return std::unexpected{item.error()};
            }
            return make_short(std::bit_cast<std::int16_t>(*item));
        }
        case tag_type::int_: {
            const auto item{reader_.i32()};
            if (!item) {
                return std::unexpected{item.error()};
            }
            return make_int(*item);
        }
        case tag_type::long_: {
            const auto item{reader_.i64()};
            if (!item) {
                return std::unexpected{item.error()};
            }
            return make_long(*item);
        }
        case tag_type::float_: {
            const auto bits{reader_.u32()};
            if (!bits) {
                return std::unexpected{bits.error()};
            }
            return make_float(std::bit_cast<float>(*bits));
        }
        case tag_type::double_: {
            const auto bits{reader_.u64()};
            if (!bits) {
                return std::unexpected{bits.error()};
            }
            return make_double(std::bit_cast<double>(*bits));
        }
        case tag_type::byte_array:
            return parse_byte_array();
        case tag_type::string: {
            auto text{reader_.string()};
            if (!text) {
                return std::unexpected{text.error()};
            }
            return make_string(std::move(*text));
        }
        case tag_type::list:
            return parse_list(depth);
        case tag_type::compound:
            return parse_compound(depth);
        case tag_type::int_array:
            return parse_int_array();
        case tag_type::long_array:
            return parse_long_array();
        }
        std::unreachable();
    }

    [[nodiscard]] auto parse_compound(std::uint32_t depth) -> result<value> {
        const auto depth_limit{
            std::min(bounds_.max_depth, max_supported_depth)};
        if (depth > depth_limit) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format("nbt: nesting depth {} exceeds the limit of "
                                 "{}",
                                 depth, depth_limit))};
        }
        compound entries;
        for (;;) {
            const auto raw_type{reader_.u8()};
            if (!raw_type) {
                return std::unexpected{raw_type.error()};
            }
            if (*raw_type == tag_id(tag_type::end)) {
                break;
            }
            if (!valid_tag_id(*raw_type)) {
                return std::unexpected{
                    fail(error_code::parse_error,
                         std::format("nbt: unknown tag id {} in compound",
                                     *raw_type))};
            }
            auto name{reader_.string()};
            if (!name) {
                return std::unexpected{name.error()};
            }
            auto child{
                parse_payload(static_cast<tag_type>(*raw_type), depth + 1)};
            if (!child) {
                return std::unexpected{child.error()};
            }
            // Duplicate names: Java's NBT writer emits a map, so a duplicate
            // can only come from foreign/corrupt files. Last occurrence wins
            // (the same rule as Java's NBT reader loop).
            entries.insert_or_assign(std::move(*name), std::move(*child));
        }
        return make_compound(std::move(entries));
    }

    [[nodiscard]] auto parse_list(std::uint32_t depth) -> result<value> {
        const auto depth_limit{
            std::min(bounds_.max_depth, max_supported_depth)};
        if (depth > depth_limit) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format("nbt: nesting depth {} exceeds the limit of "
                                 "{}",
                                 depth, depth_limit))};
        }
        const auto raw_element{reader_.u8()};
        if (!raw_element) {
            return std::unexpected{raw_element.error()};
        }
        if (!valid_tag_id(*raw_element)) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format("nbt: unknown list element tag id {}",
                                 *raw_element))};
        }
        const auto element_type{static_cast<tag_type>(*raw_element)};
        const auto raw_length{reader_.i32()};
        if (!raw_length) {
            return std::unexpected{raw_length.error()};
        }
        if (*raw_length < 0) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format("nbt: negative list length {}", *raw_length))};
        }
        const auto count{static_cast<std::size_t>(*raw_length)};
        if (count > bounds_.max_collection_items) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: list length {} exceeds the configured limit "
                            "of {}",
                            count, bounds_.max_collection_items))};
        }
        if (element_type == tag_type::end && count != 0) {
            return std::unexpected{fail(
                error_code::parse_error,
                "nbt: non-empty list declares TAG_End as its element type")};
        }
        // Every list element occupies at least one wire byte, so a declared
        // count above the remaining input is guaranteed to be truncated. This
        // also keeps hostile counts from reserving memory before the first
        // element parse fails.
        if (count > reader_.remaining()) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: truncated list (declared {} element(s), {} "
                            "byte(s) left)",
                            count, reader_.remaining()))};
        }
        std::vector<value> items;
        // Never reserve the declared length blindly: a hostile file can claim a
        // large count with a short payload, and the per-item parse below fails
        // fast on truncation.
        items.reserve(std::min<std::size_t>(count, 4096U));
        for (std::size_t item_index{0}; item_index < count; ++item_index) {
            auto item{parse_payload(element_type, depth + 1)};
            if (!item) {
                return std::unexpected{item.error()};
            }
            items.push_back(std::move(*item));
        }
        return make_list(element_type, std::move(items));
    }

    [[nodiscard]] auto parse_byte_array() -> result<value> {
        const auto raw_length{reader_.i32()};
        if (!raw_length) {
            return std::unexpected{raw_length.error()};
        }
        if (*raw_length < 0) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format("nbt: negative byte-array length {}",
                                 *raw_length))};
        }
        const auto count{static_cast<std::size_t>(*raw_length)};
        if (count > bounds_.max_collection_items) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: byte-array length {} exceeds the configured "
                            "limit of {}",
                            count, bounds_.max_collection_items))};
        }
        auto bytes{reader_.u8_array(count)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        byte_array items{std::move(*bytes)};
        return make_byte_array(std::move(items));
    }

    [[nodiscard]] auto parse_int_array() -> result<value> {
        const auto raw_length{reader_.i32()};
        if (!raw_length) {
            return std::unexpected{raw_length.error()};
        }
        if (*raw_length < 0) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: negative int-array length {}", *raw_length))};
        }
        const auto count{static_cast<std::size_t>(*raw_length)};
        if (count > bounds_.max_collection_items) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: int-array length {} exceeds the configured "
                            "limit of {}",
                            count, bounds_.max_collection_items))};
        }
        if (count > reader_.remaining() / 4U) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: truncated int array (declared {} element(s), "
                            "{} byte(s) left)",
                            count, reader_.remaining()))};
        }
        int_array items;
        items.reserve(count);
        for (std::size_t item_index{0}; item_index < count; ++item_index) {
            const auto item{reader_.i32()};
            if (!item) {
                return std::unexpected{item.error()};
            }
            items.push_back(*item);
        }
        return make_int_array(std::move(items));
    }

    [[nodiscard]] auto parse_long_array() -> result<value> {
        const auto raw_length{reader_.i32()};
        if (!raw_length) {
            return std::unexpected{raw_length.error()};
        }
        if (*raw_length < 0) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format("nbt: negative long-array length {}",
                                 *raw_length))};
        }
        const auto count{static_cast<std::size_t>(*raw_length)};
        if (count > bounds_.max_collection_items) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: long-array length {} exceeds the configured "
                            "limit of {}",
                            count, bounds_.max_collection_items))};
        }
        if (count > reader_.remaining() / 8U) {
            return std::unexpected{
                fail(error_code::parse_error,
                     std::format(
                         "nbt: truncated long array (declared {} element(s), "
                         "{} byte(s) left)",
                         count, reader_.remaining()))};
        }
        long_array items;
        items.reserve(count);
        for (std::size_t item_index{0}; item_index < count; ++item_index) {
            const auto item{reader_.i64()};
            if (!item) {
                return std::unexpected{item.error()};
            }
            items.push_back(*item);
        }
        return make_long_array(std::move(items));
    }

    byte_reader reader_;
    const limits &bounds_;
};

// --- big-endian writer ----------------------------------------------------

class byte_writer {
  public:
    void u8(std::uint8_t item) { out_.push_back(static_cast<std::byte>(item)); }

    void u16(std::uint16_t item) {
        u8(static_cast<std::uint8_t>(item >> 8U));
        u8(static_cast<std::uint8_t>(item & 0xFFU));
    }

    void u32(std::uint32_t item) {
        u8(static_cast<std::uint8_t>(item >> 24U));
        u8(static_cast<std::uint8_t>((item >> 16U) & 0xFFU));
        u8(static_cast<std::uint8_t>((item >> 8U) & 0xFFU));
        u8(static_cast<std::uint8_t>(item & 0xFFU));
    }

    void u64(std::uint64_t item) {
        u8(static_cast<std::uint8_t>(item >> 56U));
        u8(static_cast<std::uint8_t>((item >> 48U) & 0xFFU));
        u8(static_cast<std::uint8_t>((item >> 40U) & 0xFFU));
        u8(static_cast<std::uint8_t>((item >> 32U) & 0xFFU));
        u8(static_cast<std::uint8_t>((item >> 24U) & 0xFFU));
        u8(static_cast<std::uint8_t>((item >> 16U) & 0xFFU));
        u8(static_cast<std::uint8_t>((item >> 8U) & 0xFFU));
        u8(static_cast<std::uint8_t>(item & 0xFFU));
    }

    void bytes(std::span<const std::byte> data) {
        out_.insert(out_.end(), data.begin(), data.end());
    }

    void bytes(std::span<const std::uint8_t> data) {
        for (const auto item : data) {
            u8(item);
        }
    }

    void string_bytes(std::string_view text) {
        for (const auto character : text) {
            out_.push_back(
                static_cast<std::byte>(static_cast<std::uint8_t>(character)));
        }
    }

    [[nodiscard]] auto take() noexcept -> std::vector<std::byte> {
        return std::move(out_);
    }

  private:
    std::vector<std::byte> out_;
};

/// u16-length-prefixed modified UTF-8 string (names and TAG_String payloads).
[[nodiscard]] auto write_string(byte_writer &writer, std::string_view text,
                                const limits &bounds) -> status {
    auto encoded{encode_modified_utf8(text)};
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    const auto wire_limit{std::min<std::size_t>(
        bounds.max_string_bytes, std::numeric_limits<std::uint16_t>::max())};
    if (encoded->size() > wire_limit) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            std::format("nbt: string is {} modified UTF-8 bytes, exceeding the "
                        "NBT limit of {} bytes",
                        encoded->size(), wire_limit))};
    }
    writer.u16(static_cast<std::uint16_t>(encoded->size()));
    writer.string_bytes(*encoded);
    return status_ok();
}

[[nodiscard]] auto write_payload(byte_writer &writer, const value &node,
                                 std::uint32_t depth, const limits &bounds,
                                 std::size_t &tag_count) -> status {
    ++tag_count;
    if (tag_count > bounds.max_tags) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 std::format("nbt: document contains more than {} tags",
                             bounds.max_tags))};
    }
    switch (node.type()) {
    case tag_type::end:
        return std::unexpected{fail(error_code::invalid_argument,
                                    "nbt: TAG_End cannot be encoded as a "
                                    "value")};
    case tag_type::byte:
        writer.u8(static_cast<std::uint8_t>(*node.as_byte()));
        return status_ok();
    case tag_type::short_:
        writer.u16(static_cast<std::uint16_t>(*node.as_short()));
        return status_ok();
    case tag_type::int_:
        writer.u32(static_cast<std::uint32_t>(*node.as_int()));
        return status_ok();
    case tag_type::long_:
        writer.u64(static_cast<std::uint64_t>(*node.as_long()));
        return status_ok();
    case tag_type::float_:
        writer.u32(std::bit_cast<std::uint32_t>(*node.as_float()));
        return status_ok();
    case tag_type::double_:
        writer.u64(std::bit_cast<std::uint64_t>(*node.as_double()));
        return status_ok();
    case tag_type::string:
        return write_string(writer, *node.as_string(), bounds);
    case tag_type::byte_array: {
        const auto items{*node.as_byte_array()};
        if (items.size() > bounds.max_collection_items) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                std::format("nbt: byte array length {} exceeds the configured "
                            "limit of {}",
                            items.size(), bounds.max_collection_items))};
        }
        writer.u32(static_cast<std::uint32_t>(items.size()));
        writer.bytes(items);
        return status_ok();
    }
    case tag_type::int_array: {
        const auto items{*node.as_int_array()};
        if (items.size() > bounds.max_collection_items) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                std::format("nbt: int array length {} exceeds the configured "
                            "limit of {}",
                            items.size(), bounds.max_collection_items))};
        }
        writer.u32(static_cast<std::uint32_t>(items.size()));
        for (const auto item : items) {
            writer.u32(static_cast<std::uint32_t>(item));
        }
        return status_ok();
    }
    case tag_type::long_array: {
        const auto items{*node.as_long_array()};
        if (items.size() > bounds.max_collection_items) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                std::format("nbt: long array length {} exceeds the configured "
                            "limit of {}",
                            items.size(), bounds.max_collection_items))};
        }
        writer.u32(static_cast<std::uint32_t>(items.size()));
        for (const auto item : items) {
            writer.u64(static_cast<std::uint64_t>(item));
        }
        return status_ok();
    }
    case tag_type::list: {
        const auto depth_limit{std::min(bounds.max_depth, max_supported_depth)};
        if (depth > depth_limit) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                std::format("nbt: nesting depth {} exceeds the limit of {}",
                            depth, depth_limit))};
        }
        const auto *items{node.as_list()};
        const auto count{items->items.size()};
        if (count > bounds.max_collection_items) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                std::format("nbt: list length {} exceeds the configured limit "
                            "of {}",
                            count, bounds.max_collection_items))};
        }
        if (items->element_type == tag_type::end) {
            if (count != 0) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         "nbt: non-empty list declares TAG_End as its element "
                         "type")};
            }
            writer.u8(tag_id(tag_type::end));
            writer.u32(0);
            return status_ok();
        }
        if (!valid_tag_id(tag_id(items->element_type))) {
            return std::unexpected{fail(error_code::invalid_argument,
                                        "nbt: list has an unknown element tag "
                                        "type")};
        }
        for (const auto &item : items->items) {
            if (item.type() != items->element_type) {
                return std::unexpected{fail(
                    error_code::invalid_argument,
                    std::format("nbt: list element type mismatch (declared {}, "
                                "item is {})",
                                tag_type_name(items->element_type),
                                tag_type_name(item.type())))};
            }
        }
        writer.u8(tag_id(items->element_type));
        writer.u32(static_cast<std::uint32_t>(count));
        for (const auto &item : items->items) {
            if (auto written{
                    write_payload(writer, item, depth + 1U, bounds, tag_count)};
                !written) {
                return written;
            }
        }
        return status_ok();
    }
    case tag_type::compound: {
        const auto depth_limit{std::min(bounds.max_depth, max_supported_depth)};
        if (depth > depth_limit) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                std::format("nbt: nesting depth {} exceeds the limit of {}",
                            depth, depth_limit))};
        }
        const auto *entries{node.as_compound()};
        for (const auto &[key, child] : *entries) {
            writer.u8(tag_id(child.type()));
            if (auto written{write_string(writer, key, bounds)}; !written) {
                return written;
            }
            if (auto written{write_payload(writer, child, depth + 1U, bounds,
                                           tag_count)};
                !written) {
                return written;
            }
        }
        writer.u8(tag_id(tag_type::end));
        return status_ok();
    }
    }
    std::unreachable();
}

[[nodiscard]] auto serialize_root(const value &root, std::string_view root_name,
                                  const limits &bounds)
    -> result<std::vector<std::byte>> {
    if (root.type() != tag_type::compound) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 std::format("nbt: root value must be TAG_Compound, got {}",
                             tag_type_name(root.type())))};
    }
    byte_writer writer;
    std::size_t tag_count{0};
    writer.u8(tag_id(tag_type::compound));
    if (auto written{write_string(writer, root_name, bounds)}; !written) {
        return std::unexpected{written.error()};
    }
    if (auto written{write_payload(writer, root, 1, bounds, tag_count)};
        !written) {
        return std::unexpected{written.error()};
    }
    return writer.take();
}

// --- compression ----------------------------------------------------------

/// RAII zlib state; `inflateEnd`/`deflateEnd` must run on every exit path
/// (including early returns), so a small scope guard keeps the loops clean.
template <typename EndFunction> struct zlib_end_guard {
    z_stream *stream;
    EndFunction end;
    zlib_end_guard(z_stream *target, EndFunction finish)
        : stream{target}, end{finish} {}
    zlib_end_guard(const zlib_end_guard &) = delete;
    auto operator=(const zlib_end_guard &) -> zlib_end_guard & = delete;
    ~zlib_end_guard() { static_cast<void>(end(stream)); }
};

[[nodiscard]] auto decompress(std::span<const std::byte> input,
                              compression mode, const limits &bounds)
    -> result<std::vector<std::byte>> {
    if (mode == compression::none) {
        if (input.size() > bounds.max_bytes) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: input of {} bytes exceeds the configured "
                            "limit of {} bytes",
                            input.size(), bounds.max_bytes))};
        }
        return std::vector<std::byte>{input.begin(), input.end()};
    }
    z_stream stream{};
    // 15 = maximum zlib window; +16 asks zlib for the gzip wrapper.
    const auto window_bits{
        static_cast<int>(mode == compression::gzip ? 15 + 16 : 15)};
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        return std::unexpected{
            fail(error_code::out_of_memory,
                 "nbt: cannot initialise the zlib inflate stream")};
    }
    const auto guard{zlib_end_guard{
        &stream, [](z_stream *state) { return inflateEnd(state); }}};
    std::vector<std::byte> output;
    output.reserve(std::min<std::size_t>(input.size(), bounds.max_bytes));
    std::array<std::byte, 64U * 1024U> chunk{};
    std::size_t consumed{0};
    for (;;) {
        if (stream.avail_in == 0 && consumed < input.size()) {
            const auto feed{std::min<std::size_t>(
                input.size() - consumed,
                std::numeric_limits<unsigned int>::max())};
            stream.next_in = reinterpret_cast<Bytef *>(
                const_cast<std::byte *>(input.data() + consumed));
            stream.avail_in = static_cast<unsigned int>(feed);
            consumed += feed;
        }
        const auto before_in{stream.total_in};
        const auto before_out{stream.total_out};
        stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
        stream.avail_out = static_cast<unsigned int>(chunk.size());
        const auto code{inflate(&stream, Z_NO_FLUSH)};
        const auto produced{chunk.size() -
                            static_cast<std::size_t>(stream.avail_out)};
        if (output.size() + produced > bounds.max_bytes) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: decompressed data exceeds the configured "
                            "limit of {} bytes",
                            bounds.max_bytes))};
        }
        output.insert(output.end(), chunk.begin(),
                      chunk.begin() + static_cast<std::ptrdiff_t>(produced));
        if (code == Z_STREAM_END) {
            break;
        }
        if (code != Z_OK && code != Z_BUF_ERROR) {
            return std::unexpected{fail(
                error_code::parse_error,
                std::format("nbt: corrupt compressed stream (zlib error {})",
                            code))};
        }
        if (stream.total_in == before_in && stream.total_out == before_out) {
            // No progress. With no input left this is a truncated stream; with
            // input left it means the inflater refused the remaining bytes.
            if (consumed >= input.size() && stream.avail_in == 0) {
                return std::unexpected{
                    fail(error_code::parse_error,
                         "nbt: truncated compressed stream (no end-of-stream "
                         "marker)")};
            }
            return std::unexpected{
                fail(error_code::parse_error,
                     "nbt: corrupt compressed stream (inflate stalled)")};
        }
    }
    const auto trailing{static_cast<std::size_t>(stream.avail_in) +
                        (input.size() - consumed)};
    if (trailing != 0) {
        return std::unexpected{fail(
            error_code::parse_error,
            std::format("nbt: {} trailing byte(s) after the compressed stream",
                        trailing))};
    }
    return output;
}

[[nodiscard]] auto compress(std::span<const std::byte> data, compression mode)
    -> result<std::vector<std::byte>> {
    z_stream stream{};
    const auto window_bits{
        static_cast<int>(mode == compression::gzip ? 15 + 16 : 15)};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return std::unexpected{
            fail(error_code::out_of_memory,
                 "nbt: cannot initialise the zlib deflate stream")};
    }
    const auto guard{zlib_end_guard{
        &stream, [](z_stream *state) { return deflateEnd(state); }}};
    if (mode == compression::gzip) {
        // Fixed gzip header: no wall-clock timestamp and "unknown" OS byte, so
        // equal trees produce byte-identical output on every platform.
        gz_header header{};
        header.time = 0;
        header.os = 255;
        if (deflateSetHeader(&stream, &header) != Z_OK) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "nbt: cannot configure the gzip header")};
        }
    }
    const auto bound{static_cast<std::size_t>(
        deflateBound(&stream, static_cast<uLong>(data.size())))};
    // +64 covers the gzip wrapper bytes deflateBound does not account for.
    std::size_t capacity{bound + 64U};
    capacity = std::min<std::size_t>(
        capacity,
        static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()));
    std::vector<std::byte> output(capacity);
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());
    std::size_t consumed{0};
    for (;;) {
        if (stream.avail_in == 0 && consumed < data.size()) {
            const auto feed{std::min<std::size_t>(
                data.size() - consumed,
                std::numeric_limits<unsigned int>::max())};
            stream.next_in = reinterpret_cast<Bytef *>(
                const_cast<std::byte *>(data.data() + consumed));
            stream.avail_in = static_cast<unsigned int>(feed);
            consumed += feed;
        }
        const auto flush{(consumed == data.size() && stream.avail_in == 0)
                             ? Z_FINISH
                             : Z_NO_FLUSH};
        const auto code{deflate(&stream, flush)};
        if (code == Z_STREAM_END) {
            break;
        }
        if (code != Z_OK && code != Z_BUF_ERROR) {
            return std::unexpected{fail(
                error_code::io_error,
                std::format("nbt: compression failed (zlib error {})", code))};
        }
        if (stream.avail_out == 0) {
            const auto used{output.size()};
            output.resize(used == 0 ? 4096U : used * 2U);
            stream.next_out = reinterpret_cast<Bytef *>(output.data() + used);
            stream.avail_out = static_cast<unsigned int>(output.size() - used);
        }
    }
    output.resize(static_cast<std::size_t>(stream.total_out));
    return output;
}

} // namespace

// --- value -----------------------------------------------------------------

auto value::type() const noexcept -> tag_type {
    // Alternative order must stay in sync with `storage` above.
    static constexpr std::array<tag_type, 12> kinds{
        tag_type::byte,       tag_type::short_,    tag_type::int_,
        tag_type::long_,      tag_type::float_,    tag_type::double_,
        tag_type::byte_array, tag_type::string,    tag_type::list,
        tag_type::compound,   tag_type::int_array, tag_type::long_array};
    return kinds[data_.index()];
}

auto value::size() const noexcept -> std::size_t {
    return std::visit(
        [](const auto &stored) -> std::size_t {
            using stored_type = std::decay_t<decltype(stored)>;
            if constexpr (std::is_same_v<stored_type, list_value>) {
                return stored.items.size();
            } else if constexpr (std::is_same_v<stored_type, compound>) {
                return stored.size();
            } else if constexpr (std::is_same_v<stored_type, byte_array> ||
                                 std::is_same_v<stored_type, int_array> ||
                                 std::is_same_v<stored_type, long_array> ||
                                 std::is_same_v<stored_type, std::string>) {
                return stored.size();
            } else {
                return 1;
            }
        },
        data_);
}

auto value::as_byte() const noexcept -> std::optional<std::int8_t> {
    return view<std::int8_t>();
}

auto value::as_short() const noexcept -> std::optional<std::int16_t> {
    return view<std::int16_t>();
}

auto value::as_int() const noexcept -> std::optional<std::int32_t> {
    return view<std::int32_t>();
}

auto value::as_long() const noexcept -> std::optional<std::int64_t> {
    return view<std::int64_t>();
}

auto value::as_float() const noexcept -> std::optional<float> {
    return view<float>();
}

auto value::as_double() const noexcept -> std::optional<double> {
    return view<double>();
}

auto value::as_string() const noexcept -> std::optional<std::string_view> {
    if (const auto *stored{std::get_if<std::string>(&data_)};
        stored != nullptr) {
        return std::string_view{*stored};
    }
    return std::nullopt;
}

auto value::as_byte_array() const noexcept
    -> std::optional<std::span<const std::uint8_t>> {
    if (const auto *stored{std::get_if<byte_array>(&data_)};
        stored != nullptr) {
        return std::span<const std::uint8_t>{*stored};
    }
    return std::nullopt;
}

auto value::as_int_array() const noexcept
    -> std::optional<std::span<const std::int32_t>> {
    if (const auto *stored{std::get_if<int_array>(&data_)}; stored != nullptr) {
        return std::span<const std::int32_t>{*stored};
    }
    return std::nullopt;
}

auto value::as_long_array() const noexcept
    -> std::optional<std::span<const std::int64_t>> {
    if (const auto *stored{std::get_if<long_array>(&data_)};
        stored != nullptr) {
        return std::span<const std::int64_t>{*stored};
    }
    return std::nullopt;
}

auto value::as_list() const noexcept -> const list_value * {
    return std::get_if<list_value>(&data_);
}

auto value::as_compound() const noexcept -> const compound * {
    return std::get_if<compound>(&data_);
}

auto value::list_element_type() const noexcept -> std::optional<tag_type> {
    if (const auto *stored{std::get_if<list_value>(&data_)};
        stored != nullptr) {
        return stored->element_type;
    }
    return std::nullopt;
}

auto value::get(std::string_view key) noexcept -> value * {
    auto *entries{std::get_if<compound>(&data_)};
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found{entries->find(key)};
    return found == entries->end() ? nullptr : &found->second;
}

auto value::get(std::string_view key) const noexcept -> const value * {
    const auto *entries{std::get_if<compound>(&data_)};
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found{entries->find(key)};
    return found == entries->end() ? nullptr : &found->second;
}

auto value::contains(std::string_view key) const noexcept -> bool {
    return get(key) != nullptr;
}

auto value::set(std::string key, value child) -> status {
    auto *entries{std::get_if<compound>(&data_)};
    if (entries == nullptr) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "nbt: set() requires a TAG_Compound "
                                    "value")};
    }
    // `insert_or_assign` with the key_type keeps transparent lookup semantics;
    // building is not on the hot path, so the extra move is fine.
    entries->insert_or_assign(std::move(key), std::move(child));
    return status_ok();
}

auto value::push(value item) -> status {
    auto *entries{std::get_if<list_value>(&data_)};
    if (entries == nullptr) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "nbt: push() requires a TAG_List value")};
    }
    if (entries->element_type == tag_type::end) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "nbt: cannot append to a list declared with TAG_End (create the "
            "list with the intended element type)")};
    }
    if (item.type() != entries->element_type) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            std::format("nbt: list element type mismatch (declared {}, item "
                        "is {})",
                        tag_type_name(entries->element_type),
                        tag_type_name(item.type())))};
    }
    entries->items.push_back(std::move(item));
    return status_ok();
}

// --- builders -------------------------------------------------------------

auto make_byte(std::int8_t item) -> value {
    return value{value::storage{std::in_place_type<std::int8_t>, item}};
}

auto make_short(std::int16_t item) -> value {
    return value{value::storage{std::in_place_type<std::int16_t>, item}};
}

auto make_int(std::int32_t item) -> value {
    return value{value::storage{std::in_place_type<std::int32_t>, item}};
}

auto make_long(std::int64_t item) -> value {
    return value{value::storage{std::in_place_type<std::int64_t>, item}};
}

auto make_float(float item) -> value {
    return value{value::storage{std::in_place_type<float>, item}};
}

auto make_double(double item) -> value {
    return value{value::storage{std::in_place_type<double>, item}};
}

auto make_string(std::string text) -> value {
    return value{
        value::storage{std::in_place_type<std::string>, std::move(text)}};
}

auto make_byte_array(byte_array items) -> value {
    return value{
        value::storage{std::in_place_type<byte_array>, std::move(items)}};
}

auto make_int_array(int_array items) -> value {
    return value{
        value::storage{std::in_place_type<int_array>, std::move(items)}};
}

auto make_long_array(long_array items) -> value {
    return value{
        value::storage{std::in_place_type<long_array>, std::move(items)}};
}

auto make_compound() -> value {
    return value{value::storage{std::in_place_type<compound>}};
}

auto make_compound(compound entries) -> value {
    return value{
        value::storage{std::in_place_type<compound>, std::move(entries)}};
}

auto make_list(tag_type element_type) -> value {
    return value{value::storage{std::in_place_type<list_value>,
                                list_value{element_type, {}}}};
}

auto make_list(tag_type element_type, std::vector<value> items) -> value {
    return value{value::storage{std::in_place_type<list_value>,
                                list_value{element_type, std::move(items)}}};
}

// --- public codec API -----------------------------------------------------

auto read(std::span<const std::byte> data, compression mode,
          const limits &bounds) -> result<value> {
    auto decompressed{decompress(data, mode, bounds)};
    if (!decompressed) {
        return std::unexpected{decompressed.error()};
    }
    parser source_parser{*decompressed, bounds};
    return source_parser.parse_root();
}

auto write(const value &root, std::string_view root_name, compression mode,
           const limits &bounds) -> result<std::vector<std::byte>> {
    auto raw{serialize_root(root, root_name, bounds)};
    if (!raw) {
        return std::unexpected{raw.error()};
    }
    if (mode == compression::none) {
        return raw;
    }
    return compress(*raw, mode);
}

auto read_file(std::string_view path, compression mode, const limits &bounds)
    -> result<value> {
    auto bytes{core::io::read_file(path)};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    return read(*bytes, mode, bounds);
}

auto write_file(std::string_view path, const value &root,
                std::string_view root_name, compression mode,
                const limits &bounds) -> status {
    auto encoded{write(root, root_name, mode, bounds)};
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    return core::io::atomic_write_file(path, *encoded);
}

} // namespace refrain::data::nbt
