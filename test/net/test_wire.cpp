// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/wire.hpp>

#include <boost/ut.hpp>

#include <array>
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
