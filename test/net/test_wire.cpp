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

    "movement delta round trips linear and stepped forms"_test = [] {
        const net::vec_delta linear{.on_ground = true, .x = 1, .y = -2, .z = 3};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_vec_delta(out, linear)));
        expect(bytes.size() == 7U);
        net::reader in{bytes};
        net::vec_delta decoded{};
        expect(static_cast<bool>(net::decode_vec_delta(in, decoded)));
        expect(decoded.on_ground);
        expect(decoded.x == 1 && decoded.y == -2 && decoded.z == 3);
        expect(decoded.steps.empty());
        expect(in.empty());

        net::vec_delta stepped{};
        stepped.on_ground = false;
        stepped.steps = {{5U, 1, 2, 3}, {6U, -1, -2, -3}};
        bytes.clear();
        net::writer stepped_out{bytes};
        expect(static_cast<bool>(net::encode_vec_delta(stepped_out, stepped)));
        net::reader stepped_in{bytes};
        net::vec_delta stepped_decoded{};
        expect(static_cast<bool>(
            net::decode_vec_delta(stepped_in, stepped_decoded)));
        const bool same{stepped_decoded.steps == stepped.steps};
        expect(same);
        expect(stepped_in.empty());
    };

    "advancement screen selection round trips both actions"_test = [] {
        const auto tab{
            refrain::data::resource_location::parse("minecraft:story/root")};
        expect(tab.has_value());
        if (!tab) {
            return;
        }
        const net::advancement_seen opened{.action = 0U, .tab = *tab};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_advancement_seen(out, opened)));
        net::reader in{bytes};
        net::advancement_seen decoded{};
        expect(static_cast<bool>(net::decode_advancement_seen(in, decoded)));
        expect(decoded.action == 0U);
        expect(decoded.tab.to_string() == tab->to_string());
        expect(in.empty());

        const net::advancement_seen closed{.action = 1U};
        bytes.clear();
        net::writer closed_out{bytes};
        expect(static_cast<bool>(
            net::encode_advancement_seen(closed_out, closed)));
        expect(bytes.size() == 1U);
        net::reader closed_in{bytes};
        net::advancement_seen closed_decoded{.action = 0U};
        expect(static_cast<bool>(
            net::decode_advancement_seen(closed_in, closed_decoded)));
        expect(closed_decoded.action == 1U);
    };

    "stat entry round trips two varints"_test = [] {
        const net::stat_entry value{.type = 3U, .value = 42U};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_stat_entry(out, value)));
        expect(bytes.size() == 2U);
        net::reader in{bytes};
        net::stat_entry decoded{};
        expect(static_cast<bool>(net::decode_stat_entry(in, decoded)));
        const bool same{decoded == value};
        expect(same);
        expect(in.empty());
    };

    "position path round trips linear and stepped forms"_test = [] {
        const net::position_path linear{
            .type = 0U, .end = {1.5, -2.0, 3.25}, .steps = {}};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_position_path(out, linear)));
        net::reader in{bytes};
        net::position_path decoded{};
        expect(static_cast<bool>(net::decode_position_path(in, decoded)));
        expect(decoded.type == 0U);
        expect(decoded.end == linear.end);
        expect(in.empty());

        net::position_path stepped{};
        stepped.type = 1U;
        stepped.steps = {{1.0, 2.0, 3.0, 5U}, {4.0, 5.0, 6.0, 7U}};
        bytes.clear();
        net::writer stepped_out{bytes};
        expect(
            static_cast<bool>(net::encode_position_path(stepped_out, stepped)));
        net::reader stepped_in{bytes};
        net::position_path stepped_decoded{};
        expect(static_cast<bool>(
            net::decode_position_path(stepped_in, stepped_decoded)));
        const bool same_steps{stepped_decoded.steps == stepped.steps};
        expect(same_steps);
        expect(stepped_in.empty());

        net::position_path invalid{};
        invalid.type = 5U;
        std::vector<std::byte> sink;
        net::writer invalid_out{sink};
        const auto status{net::encode_position_path(invalid_out, invalid)};
        expect(!status);
        expect(status.error().code == refrain::error_code::invalid_argument);
    };

    "map patch round trips empty and present forms"_test = [] {
        net::map_patch empty{};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_map_patch(out, empty)));
        expect(bytes.size() == 1U);
        net::reader in{bytes};
        net::map_patch decoded{};
        expect(static_cast<bool>(net::decode_map_patch(in, decoded)));
        expect(decoded.width == 0U);
        expect(in.empty());

        net::map_patch patch{
            .width = 2U,
            .height = 3U,
            .start_x = 4U,
            .start_y = 5U,
            .colors = {std::byte{1}, std::byte{2}, std::byte{3}}};
        bytes.clear();
        net::writer patch_out{bytes};
        expect(static_cast<bool>(net::encode_map_patch(patch_out, patch)));
        net::reader patch_in{bytes};
        net::map_patch patch_decoded{};
        expect(
            static_cast<bool>(net::decode_map_patch(patch_in, patch_decoded)));
        const bool same_patch{patch_decoded == patch};
        expect(same_patch);
        expect(patch_in.empty());
    };

    "filter mask round trips both short and partial forms"_test = [] {
        for (const std::uint32_t type : {0U, 1U}) {
            net::filter_mask value{.type = type};
            std::vector<std::byte> bytes;
            net::writer out{bytes};
            expect(static_cast<bool>(net::encode_filter_mask(out, value)));
            expect(bytes.size() == 1U);
            net::reader in{bytes};
            net::filter_mask decoded{.type = 9U};
            expect(static_cast<bool>(net::decode_filter_mask(in, decoded)));
            expect(decoded.type == type);
            expect(decoded.mask.empty());
            expect(in.empty());
        }
        net::filter_mask partial{.type = 2U,
                                 .mask = {std::byte{0xAA}, std::byte{0x01}}};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_filter_mask(out, partial)));
        net::reader in{bytes};
        net::filter_mask decoded{};
        expect(static_cast<bool>(net::decode_filter_mask(in, decoded)));
        expect(decoded.type == 2U);
        const bool same{decoded.mask == partial.mask};
        expect(same);
        expect(in.empty());
    };

    "packed message signature round trips both forms"_test = [] {
        net::message_signature_packed id_form{};
        id_form.id = 5U;
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(
            net::encode_message_signature_packed(out, id_form)));
        net::reader in{bytes};
        net::message_signature_packed decoded{};
        expect(static_cast<bool>(
            net::decode_message_signature_packed(in, decoded)));
        expect(!decoded.full);
        expect(decoded.id == 5U);
        expect(in.empty());

        net::message_signature_packed full_form{};
        full_form.full = true;
        for (std::size_t index{0}; index < full_form.signature.size();
             ++index) {
            full_form.signature[index] = static_cast<std::byte>(index & 0xFFU);
        }
        bytes.clear();
        net::writer full_out{bytes};
        expect(static_cast<bool>(
            net::encode_message_signature_packed(full_out, full_form)));
        expect(bytes.size() == 257U);
        net::reader full_in{bytes};
        net::message_signature_packed full_decoded{};
        expect(static_cast<bool>(
            net::decode_message_signature_packed(full_in, full_decoded)));
        expect(full_decoded.full);
        const bool same{full_decoded.signature == full_form.signature};
        expect(same);
        expect(full_in.empty());
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
