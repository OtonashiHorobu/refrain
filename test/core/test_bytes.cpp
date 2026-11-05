// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/bytes.hpp>
#include <refrain/core/hex.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using namespace boost::ut;

using refrain::core::bytes::align_up;
using refrain::core::bytes::align_up_checked;
using refrain::core::bytes::read_u16;
using refrain::core::bytes::read_u32;
using refrain::core::bytes::read_u64;
using refrain::core::bytes::write_le;
using refrain::core::hex::hex_u64;

[[maybe_unused]] const suite bytes_tests = [] {
    "bytes/read_u16 at zero and non-zero offsets"_test = [] {
        const std::array<std::byte, 2> at_zero{std::byte{0xFF},
                                               std::byte{0xFF}};
        expect(read_u16(at_zero, 0) == 0xFFFFU);

        const std::array<std::byte, 4> at_offset{
            std::byte{0x00}, std::byte{0x34}, std::byte{0x12}, std::byte{0x00}};
        expect(read_u16(at_offset, 1) == 0x1234U);
    };

    "bytes/read_u32 at zero and non-zero offsets"_test = [] {
        const std::array<std::byte, 4> at_zero{
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        expect(read_u32(at_zero, 0) == 0xFFFFFFFFU);

        const std::array<std::byte, 8> at_offset{
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x78},
            std::byte{0x56}, std::byte{0x34}, std::byte{0x12}, std::byte{0x00}};
        expect(read_u32(at_offset, 3) == 0x12345678U);
    };

    "bytes/read_u64 at zero and non-zero offsets"_test = [] {
        const std::array<std::byte, 8> at_zero{
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        expect(read_u64(at_zero, 0) == 0xFFFFFFFFFFFFFFFFULL);

        const std::array<std::byte, 14> at_offset{
            std::byte{0x00}, std::byte{0x00}, std::byte{0xEF}, std::byte{0xCD},
            std::byte{0xAB}, std::byte{0x89}, std::byte{0x67}, std::byte{0x45},
            std::byte{0x23}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}};
        expect(read_u64(at_offset, 2) == 0x0123456789ABCDEFULL);
    };

    "bytes/write_le round-trips fixed-width values"_test = [] {
        std::array<std::byte, 18> storage{};

        write_le<std::uint16_t>(storage, 1, 0xABCDU);
        expect(storage[1] == std::byte{0xCD});
        expect(storage[2] == std::byte{0xAB});
        expect(read_u16(storage, 1) == 0xABCDU);

        write_le<std::uint32_t>(storage, 3, 0x01234567U);
        expect(storage[3] == std::byte{0x67});
        expect(read_u32(storage, 3) == 0x01234567U);

        write_le<std::uint64_t>(storage, 5, 0x0123456789ABCDEFULL);
        expect(storage[5] == std::byte{0xEF});
        expect(storage[12] == std::byte{0x01});
        expect(read_u64(storage, 5) == 0x0123456789ABCDEFULL);
    };

    "bytes/align_up rounds to the next multiple"_test = [] {
        struct align_case {
            std::uint64_t value;
            std::uint64_t alignment;
            std::uint64_t expected;
        };
        constexpr std::array cases{
            align_case{0U, 8U, 0U},    align_case{1U, 8U, 8U},
            align_case{63U, 8U, 64U},  align_case{64U, 8U, 64U},
            align_case{65U, 8U, 72U},  align_case{0U, 64U, 0U},
            align_case{1U, 64U, 64U},  align_case{63U, 64U, 64U},
            align_case{64U, 64U, 64U}, align_case{65U, 64U, 128U},
        };
        for (const auto &test_case : cases) {
            expect(align_up(test_case.value, test_case.alignment) ==
                   test_case.expected);
        }
        static_assert(align_up(65U, 8U) == 72U);
        static_assert(align_up(0U, 64U) == 0U);
    };

    // align_up() keeps its reporting-free signature for the existing call
    // sites: a precondition violation asserts in debug builds and returns 0 in
    // release builds. The checked variant is the one that reports it, so only
    // that one is exercised here (a debug test binary would abort otherwise).
    "bytes/align_up_checked rejects zero, non-powers of two and overflow"_test =
        [] {
            expect(!align_up_checked(0U, 0U).has_value());
            expect(!align_up_checked(16U, 0U).has_value());
            expect(!align_up_checked(16U, 3U).has_value());
            expect(!align_up_checked(16U, 6U).has_value());
            expect(!align_up_checked(16U, 9U).has_value());

            constexpr auto max{std::numeric_limits<std::uint64_t>::max()};
            // value + alignment - 1 would wrap at the top of the range.
            expect(!align_up_checked(max, 8U).has_value());
            expect(!align_up_checked(max - 3U, 8U).has_value());
            // The largest value that still rounds without overflowing.
            expect(align_up_checked(max - 7U, 8U).value_or(0U) == max - 7U);

            expect(align_up_checked(0U, 8U).value_or(1U) == 0U);
            expect(align_up_checked(1U, 8U).value_or(0U) == 8U);
            expect(align_up_checked(65U, 8U).value_or(0U) == 72U);
            expect(align_up_checked(1U, 1U).value_or(0U) == 1U);
        };

    "hex/hex_u64 formats sixteen lowercase digits"_test = [] {
        expect(hex_u64(0) == "0000000000000000");
        expect(hex_u64(0x0123456789ABCDEFULL) == "0123456789abcdef");
        expect(hex_u64(0xFFFFFFFFFFFFFFFFULL) == "ffffffffffffffff");
    };
};

} // namespace
