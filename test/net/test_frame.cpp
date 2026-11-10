// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/frame.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

[[nodiscard]] auto packet_of(std::size_t size, std::uint8_t seed)
    -> std::vector<std::byte> {
    std::vector<std::byte> body(size);
    for (std::size_t index{0}; index < size; ++index) {
        body[index] = static_cast<std::byte>((seed + index) & 0xFFU);
    }
    return body;
}

[[nodiscard]] auto frame_of(const net::frame_encoder &encoder,
                            const std::vector<std::byte> &body)
    -> std::vector<std::byte> {
    std::vector<std::byte> framed;
    const auto status{encoder.encode(body, framed)};
    expect(status.has_value());
    return framed;
}

[[nodiscard]] auto decode_all(net::frame_decoder &decoder,
                              const std::vector<std::byte> &bytes)
    -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> frames;
    expect(decoder.push(bytes).has_value());
    for (;;) {
        std::vector<std::byte> body;
        const auto next{decoder.next(body)};
        if (!next) {
            break;
        }
        if (!*next) {
            break;
        }
        frames.push_back(std::move(body));
    }
    return frames;
}

const suite tests = [] {
    "net/frame round trips without compression"_test = [] {
        const net::frame_encoder encoder{-1};
        const auto body{packet_of(300, 7)};
        const auto framed{frame_of(encoder, body)};

        net::frame_decoder decoder{-1};
        const auto frames{decode_all(decoder, framed)};
        expect(frames.size() == 1U);
        if (!frames.empty()) {
            expect(frames.front() == body);
        }
        expect(decoder.buffered() == 0U);
    };

    "net/frame round trips above and below the compression threshold"_test =
        [] {
            const net::frame_encoder encoder{64};
            const auto small{packet_of(16, 1)};
            const auto large{packet_of(4096, 2)};

            std::vector<std::byte> framed;
            expect(encoder.encode(small, framed).has_value());
            expect(encoder.encode(large, framed).has_value());

            net::frame_decoder decoder{64};
            const auto frames{decode_all(decoder, framed)};
            expect(frames.size() == 2U);
            if (frames.size() == 2U) {
                expect(frames[0] == small);
                expect(frames[1] == large);
            }
        };

    "net/frame decoder accepts input split at arbitrary boundaries"_test = [] {
        const net::frame_encoder encoder{32};
        const auto body{packet_of(1000, 9)};
        const auto framed{frame_of(encoder, body)};

        net::frame_decoder decoder{32};
        std::vector<std::vector<std::byte>> frames;
        for (const auto byte : framed) {
            expect(decoder.push(std::span{&byte, 1}).has_value());
            std::vector<std::byte> decoded;
            auto next{decoder.next(decoded)};
            if (next && *next) {
                frames.push_back(std::move(decoded));
            }
        }
        expect(frames.size() == 1U);
        if (!frames.empty()) {
            expect(frames.front() == body);
        }
    };

    "net/frame decoder reports a zero-length frame"_test = [] {
        const std::vector<std::byte> zero{static_cast<std::byte>(0)};
        net::frame_decoder decoder{-1};
        expect(decoder.push(zero).has_value());
        std::vector<std::byte> body;
        expect(!decoder.next(body).has_value());
        expect(decoder.failed());
    };

    "net/frame decoder rejects a prefix wider than 21 bits"_test = [] {
        const std::vector<std::byte> prefix{static_cast<std::byte>(0x80),
                                            static_cast<std::byte>(0x80),
                                            static_cast<std::byte>(0x80)};
        net::frame_decoder decoder{-1};
        expect(decoder.push(prefix).has_value());
        std::vector<std::byte> body;
        expect(!decoder.next(body).has_value());
        expect(decoder.failed());
    };

    "net/frame decoder rejects a compressed body below the threshold"_test =
        [] {
            // Build: outer length 4, inner length 1, one zlib byte.
            std::vector<std::byte> framed{};
            net::writer writer{framed};
            writer.write_varint(2);
            writer.write_varint(1);
            writer.write_u8(0x78);
            net::frame_decoder decoder{16};
            expect(decoder.push(framed).has_value());
            std::vector<std::byte> body;
            expect(!decoder.next(body).has_value());
        };

    "net/frame decoder rejects a body-length mismatch"_test = [] {
        const net::frame_encoder encoder{0};
        const auto body{packet_of(128, 3)};
        auto framed{frame_of(encoder, body)};
        // Change the inner declared length without recompressing.
        net::reader reader{framed};
        const auto outer{reader.read_varint()};
        expect(outer.has_value());
        const auto inner_offset{reader.position()};
        expect(inner_offset == 2U);
        framed[inner_offset] = static_cast<std::byte>(127);
        net::frame_decoder decoder{0};
        expect(decoder.push(framed).has_value());
        std::vector<std::byte> decoded;
        expect(!decoder.next(decoded).has_value());
    };

    "net/frame encoder rejects a body above the frame limit"_test = [] {
        net::frame_encoder encoder{-1};
        const auto body{packet_of(net::max_frame_length + 1, 0)};
        std::vector<std::byte> out;
        expect(!encoder.encode(body, out).has_value());
    };
};

} // namespace
