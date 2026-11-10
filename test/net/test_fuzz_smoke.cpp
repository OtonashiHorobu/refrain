// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/frame.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/schema/protocol/777/configuration_clientbound.hpp>
#include <refrain/schema/protocol/777/login_clientbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>
#include <refrain/schema/protocol/777/play_clientbound.hpp>
#include <refrain/schema/protocol/777/play_serverbound.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace login_cb = refrain::protocol::v777::login::clientbound;
namespace login_sb = refrain::protocol::v777::login::serverbound;
namespace config_cb = refrain::protocol::v777::configuration::clientbound;
namespace play_cb = refrain::protocol::v777::play::clientbound;
namespace play_sb = refrain::protocol::v777::play::serverbound;

[[nodiscard]] auto random_bytes(std::mt19937_64 &engine, std::size_t limit)
    -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> length{0, limit};
    std::vector<std::byte> bytes(length(engine));
    std::uniform_int_distribution<unsigned int> value{0, 255};
    for (auto &byte : bytes) {
        byte = static_cast<std::byte>(value(engine));
    }
    return bytes;
}

template <typename Packet, typename Decoder>
void try_decode(Decoder decode, std::span<const std::byte> bytes) {
    net::reader in{bytes};
    Packet packet{};
    const auto status{decode(in, packet)};
    static_cast<void>(status);
}

suite fuzz_smoke_tests = [] {
    "net/frame decoder survives random input"_test = [] {
        std::mt19937_64 engine{0x5EED1234ULL};
        for (int iteration{0}; iteration < 500; ++iteration) {
            net::frame_decoder decoder{iteration % 2 == 0 ? -1 : 256};
            const auto bytes{random_bytes(engine, 128)};
            static_cast<void>(decoder.push(bytes));
            std::vector<std::byte> body;
            for (int packets{0}; packets < 8; ++packets) {
                auto next{decoder.next(body)};
                if (!next || !*next) {
                    break;
                }
            }
        }
        expect(true);
    };

    "net/text component decoder survives random input"_test = [] {
        std::mt19937_64 engine{0xABCDEF01ULL};
        for (int iteration{0}; iteration < 1000; ++iteration) {
            const auto bytes{random_bytes(engine, 64)};
            net::reader in{bytes};
            net::text_component component{};
            const auto status{net::decode_text_component(in, component)};
            static_cast<void>(status);
        }
        expect(true);
    };

    "net/generated packet decoders survive random input"_test = [] {
        std::mt19937_64 engine{0x13572468ULL};
        for (int iteration{0}; iteration < 300; ++iteration) {
            const auto bytes{random_bytes(engine, 96)};
            try_decode<login_cb::hello>(login_cb::decode_hello, bytes);
            try_decode<login_cb::login_finished>(
                login_cb::decode_login_finished, bytes);
            try_decode<login_sb::hello>(login_sb::decode_hello, bytes);
            try_decode<config_cb::custom_payload>(
                config_cb::decode_custom_payload, bytes);
            try_decode<play_cb::login>(play_cb::decode_login, bytes);
            try_decode<play_cb::system_chat>(play_cb::decode_system_chat,
                                             bytes);
            try_decode<play_sb::chat_command>(play_sb::decode_chat_command,
                                              bytes);
            try_decode<net::item_stack>(net::decode_item_stack, bytes);
        }
        expect(true);
    };
};

} // namespace
