// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/crypto.hpp>
#include <refrain/net/frame.hpp>
#include <refrain/protocol/java777_codec.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace protocol = refrain::protocol;

[[nodiscard]] auto frame_body(std::uint32_t packet_id,
                              std::span<const std::byte> payload,
                              int threshold = -1) -> std::vector<std::byte> {
    std::vector<std::byte> body;
    net::writer writer{body};
    writer.write_varint(packet_id);
    writer.write_bytes(payload);
    net::frame_encoder encoder{threshold};
    std::vector<std::byte> framed;
    static_cast<void>(encoder.encode(body, framed));
    return framed;
}

suite java777_codec_tests = [] {
    "protocol/777 codec round trips an uncompressed packet"_test = [] {
        protocol::java777_codec codec{};
        codec.create_connection(1);
        const std::array payload{std::byte{0xAA}, std::byte{0xBB}};
        expect(static_cast<bool>(codec.send_packet(1, 0x10U, payload)));
        std::span<const std::byte> framed{};
        expect(codec.take_outbound(1, framed));
        net::frame_decoder decoder{-1};
        expect(static_cast<bool>(decoder.push(framed)));
        std::vector<std::byte> body;
        auto next{decoder.next(body)};
        expect(next.has_value() && *next);
        net::reader reader{body};
        auto packet_id{reader.read_varint()};
        expect(packet_id.has_value() && *packet_id == 0x10U);
        const auto decoded{reader.read_bytes(reader.remaining())};
        expect(decoded.has_value());
        if (decoded) {
            const bool same{std::equal(decoded->begin(), decoded->end(),
                                       payload.begin(), payload.end())};
            expect(same);
        }
    };

    "protocol/777 codec feeds and decodes a compressed packet"_test = [] {
        protocol::java777_codec codec{};
        codec.create_connection(2);
        expect(static_cast<bool>(codec.set_compression(2, 0)));
        const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};
        const auto framed{frame_body(0x20U, payload, 0)};
        expect(static_cast<bool>(codec.feed(2, framed)));
        std::uint32_t packet_id{0};
        std::span<const std::byte> decoded_payload{};
        auto next{codec.next_packet(2, packet_id, decoded_payload)};
        expect(next.has_value() && *next);
        expect(packet_id == 0x20U);
        expect(decoded_payload.size() == payload.size());
        if (decoded_payload.size() == payload.size()) {
            expect(decoded_payload[0] == payload[0]);
            expect(decoded_payload[2] == payload[2]);
        }
    };

    "protocol/777 codec encrypts both directions"_test = [] {
        std::array<std::byte, net::aes128_key_bytes> key{};
        for (std::size_t index{0}; index < key.size(); ++index) {
            key[index] = static_cast<std::byte>(index + 1U);
        }
        auto server{protocol::java777_codec{}};
        server.create_connection(3);
        expect(static_cast<bool>(server.set_cipher(3, key, key)));
        auto client{net::aes128_cfb8::create(key, key)};
        expect(client.has_value());
        if (!client) {
            return;
        }
        const std::array payload{std::byte{9}, std::byte{8}};
        expect(static_cast<bool>(server.send_packet(3, 0x30U, payload)));
        std::span<const std::byte> framed{};
        expect(server.take_outbound(3, framed));
        auto plain{client->decrypt(framed)};
        expect(plain.has_value());
        if (plain) {
            net::frame_decoder decoder{-1};
            expect(static_cast<bool>(decoder.push(*plain)));
            std::vector<std::byte> body;
            auto next{decoder.next(body)};
            expect(next.has_value() && *next);
            net::reader reader{body};
            auto packet_id{reader.read_varint()};
            expect(packet_id.has_value() && *packet_id == 0x30U);
        }

        const std::array inbound_payload{std::byte{7}, std::byte{6}};
        auto inbound{frame_body(0x31U, inbound_payload)};
        auto encrypted{client->encrypt(inbound)};
        expect(encrypted.has_value());
        if (encrypted) {
            expect(static_cast<bool>(server.feed(3, *encrypted)));
            std::uint32_t packet_id{0};
            std::span<const std::byte> decoded{};
            auto next{server.next_packet(3, packet_id, decoded)};
            expect(next.has_value() && *next);
            expect(packet_id == 0x31U);
            expect(decoded.size() == inbound_payload.size());
        }
    };

    "protocol/777 codec preserves outbound order and rejects unknown connections"_test =
        [] {
            protocol::java777_codec codec{};
            codec.create_connection(4);
            const std::array first{std::byte{1}};
            const std::array second{std::byte{2}};
            expect(static_cast<bool>(codec.send_packet(4, 1U, first)));
            expect(static_cast<bool>(codec.send_packet(4, 2U, second)));
            std::span<const std::byte> framed{};
            expect(codec.take_outbound(4, framed));
            net::frame_decoder decoder{-1};
            expect(static_cast<bool>(decoder.push(framed)));
            std::vector<std::byte> body;
            auto next{decoder.next(body)};
            expect(next.has_value() && *next);
            net::reader reader{body};
            auto packet_id{reader.read_varint()};
            expect(packet_id.has_value() && *packet_id == 1U);
            expect(codec.take_outbound(4, framed));
            expect(static_cast<bool>(decoder.push(framed)));
            body.clear();
            next = decoder.next(body);
            expect(next.has_value() && *next);
            net::reader second_reader{body};
            auto second_id{second_reader.read_varint()};
            expect(second_id.has_value() && *second_id == 2U);
            expect(!codec.take_outbound(4, framed));

            const std::array data{std::byte{0}};
            expect(!codec.feed(99, data));
            expect(!codec.set_compression(99, 0));
        };

    // Two connections encode at the same time. The first connection's encode is
    // parked inside the codec until the second connection's encode has
    // returned, so the assertion does not depend on wall-clock speed: a codec
    // that serialises every connection on one lock never returns the second
    // encode. The wait is bounded, so such a regression fails instead of
    // hanging.
    "protocol/777 codec encodes two connections concurrently"_test = [] {
        constexpr std::uint64_t first{0x11};
        constexpr std::uint64_t second{0x12};
        std::binary_semaphore parked{0};
        std::binary_semaphore release{0};
        protocol::java777_codec codec{[&](std::uint64_t connection) {
            if (connection != first) {
                return;
            }
            parked.release();
            release.acquire();
        }};
        codec.create_connection(first);
        codec.create_connection(second);
        const std::array payload{std::byte{0x5A}};

        bool first_sent{false};
        std::thread first_encode{[&] {
            first_sent =
                static_cast<bool>(codec.send_packet(first, 1U, payload));
        }};
        const bool first_parked{
            parked.try_acquire_for(std::chrono::seconds{5})};
        std::promise<bool> second_status{};
        auto second_done{second_status.get_future()};
        std::thread second_encode{[&] {
            second_status.set_value(
                static_cast<bool>(codec.send_packet(second, 2U, payload)));
        }};
        const bool second_completed{second_done.wait_for(std::chrono::seconds{
                                        5}) == std::future_status::ready};
        release.release();
        first_encode.join();
        second_encode.join();

        expect(first_parked);
        expect(first_sent);
        expect(second_completed);
        expect(second_done.get());
        const auto outbound_id{[&codec](std::uint64_t connection) {
            std::span<const std::byte> framed{};
            if (!codec.take_outbound(connection, framed)) {
                return std::uint32_t{0};
            }
            net::frame_decoder decoder{-1};
            if (!decoder.push(framed)) {
                return std::uint32_t{0};
            }
            std::vector<std::byte> body;
            if (!decoder.next(body) || body.empty()) {
                return std::uint32_t{0};
            }
            net::reader reader{body};
            const auto packet_id{reader.read_varint()};
            return packet_id ? *packet_id : std::uint32_t{0};
        }};
        expect(outbound_id(first) == 1U);
        expect(outbound_id(second) == 2U);
    };
};

} // namespace
