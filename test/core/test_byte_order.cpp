// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/byte_order.hpp>

#include <boost/ut.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace {

using namespace boost::ut;

namespace byte_order = refrain::core::byte_order;

/// The wire image every predicate below decodes: the bytes of
/// 0x0123456789ABCDEF in big-endian order, which is also the order the packet
/// and NBT fields use.
constexpr std::array<std::byte, 8> wire{
    std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
    std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};

suite<"core/byte-order"> byte_order_tests = [] {
    "load_be reads the wire order at every width"_test = [] {
        expect(byte_order::load_be16(wire) == 0x0123U);
        expect(byte_order::load_be32(wire) == 0x01234567U);
        expect(byte_order::load_be64(wire) == 0x0123456789ABCDEFULL);
        expect(byte_order::load_be16(wire, 2) == 0x4567U);
        expect(byte_order::load_be32(wire, 4) == 0x89ABCDEFU);
    };

    "load_le reads the little-endian order at every width"_test = [] {
        expect(byte_order::load_le16(wire) == 0x2301U);
        expect(byte_order::load_le32(wire) == 0x67452301U);
        expect(byte_order::load_le64(wire) == 0xEFCDAB8967452301ULL);
        expect(byte_order::load_le16(wire, 2) == 0x6745U);
        expect(byte_order::load_le32(wire, 4) == 0xEFCDAB89U);
    };

    "store_be writes the wire order at every width"_test = [] {
        std::array<std::byte, 8> out{};
        byte_order::store_be16(out, 0, 0x0123U);
        expect(out[0] == std::byte{0x01});
        expect(out[1] == std::byte{0x23});

        byte_order::store_be32(out, 1, 0x01234567U);
        expect(out[1] == std::byte{0x01});
        expect(out[2] == std::byte{0x23});
        expect(out[3] == std::byte{0x45});
        expect(out[4] == std::byte{0x67});

        byte_order::store_be64(out, 0, 0x0123456789ABCDEFULL);
        expect(out == wire);
    };

    "store_le writes the little-endian order at every width"_test = [] {
        std::array<std::byte, 8> out{};
        byte_order::store_le16(out, 0, 0x0123U);
        expect(out[0] == std::byte{0x23});
        expect(out[1] == std::byte{0x01});

        byte_order::store_le32(out, 1, 0x01234567U);
        expect(out[1] == std::byte{0x67});
        expect(out[2] == std::byte{0x45});
        expect(out[3] == std::byte{0x23});
        expect(out[4] == std::byte{0x01});

        byte_order::store_le64(out, 0, 0x0123456789ABCDEFULL);
        expect(out[0] == std::byte{0xEF});
        expect(out[7] == std::byte{0x01});
        expect(byte_order::load_le64(out) == 0x0123456789ABCDEFULL);
    };

    "load and store round-trip the boundary values"_test = [] {
        constexpr std::array<std::uint16_t, 4> u16_values{0U, 0x00FFU, 0xFF00U,
                                                          0xFFFFU};
        constexpr std::array<std::uint32_t, 5> u32_values{
            0U, 0x000000FFU, 0xFF000000U, 0x01020304U, 0xFFFFFFFFU};
        constexpr std::array<std::uint64_t, 4> u64_values{
            0U, 0x00000000000000FFULL, 0x0102030405060708ULL,
            0xFFFFFFFFFFFFFFFFULL};

        for (const auto value : u16_values) {
            std::array<std::byte, 2> stored{};
            byte_order::store_be16(stored, 0, value);
            expect(byte_order::load_be16(stored) == value);
            byte_order::store_le16(stored, 0, value);
            expect(byte_order::load_le16(stored) == value);
        }
        for (const auto value : u32_values) {
            std::array<std::byte, 4> stored{};
            byte_order::store_be32(stored, 0, value);
            expect(byte_order::load_be32(stored) == value);
            byte_order::store_le32(stored, 0, value);
            expect(byte_order::load_le32(stored) == value);
        }
        for (const auto value : u64_values) {
            std::array<std::byte, 8> stored{};
            byte_order::store_be64(stored, 0, value);
            expect(byte_order::load_be64(stored) == value);
            byte_order::store_le64(stored, 0, value);
            expect(byte_order::load_le64(stored) == value);
        }
    };

    "byteswap reverses every supported width"_test = [] {
        expect(byte_order::byteswap<std::uint16_t>(0x1234U) == 0x3412U);
        expect(byte_order::byteswap<std::uint32_t>(0x01020304U) == 0x04030201U);
        expect(byte_order::byteswap<std::uint64_t>(0x0102030405060708ULL) ==
               0x0807060504030201ULL);
        expect(byte_order::byteswap<std::uint8_t>(0xA5U) == 0xA5U);
        expect(byte_order::byteswap<std::uint16_t>(0xFFFFU) == 0xFFFFU);
        expect(byte_order::byteswap<std::uint64_t>(0U) == 0U);
        // A signed input keeps its exact bits, including the sign bit.
        expect(byte_order::byteswap<std::int32_t>(-1) == -1);
        expect(std::bit_cast<std::uint32_t>(byte_order::byteswap<std::int32_t>(
                   0x01020304)) == 0x04030201U);
        // The wrapper is its own inverse at every width.
        for (const auto value : {std::uint16_t{0x1234}, std::uint16_t{0}}) {
            expect(byte_order::byteswap(byte_order::byteswap(value)) == value);
        }
        constexpr std::uint64_t wide{0x0123456789ABCDEFULL};
        expect(byte_order::byteswap(byte_order::byteswap(wide)) == wide);
    };

    "negative values round-trip through the unsigned bits"_test = [] {
        std::array<std::byte, 8> stored{};
        byte_order::store_be(stored, 0, std::int32_t{-2});
        expect(stored[0] == std::byte{0xFF});
        expect(stored[3] == std::byte{0xFE});
        expect(byte_order::load_be<std::int32_t>(stored) == -2);

        byte_order::store_le(stored, 0, std::int64_t{-2});
        expect(stored[0] == std::byte{0xFE});
        expect(stored[7] == std::byte{0xFF});
        expect(byte_order::load_le<std::int64_t>(stored) == -2);

        // All-ones decodes to -1 in the signed type and to the unsigned
        // maximum in the unsigned one.
        constexpr std::array<std::byte, 4> minus_one{
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        expect(byte_order::load_be<std::int32_t>(minus_one) == -1);
        expect(byte_order::load_be32(minus_one) == 0xFFFFFFFFU);
    };

    "loads work at an unaligned offset"_test = [] {
        constexpr std::array<std::byte, 9> buffer{
            std::byte{0xAA}, std::byte{0x01}, std::byte{0x23},
            std::byte{0x45}, std::byte{0x67}, std::byte{0x89},
            std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};
        expect(byte_order::load_be32(buffer, 1) == 0x01234567U);
        expect(byte_order::load_be64(buffer, 1) == 0x0123456789ABCDEFULL);

        std::array<std::byte, 9> out{};
        byte_order::store_be64(out, 1, 0x0123456789ABCDEFULL);
        expect(out[0] == std::byte{0x00});
        expect(out[1] == std::byte{0x01});
        expect(out[8] == std::byte{0xEF});
    };

    "as_bytes views object storage in host order"_test = [] {
        std::uint32_t word{0x01020304U};
        const auto view{std::as_bytes(std::span{&word, 1})};
        expect(view.size() == sizeof(word));
        if constexpr (std::endian::native == std::endian::little) {
            expect(view[0] == std::byte{0x04});
            expect(byte_order::load_le32(view) == word);
        } else {
            expect(view[0] == std::byte{0x01});
            expect(byte_order::load_be32(view) == word);
        }
    };

    "as_writable_bytes decodes into object storage"_test = [] {
        std::array<std::uint16_t, 2> values{0U, 0U};
        auto view{std::as_writable_bytes(std::span{values})};
        expect(view.size() == sizeof(values));
        byte_order::store_be16(view, 0, 0x1234U);
        byte_order::store_be16(view, 2, 0xABCDU);
        expect(byte_order::load_be16(std::as_bytes(std::span{values}), 0) ==
               0x1234U);
        expect(byte_order::load_be16(std::as_bytes(std::span{values}), 2) ==
               0xABCDU);
    };
};

constexpr std::array<std::byte, 2> constexpr_wire16{std::byte{0x12},
                                                    std::byte{0x34}};
static_assert(byte_order::load_be16(constexpr_wire16) == 0x1234U);
static_assert(byte_order::load_le16(constexpr_wire16) == 0x3412U);
static_assert(byte_order::byteswap<std::uint32_t>(0x01020304U) == 0x04030201U);

} // namespace
