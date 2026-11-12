// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The varint size helpers and block position packing are the single wire
// definition shared by the codec, the frame layer and the server. These tests
// freeze the encoded size and the round trip over boundary values, and check
// that the frame prefix decoder agrees with `reader::read_varint`.

#include <refrain/net/frame.hpp>
#include <refrain/net/primitives.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

/// Boundary values that cover every encoded width of a 32-bit VarInt.
constexpr std::array<std::uint32_t, 8> varint_corpus{
    0U, 1U, 127U, 128U, 255U, 300U, 0x7FFFFFFFU, 0xFFFFFFFFU};

/// Boundary values that cover the wide end of a 64-bit VarLong.
constexpr std::array<std::uint64_t, 10> varlong_corpus{0ULL,
                                                       1ULL,
                                                       127ULL,
                                                       128ULL,
                                                       255ULL,
                                                       300ULL,
                                                       0x7FFFFFFFULL,
                                                       0xFFFFFFFFULL,
                                                       0x7FFFFFFFFFFFFFFFULL,
                                                       0xFFFFFFFFFFFFFFFFULL};

/// Frame lengths that fit the VarInt21 prefix; the boundary is inclusive.
constexpr std::array<std::uint32_t, 7> frame_corpus{
    1U,
    127U,
    128U,
    255U,
    300U,
    65'535U,
    static_cast<std::uint32_t>(net::max_frame_length)};

[[nodiscard]] auto encode_varint(std::uint32_t value)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    net::writer writer{out};
    writer.write_varint(value);
    return out;
}

[[nodiscard]] auto encode_varlong(std::uint64_t value)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    net::writer writer{out};
    writer.write_varlong(value);
    return out;
}

/// Byte equality outside boost.ut's range comparison, which would try to
/// stream `std::byte` elements when an assertion fails.
[[nodiscard]] auto same_bytes(std::span<const std::byte> lhs,
                              std::span<const std::byte> rhs) -> bool {
    return std::ranges::equal(lhs, rhs);
}

[[nodiscard]] auto body_of(std::size_t size) -> std::vector<std::byte> {
    std::vector<std::byte> body(size);
    for (std::size_t index{0}; index < size; ++index) {
        body[index] = static_cast<std::byte>((index * 31U + 7U) & 0xFFU);
    }
    return body;
}

