// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Contract tests for the C ABI string/bytes/status bridge. Every adapter and
// provider converts through these helpers, so the null, empty and embedded-NUL
// cases and the error -> status lookup are pinned here once instead of in one
// test per module.

#include <refrain/host/abi_bridge.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace abi = refrain::host::abi;

constexpr abi::status_entry test_statuses[]{
    {refrain::error_code::not_found, 21U},
    {refrain::error_code::io_error, 22U},
};

constexpr refrain_status test_fallback{23U};

static_assert(noexcept(abi::to_view(refrain_str{})));
static_assert(noexcept(abi::to_span(refrain_bytes{})));
static_assert(noexcept(abi::to_bytes(std::span<const std::uint8_t>{})));
static_assert(noexcept(abi::to_borrowed(std::string_view{})));
static_assert(noexcept(abi::to_status(refrain::error_code::not_found,
                                      test_statuses, test_fallback)));

const suite host_abi_bridge_tests = [] {
    "host/abi a null string with zero length reads as the empty view"_test =
        [] {
            const refrain_str text{.data = nullptr, .len = 0};
            const auto view{abi::to_view(text)};
            expect(view.empty());
            expect(view.data() == nullptr);
            expect(abi::to_string(text).empty());
            expect(abi::to_borrowed(view).data == nullptr);
            expect(abi::to_borrowed(view).len == 0U);
        };

    "host/abi a zero-length string with a live pointer reads as empty"_test =
        [] {
            constexpr char byte{'x'};
            const refrain_str text{
                .data = reinterpret_cast<const std::uint8_t *>(&byte),
                .len = 0,
            };
            const auto view{abi::to_view(text)};
            expect(view.empty());
            expect(view.data() == nullptr);
            expect(abi::to_string(text).empty());
        };

    "host/abi a null string with a length reads as the empty view"_test = [] {
        const refrain_str text{.data = nullptr, .len = 5};
        expect(abi::to_view(text).empty());
        expect(abi::to_string(text).empty());
    };

    "host/abi an embedded NUL keeps the ABI length"_test = [] {
        constexpr std::string_view raw{"ab\0cd", 5};
        const refrain_str text{
            .data = reinterpret_cast<const std::uint8_t *>(raw.data()),
            .len = raw.size(),
        };
        const auto view{abi::to_view(text)};
        expect(view.size() == 5U);
        expect(view == raw);

        const auto copy{abi::to_string(text)};
        expect(copy.size() == 5U);
        expect(copy == std::string{raw});
        expect(copy[2] == '\0');

        const auto borrowed{abi::to_borrowed(raw)};
        expect(borrowed.len == 5U);
        expect(borrowed.data == text.data);
        expect(abi::to_view(borrowed) == raw);
    };

    "host/abi null and empty bytes read as the empty span"_test = [] {
        const auto null_span{
            abi::to_span(refrain_bytes{.data = nullptr, .len = 0})};
        expect(null_span.empty());
        expect(null_span.data() == nullptr);

        const auto null_with_length{
            abi::to_span(refrain_bytes{.data = nullptr, .len = 4})};
        expect(null_with_length.empty());
        expect(null_with_length.data() == nullptr);

        const auto empty{abi::to_span(refrain_bytes{})};
        expect(empty.empty());
        const auto round_trip{abi::to_bytes(empty)};
        expect(round_trip.data == nullptr);
        expect(round_trip.len == 0U);
    };

    "host/abi bytes keep the borrowed pointer and length"_test = [] {
        constexpr std::uint8_t raw[]{0x00, 0x7f, 0x80, 0xff};
        const auto bytes{abi::to_bytes(std::span<const std::uint8_t>{raw})};
        expect(bytes.data == raw);
        expect(bytes.len == 4U);

        const auto span{abi::to_span(bytes)};
        expect(span.size() == 4U);
        expect(span.data() == raw);
        expect(span[0] == 0x00);
        expect(span[3] == 0xff);
    };

    "host/abi maps known error codes and falls back for the rest"_test = [] {
        expect(abi::to_status(refrain::error_code::not_found, test_statuses,
                              test_fallback) == 21U);
        expect(abi::to_status(refrain::error_code::io_error, test_statuses,
                              test_fallback) == 22U);
        expect(abi::to_status(refrain::error_code::parse_error, test_statuses,
                              test_fallback) == 23U);
        expect(abi::to_status(refrain::error_code::success, test_statuses,
                              test_fallback) == 23U);

        // The error overload only reads the code; message and location must
        // not influence the contract status.
        const refrain::error failure{
            .code = refrain::error_code::io_error,
            .message = "disk full",
        };
        expect(abi::to_status(failure, test_statuses, test_fallback) == 22U);
    };
};

} // namespace
