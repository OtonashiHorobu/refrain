// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Contract C ABI / C++ convenience header tests. This file intentionally
// includes only `refrain/contracts/abi.hpp` (plus Boost.UT and the standard
// headers it itself uses) to keep the "include what you use" property of the
// hand-maintained header under test.

#include <refrain/contracts/abi.hpp>

#include <boost/ut.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace {

using namespace boost::ut;

namespace contracts = refrain::contracts;

// Tags are empty incomplete-ish marker types, exactly like the ones declared by
// the generated module headers.
struct widget_tag;
struct gadget_tag;
struct uninitialized_tag;

struct sample_abi_struct {
    std::uint32_t struct_size{0};
    std::uint32_t flags{0};
    refrain_handle handle{REFRAIN_HANDLE_INVALID};
};

} // namespace

namespace {

const suite abi_tests = [] {
    "contracts/handle compares by value"_test = [] {
        using handle = contracts::handle<widget_tag>;

        static_assert(std::is_standard_layout_v<handle>);
        static_assert(std::is_trivially_copyable_v<handle>);
        static_assert(std::is_default_constructible_v<handle>);
        static_assert(!std::is_convertible_v<std::uint64_t, handle>);

        const handle invalid{};
        expect(!invalid.valid());
        expect(invalid.raw() == REFRAIN_HANDLE_INVALID);

        constexpr handle first{handle{std::uint64_t{7}}};
        constexpr handle second{handle{std::uint64_t{7}}};
        constexpr handle other{handle{std::uint64_t{9}}};

        // A defaulted operator<=> does not synthesize operator==; the header
        // explicitly defaults it so handles are equality-comparable.
        expect(first == second);
        expect(first != other);
        expect(first < other);
        expect(other > first);
        expect(first <= second);
        expect(other >= first);
        static_assert((first <=> second) == std::strong_ordering::equal);
        expect(first.valid());
        expect(other.valid());
    };

    "contracts/handle tags are distinct types despite equal values"_test = [] {
        const contracts::handle<widget_tag> widget{std::uint64_t{42}};
        const contracts::handle<gadget_tag> gadget{std::uint64_t{42}};
        // Same raw value, different tag: the strong typing lives in the type
        // system (the two are not even comparable with each other).
        expect(widget.raw() == gadget.raw());
        static_assert(
            !std::equality_comparable_with<contracts::handle<widget_tag>,
                                           contracts::handle<gadget_tag>>);
    };

    "contracts/borrowed spans roundtrip"_test = [] {
        const std::string_view text{"refrain"};
        const auto borrowed{contracts::to_borrowed(text)};
        expect(borrowed.data != nullptr);
        expect(borrowed.len == text.size());
        expect(contracts::as_string_view(borrowed) == text);

        const refrain_str empty{};
        expect(contracts::as_string_view(empty).empty());

        const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
        const auto borrowed_bytes{contracts::to_borrowed(bytes)};
        expect(contracts::as_bytes(borrowed_bytes).size() == text.size());

        const refrain_bytes no_bytes{};
        expect(contracts::as_bytes(no_bytes).empty());
    };

    "contracts/struct_size helpers fix up generated structs"_test = [] {
        static_assert(contracts::sized_abi_struct<sample_abi_struct>);
        static_assert(!contracts::sized_abi_struct<int>);

        sample_abi_struct zero{};
        expect(zero.struct_size == 0U);
        contracts::init_struct(zero);
        expect(zero.struct_size == sizeof(sample_abi_struct));

        // A caller that skipped init_struct() still cannot send struct_size ==
        // 0 across the ABI: the wrapper helper repairs the copy.
        const sample_abi_struct repaired{
            contracts::with_struct_size(sample_abi_struct{})};
        expect(repaired.struct_size == sizeof(sample_abi_struct));
        expect(repaired.flags == 0U);
    };

    "contracts/version and metadata constants agree with the C header"_test =
        [] {
            expect(contracts::abi_major == REFRAIN_ABI_MAJOR);
            expect(contracts::abi_minor == REFRAIN_ABI_MINOR);
            expect(contracts::status_ok == REFRAIN_STATUS_OK);
            expect(static_cast<std::uint32_t>(contracts::side_kind::both) ==
                   REFRAIN_SIDE_BOTH);
            expect(static_cast<std::uint32_t>(contracts::thread_kind::render) ==
                   REFRAIN_THREAD_RENDER);
        };
};

} // namespace
