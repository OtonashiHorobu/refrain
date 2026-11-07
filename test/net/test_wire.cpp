// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/wire.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

const suite tests = [] {
    "text component writes a network nbt string tag"_test = [] {
        const net::text_component value{.text = "abc"};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_text_component(out, value)));
        expect(bytes.size() == 8U);
        expect(bytes[0] == std::byte{8});
        expect(bytes[1] == std::byte{0});
        expect(bytes[2] == std::byte{0});
        expect(bytes[3] == std::byte{0});
        expect(bytes[4] == std::byte{3});
        expect(bytes[5] == std::byte{'a'});
        expect(bytes[7] == std::byte{'c'});
    };

    "text component round trips"_test = [] {
        const net::text_component value{.text = "Hello, Refrain!"};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_text_component(out, value)));
        net::reader in{bytes};
        net::text_component decoded{};
        expect(static_cast<bool>(net::decode_text_component(in, decoded)));
        expect(decoded == value);
        expect(in.empty());
    };

    "text component rejects a compound tag"_test = [] {
        const std::array bytes{std::byte{10}, std::byte{0}, std::byte{0}};
        net::reader in{bytes};
        net::text_component decoded{};
        const auto status{net::decode_text_component(in, decoded)};
        expect(!status);
        expect(status.error().code == refrain::error_code::unsupported);
    };

    "text component rejects nul and supplementary characters"_test = [] {
        for (const std::string_view text :
             {std::string_view{"a\0b", 3},
              std::string_view{"\xF0\x9F\x98\x80"}}) {
            std::vector<std::byte> bytes;
            net::writer out{bytes};
            const auto status{net::encode_text_component(
                out, net::text_component{std::string{text}})};
            expect(!status);
            expect(status.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "nbt end tag round trips as an empty value"_test = [] {
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_nbt(out, net::nbt_blob{})));
        const bool end_tag{bytes == std::vector<std::byte>{std::byte{0}}};
        expect(end_tag);
        net::reader in{bytes};
        net::nbt_blob decoded{};
        expect(static_cast<bool>(net::decode_nbt(in, decoded)));
        expect(decoded.bytes.empty());
        expect(in.empty());
    };

    "nbt compound round trips byte for byte"_test = [] {
        // TAG_Compound with an empty name holding one TAG_Int named "value".
        std::vector<std::byte> tag;
        const auto push = [&tag](std::initializer_list<int> values) {
            for (const int value : values) {
                tag.push_back(static_cast<std::byte>(value));
            }
        };
        push({10, 0, 0, 3, 0, 5});
        for (const char character : std::string_view{"value"}) {
            tag.push_back(static_cast<std::byte>(character));
        }
        push({0, 0, 0, 42, 0});
        const net::nbt_blob value{.bytes = tag};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_nbt(out, value)));
        net::reader in{bytes};
        net::nbt_blob decoded{};
        expect(static_cast<bool>(net::decode_nbt(in, decoded)));
        const bool same{decoded.bytes == tag};
        expect(same);
        expect(in.empty());
    };

    "nbt rejects a truncated tag"_test = [] {
        const std::array bytes{std::byte{10}, std::byte{0}, std::byte{0},
                               std::byte{3}};
        net::reader in{bytes};
        net::nbt_blob decoded{};
        const auto status{net::decode_nbt(in, decoded)};
        expect(!status);
        expect(status.error().code == refrain::error_code::parse_error);
    };

    "lp vec3 encodes the zero vector as one byte"_test = [] {
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_lp_vec3(out, net::lp_vec3{})));
        expect(bytes.size() == 1U);
        if (bytes.size() == 1U) {
            expect(bytes[0] == std::byte{0});
        }
        net::reader in{bytes};
        net::lp_vec3 decoded{.x = 1.0};
        expect(static_cast<bool>(net::decode_lp_vec3(in, decoded)));
        expect(decoded == net::lp_vec3{});
        expect(in.empty());
    };

    "lp vec3 round trips within its quantization step"_test = [] {
        const net::lp_vec3 value{.x = 0.5, .y = -0.25, .z = 0.125};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_lp_vec3(out, value)));
        net::reader in{bytes};
        net::lp_vec3 decoded{};
        expect(static_cast<bool>(net::decode_lp_vec3(in, decoded)));
        expect(in.empty());
        const auto close = [](double a, double b) {
            return std::abs(a - b) <= 0.01;
        };
        expect(close(decoded.x, value.x));
        expect(close(decoded.y, value.y));
        expect(close(decoded.z, value.z));
    };

    "lp vec3 rejects truncated input"_test = [] {
        const std::array<std::byte, 2> bytes{std::byte{1}, std::byte{2}};
        net::reader in{bytes};
        net::lp_vec3 decoded{};
        const auto status{net::decode_lp_vec3(in, decoded)};
        expect(!status);
        expect(status.error().code == refrain::error_code::parse_error);
    };

    "empty item stack round trips"_test = [] {
        const net::item_stack value{};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_item_stack(out, value)));
        expect(bytes.size() == 1U);
        expect(bytes[0] == std::byte{0});
        net::reader in{bytes};
        net::item_stack decoded{.count = 7};
        expect(static_cast<bool>(net::decode_item_stack(in, decoded)));
        expect(decoded == value);
        expect(in.empty());
    };

    "non-empty item stacks are rejected"_test = [] {
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        const auto status{
            net::encode_item_stack(out, net::item_stack{.count = 1})};
        expect(!status);
        expect(status.error().code == refrain::error_code::unsupported);

        const std::array encoded{std::byte{1}};
        net::reader in{encoded};
        net::item_stack decoded{};
        const auto decode{net::decode_item_stack(in, decoded)};
        expect(!decode);
        expect(decode.error().code == refrain::error_code::unsupported);
    };

    "text component rejects invalid utf8"_test = [] {
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        const auto status{net::encode_text_component(
            out, net::text_component{std::string{"\xC0\x80", 2}})};
        expect(!status);
        expect(status.error().code == refrain::error_code::invalid_argument);
    };
};

} // namespace
