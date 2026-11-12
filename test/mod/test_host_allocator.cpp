// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The host allocator table is the only allocator a mod may hand memory back
// through; these tests pin its contract.

#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

namespace ut = boost::ut;
namespace mod = refrain::mod;

ut::suite<"mod/host-allocator"> host_allocator_suite = [] {
    using namespace ut;

    "the table is populated"_test = [] {
        const auto table{mod::host_allocator()};
        expect(table.alloc != nullptr);
        expect(table.free != nullptr);
    };

    "an allocation round trips through the table"_test = [] {
        const auto table{mod::host_allocator()};
        constexpr std::size_t size{1024};
        auto *buffer{static_cast<std::byte *>(
            table.alloc(table.self, size, alignof(std::max_align_t)))};
        expect(buffer != nullptr);
        if (buffer == nullptr) {
            return;
        }
        std::memset(buffer, 0x5A, size);
        expect(static_cast<std::uint8_t>(buffer[0]) == 0x5A);
        expect(static_cast<std::uint8_t>(buffer[size - 1]) == 0x5A);
        table.free(table.self, buffer);
    };

    "the requested alignment is honoured"_test = [] {
        const auto table{mod::host_allocator()};
        constexpr std::size_t alignment{64};
        auto *buffer{
            static_cast<std::byte *>(table.alloc(table.self, 256, alignment))};
        expect(buffer != nullptr);
        if (buffer == nullptr) {
            return;
        }
        expect(reinterpret_cast<std::uintptr_t>(buffer) % alignment == 0);
        table.free(table.self, buffer);
    };

    "a non-power-of-two alignment is refused"_test = [] {
        const auto table{mod::host_allocator()};
        expect(table.alloc(table.self, 64, 3) == nullptr);
    };

    "a zero alignment falls back to the natural one"_test = [] {
        const auto table{mod::host_allocator()};
        auto *buffer{table.alloc(table.self, 16, 0)};
        expect(buffer != nullptr);
        table.free(table.self, buffer);
    };
};

} // namespace
