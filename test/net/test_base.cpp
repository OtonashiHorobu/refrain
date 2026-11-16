// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/base.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

[[nodiscard]] auto bytes_of(std::initializer_list<std::uint8_t> values)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

[[nodiscard]] auto encode_varint(std::uint32_t value)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    net::writer writer{out};
    writer.write_varint(value);
    return out;
}

const suite tests = [] {
    "net/varint round trips boundary values"_test = [] {
        for (const auto value :
             {0U, 1U, 127U, 128U, 255U, 25565U, 2'097'151U, 0x7FFFFFFFU}) {
            const auto encoded{encode_varint(value)};
            net::reader reader{encoded};
            const auto decoded{reader.read_varint()};
            expect(decoded.has_value());
            if (decoded) {
                expect(*decoded == value);
            }
            expect(reader.empty());
        }
    };

    "net/varint rejects oversized and truncated input"_test = [] {
        const auto six_bytes{bytes_of({0x80, 0x80, 0x80, 0x80, 0x80, 0x01})};
        net::reader overlong{six_bytes};
        expect(!overlong.read_varint().has_value());

        const auto overflow{bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x1F})};
        net::reader wide{overflow};
        expect(!wide.read_varint().has_value());

        const auto truncated{bytes_of({0x80, 0x80})};
        net::reader short_input{truncated};
        expect(!short_input.read_varint().has_value());
    };

    "net/varlong round trips boundary values"_test = [] {
        for (const auto value :
             {0ULL, 127ULL, 128ULL, 0xFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL}) {
            std::vector<std::byte> out;
            net::writer writer{out};
            writer.write_varlong(value);
            net::reader reader{out};
            const auto decoded{reader.read_varlong()};
            expect(decoded.has_value());
            if (decoded) {
                expect(*decoded == value);
            }
        }
    };

    "net/readers reject truncated fixed-width values"_test = [] {
        const auto data{bytes_of({0x01, 0x02, 0x03})};
        net::reader reader{data};
        expect(!reader.read_i32().has_value());
        expect(reader.position() == 0U);
    };

    "net/strings enforce character limits and UTF-8 validity"_test = [] {
        std::vector<std::byte> out;
        net::writer writer{out};
        expect(writer.write_string("hello", 5).has_value());
        expect(!writer.write_string("hello", 4).has_value());
        expect(
            !writer.write_string(std::string_view{"\xFF", 1}, 4).has_value());

        net::reader reader{out};
        const auto value{reader.read_string(5)};
        expect(value.has_value());
        if (value) {
            expect(*value == "hello");
        }
    };

    "net/strings reject a byte length above the field ceiling"_test = [] {
        std::vector<std::byte> out;
        net::writer writer{out};
        for (char character : std::string(9, 'a')) {
            writer.write_u8(static_cast<std::uint8_t>(character));
        }
        // Hand-build a string with a VarInt length but no limit check.
        std::vector<std::byte> encoded;
        net::writer encoded_writer{encoded};
        encoded_writer.write_varint(8);
        for (int index{0}; index < 8; ++index) {
            encoded_writer.write_u8(static_cast<std::uint8_t>('a'));
        }
        net::reader reader{encoded};
        expect(!reader.read_string(1).has_value());
    };

    "net/uuid round trips its textual form"_test = [] {
        const auto parsed{
            net::uuid::parse("12345678-90ab-cdef-8123-456789abcdef")};
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        expect(parsed->to_string() == "12345678-90ab-cdef-8123-456789abcdef");

        std::vector<std::byte> out;
        net::writer writer{out};
        expect(writer.write_uuid(*parsed).has_value());
        net::reader reader{out};
        const auto decoded{reader.read_uuid()};
        expect(decoded.has_value());
        if (decoded) {
            expect(*decoded == *parsed);
        }
    };

    "net/uuid rejects malformed text"_test = [] {
        expect(!net::uuid::parse("1234567890abcdef").has_value());
        expect(!net::uuid::parse("12345678-90ab-cdef-8123-456789abcdeg")
                    .has_value());
        expect(!net::uuid::parse("12345678-90ab-cdef-8123-456789abcde")
                    .has_value());
    };

    "net/big-endian scalars match the wire order"_test = [] {
        std::vector<std::byte> out;
        net::writer writer{out};
        writer.write_u16(0x1234);
        writer.write_i32(0x01020304);
        writer.write_bool(true);
        expect(out == bytes_of({0x12, 0x34, 0x01, 0x02, 0x03, 0x04, 0x01}));

        net::reader reader{out};
        const auto u16{reader.read_u16()};
        const auto i32{reader.read_i32()};
        const auto flag{reader.read_bool()};
        expect(u16.has_value() && *u16 == 0x1234);
        expect(i32.has_value() && *i32 == 0x01020304);
        expect(flag.has_value() && *flag);
    };

    "net/big-endian scalars keep the wire order at every width"_test = [] {
        std::vector<std::byte> out;
        net::writer writer{out};
        writer.write_u16(0x1234);
        writer.write_i32(0x01020304);
        writer.write_i64(0x0102030405060708);
        writer.write_f32(1.5F);
        writer.write_f64(-2.25);
        writer.write_bool(false);
        expect(out == bytes_of({0x12, 0x34, 0x01, 0x02, 0x03, 0x04, 0x01,
                                0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                0x3F, 0xC0, 0x00, 0x00, 0xC0, 0x02, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

        net::reader reader{out};
        const auto u16{reader.read_u16()};
        const auto i32{reader.read_i32()};
        const auto i64{reader.read_i64()};
        const auto f32{reader.read_f32()};
        const auto f64{reader.read_f64()};
        const auto flag{reader.read_bool()};
        expect(u16.has_value() && *u16 == 0x1234);
        expect(i32.has_value() && *i32 == 0x01020304);
        expect(i64.has_value() && *i64 == 0x0102030405060708);
        expect(f32.has_value() && *f32 == 1.5F);
        expect(f64.has_value() && *f64 == -2.25);
        expect(flag.has_value() && !*flag);
        expect(reader.empty());
    };

    "net/truncated big-endian scalars fail without consuming bytes"_test = [] {
        const auto body{
            bytes_of({0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0})};
        const auto expect_parse_error = [](auto result) {
            expect(!result.has_value());
            if (!result) {
                expect(result.error().code == refrain::error_code::parse_error);
            }
        };

        net::reader short16{std::span<const std::byte>{body}.first(1)};
        expect_parse_error(short16.read_u16());
        expect(short16.position() == 0);
        expect(short16.read_u8().value_or(0) == 0x12);

        net::reader short32{std::span<const std::byte>{body}.first(3)};
        expect_parse_error(short32.read_i32());
        expect(short32.position() == 0);

        net::reader short64{std::span<const std::byte>{body}.first(7)};
        expect_parse_error(short64.read_i64());
        expect(short64.position() == 0);

        net::reader short_f32{std::span<const std::byte>{body}.first(3)};
        expect_parse_error(short_f32.read_f32());
        net::reader short_f64{std::span<const std::byte>{body}.first(7)};
        expect_parse_error(short_f64.read_f64());
    };
};

} // namespace
