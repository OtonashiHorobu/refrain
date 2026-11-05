// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Reflection codegen specialisation: the schema codegen emits one
// `<refrain/reflection.hpp>` carrying `custom_traits<T>` specialisations for
// every generated RSC/RIDL struct (type name + fields in declaration order).
// This TU includes only that header (it must bring in every aggregate it
// describes) and proves the metadata works without any REFR_REFLECT macro in
// the module headers and without the backend being a dependency of normal
// generated headers.

#if defined(REFRAIN_USE_REFLECTION) && REFRAIN_USE_REFLECTION

#include <refrain/reflection.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

using namespace boost::ut;

namespace reflect = refrain::reflect;

// RIDL service struct (C ABI struct + generated C++ alias). `struct_size` is
// ABI plumbing, not a schema field, so it must not appear in the metadata.
using ridl_struct = refrain::services::storage::chunk_key;
// RSC storage record from schema/storage/chunk.rsc (field order is the storage
// ABI; the metadata must preserve declaration order).
using chunk_record = refrain::storage::chunk;

static_assert(reflect::reflected<ridl_struct>);
static_assert(reflect::field_count<ridl_struct> == 2U);
static_assert(reflect::field_name<ridl_struct, 0>() == "pos");
static_assert(reflect::field_name<ridl_struct, 1>() == "format_version");
static_assert(reflect::name<ridl_struct> ==
              "refrain::services::storage::chunk_key");

static_assert(reflect::reflected<chunk_record>);
static_assert(reflect::field_count<chunk_record> == 16U);
static_assert(reflect::field_name<chunk_record, 0>() == "format_version");
static_assert(reflect::field_name<chunk_record, 1>() == "status");
static_assert(reflect::field_name<chunk_record, 15>() == "light_sections");
static_assert(reflect::name<chunk_record> == "refrain::storage::chunk");

// Member pointers keep the exact declared type, including container fields: a
// customized walk can therefore bind mutable references to them.
static_assert(
    std::is_same_v<
        std::remove_cvref_t<
            decltype(std::get<4>(reflect::custom_traits<chunk_record>::fields)
                         .pointer)>,
        std::vector<refrain::storage::section_record>
            refrain::storage::chunk::*>);
static_assert(
    std::is_same_v<
        std::remove_cvref_t<
            decltype(std::get<2>(reflect::custom_traits<chunk_record>::fields)
                         .pointer)>,
        std::int64_t refrain::storage::chunk::*>);

const suite reflection_generated_tests = [] {
    "schema/generated reflection reports the qualified type name"_test = [] {
        expect(reflect::name<ridl_struct> ==
               "refrain::services::storage::chunk_key");
        expect(reflect::name<ridl_struct>.find("chunk_key") !=
               std::string_view::npos);
        expect(reflect::name<chunk_record>.find("chunk") !=
               std::string_view::npos);
        expect(reflect::field_count<chunk_record> == std::size_t{16});
    };

    "schema/generated reflection walks fields in declaration order"_test = [] {
        const std::array<std::string_view, 3> expected{"y", "block_light",
                                                       "sky_light"};
        auto light = refrain::storage::light_section{};
        light.y = 4;
        light.block_light = {std::byte{0x11}};
        light.sky_light = {std::byte{0x22}, std::byte{0x33}};

        std::array<std::string_view, 3> visited{};
        std::size_t index{0};
        reflect::for_each_field(
            light, [&](std::string_view field, auto &value) {
                expect(index < visited.size());
                if (index < visited.size()) {
                    visited[index] = field;
                }
                ++index;
                using value_type = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<value_type, std::int8_t>) {
                    value = 9; // mutable object -> mutable reference
                }
            });
        expect(index == expected.size());
        expect(visited == expected);
        expect(light.y == 9);
        expect(light.block_light.size() == std::size_t{1});
        expect(light.sky_light.size() == std::size_t{2});
    };

    "schema/generated reflection yields const references on const objects"_test =
        [] {
            auto light = refrain::storage::light_section{};
            light.y = 4;
            light.block_light = {std::byte{0x11}};
            const auto &const_light = light;

            std::size_t visited{0};
            reflect::for_each_field(const_light, [&](std::string_view field,
                                                     const auto &value) {
                static_assert(
                    std::is_const_v<std::remove_reference_t<decltype(value)>>);
                using value_type = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<value_type, std::int8_t>) {
                    expect(field == "y");
                    expect(value == 4);
                } else {
                    static_assert(
                        std::is_same_v<value_type, std::vector<std::byte>>);
                    if (field == "block_light") {
                        expect(value.size() == std::size_t{1});
                    } else {
                        expect(field == "sky_light");
                        expect(value.empty());
                    }
                }
                ++visited;
            });
            expect(visited == std::size_t{3});
        };

    "schema/generated custom traits work on mutable and const objects"_test =
        [] {
            using traits = reflect::custom_traits<refrain::storage::saved_tick>;
            static_assert(std::tuple_size_v<
                              std::remove_cvref_t<decltype(traits::fields)>> ==
                          5U);

            auto tick = refrain::storage::saved_tick{};
            auto &id = tick.*(std::get<0>(traits::fields).pointer);
            id = "minecraft:stone";
            tick.*(std::get<4>(traits::fields).pointer) = 20;

            const auto &const_tick = tick;
            expect(const_tick.*(std::get<0>(traits::fields).pointer) ==
                   "minecraft:stone");
            expect(const_tick.*(std::get<4>(traits::fields).pointer) == 20);
        };
};

} // namespace

#else // !REFRAIN_USE_REFLECTION

namespace {

// Keep the translation unit non-empty when the project is configured with
// -DREFRAIN_USE_REFLECTION=OFF (the generated header is a no-op then).
[[maybe_unused]] constexpr int reflection_generated_disabled = 0;

} // namespace

#endif // REFRAIN_USE_REFLECTION