const suite tests = [] {
    "net/varint size matches the encoded length over boundary values"_test =
        [] {
            for (const auto value : varint_corpus) {
                const auto encoded{encode_varint(value)};
                expect(net::varint_size(value) == encoded.size())
                    << "size mismatch for " << value;
                expect(encoded.size() >= 1U && encoded.size() <= 5U)
                    << "width out of range for " << value;

                net::reader in{encoded};
                auto decoded{in.read_varint()};
                expect(decoded.has_value()) << "decode failed for " << value;
                if (decoded) {
                    expect(*decoded == value) << "round trip for " << value;
                    expect(in.empty()) << "reader left bytes for " << value;
                }
            }
        };

    "net/varint size reports one byte per seven payload bits"_test = [] {
        expect(net::varint_size(0U) == 1U);
        expect(net::varint_size(127U) == 1U);
        expect(net::varint_size(128U) == 2U);
        expect(net::varint_size(16'383U) == 2U);
        expect(net::varint_size(16'384U) == 3U);
        expect(net::varint_size(2'097'151U) == 3U);
        expect(net::varint_size(2'097'152U) == 4U);
        expect(net::varint_size(268'435'455U) == 4U);
        expect(net::varint_size(268'435'456U) == 5U);
        expect(net::varint_size(0xFFFFFFFFU) == 5U);
    };

    "net/varlong size matches the encoded length over boundary values"_test =
        [] {
            for (const auto value : varlong_corpus) {
                const auto encoded{encode_varlong(value)};
                expect(net::varlong_size(value) == encoded.size())
                    << "size mismatch for " << value;
                expect(encoded.size() >= 1U && encoded.size() <= 10U)
                    << "width out of range for " << value;

                net::reader in{encoded};
                auto decoded{in.read_varlong()};
                expect(decoded.has_value()) << "decode failed for " << value;
                if (decoded) {
                    expect(*decoded == value) << "round trip for " << value;
                    expect(in.empty()) << "reader left bytes for " << value;
                }
            }
        };

    "net/varlong size reports one byte per seven payload bits"_test = [] {
        expect(net::varlong_size(0ULL) == 1U);
        expect(net::varlong_size(127ULL) == 1U);
        expect(net::varlong_size(128ULL) == 2U);
        expect(net::varlong_size(268'435'456ULL) == 5U);
        expect(net::varlong_size(0x7FFFFFFFFFFFFFFFULL) == 9U);
        expect(net::varlong_size(0x8000000000000000ULL) == 10U);
        expect(net::varlong_size(0xFFFFFFFFFFFFFFFFULL) == 10U);
    };

    "net/frame prefix decoder agrees with the varint reader"_test = [] {
        // Zero is a legal encoding but not a legal frame length, so it is
        // covered by the size and round-trip suites above only.
        for (const auto length : frame_corpus) {
            const auto body{body_of(length)};
            const auto prefix{encode_varint(length)};

            net::reader in{prefix};
            const auto decoded{in.read_varint()};
            expect(decoded.has_value())
                << "prefix decode failed for " << length;
            if (!decoded) {
                continue;
            }
            expect(*decoded == length);
            expect(in.position() == net::varint_size(length));

            std::vector<std::byte> framed{prefix};
            framed.insert(framed.end(), body.begin(), body.end());

            net::frame_decoder decoder{-1};
            expect(decoder.push(framed).has_value());
            std::vector<std::byte> packet;
            const auto next{decoder.next(packet)};
            expect(next.has_value() && *next)
                << "frame decode failed for " << length;
            if (next && *next) {
                expect(same_bytes(packet, body));
            }
        }
    };

    "net/frame encoder writes the varint prefix without compression"_test = [] {
        for (const auto length : frame_corpus) {
            const auto body{body_of(length)};
            std::vector<std::byte> framed;
            const net::frame_encoder encoder{-1};
            expect(encoder.encode(body, framed).has_value());

            std::vector<std::byte> expected{encode_varint(length)};
            expected.insert(expected.end(), body.begin(), body.end());
            expect(same_bytes(framed, expected));
        }
    };

    "net/frame decoder resumes a prefix split across pushes"_test = [] {
        // 300 encodes as two bytes, so the first push ends inside the prefix.
        const auto body{body_of(300)};
        const auto prefix{encode_varint(300)};
        expect(prefix.size() == 2U);

        net::frame_decoder decoder{-1};
        expect(decoder.push(std::span{prefix}.first(1)).has_value());
        std::vector<std::byte> packet;
        const auto waiting{decoder.next(packet)};
        expect(waiting.has_value() && !*waiting);

        std::vector<std::byte> rest(prefix.begin() + 1, prefix.end());
        rest.insert(rest.end(), body.begin(), body.end());
        expect(decoder.push(rest).has_value());
        const auto next{decoder.next(packet)};
        expect(next.has_value() && *next);
        if (next && *next) {
            expect(same_bytes(packet, body));
        }
    };

    "net/block position packs the vanilla wire layout"_test = [] {
        expect(net::pack_block_position(0, 0, 0) == 0);
        expect(net::pack_block_position(0, 64, 0) == 64);
        expect(net::pack_block_position(1, 2, 3) ==
               ((std::int64_t{1} << 38) | (std::int64_t{3} << 12) | 2));
        expect(net::pack_block_position(0, 0, 1) == (std::int64_t{1} << 12));

        const auto origin{
            net::unpack_block_position(net::pack_block_position(0, 64, 0))};
        expect(origin == net::block_position{.x = 0, .y = 64, .z = 0});

        const auto corner{net::unpack_block_position(
            net::pack_block_position(0x3FFFFFF, 0xFFF, 0x3FFFFFF))};
        expect(corner ==
               net::block_position{.x = 0x3FFFFFF, .y = 0xFFF, .z = 0x3FFFFFF});
    };
};

} // namespace
