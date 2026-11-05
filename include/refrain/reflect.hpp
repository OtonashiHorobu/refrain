// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

// `refrain::reflect<T>` facade: the only public entry point for static
// reflection in the engine.
//
// Contracts:
//  - business code includes ONLY this header; refl-cpp / magic_enum headers
//    must not appear in broadly included public headers,
//  - reflection is internal ergonomics (logging, debug UI, tools, tests); it
//    must never define or drive wire/archive/mod ABI formats (schema
//    codegen remains the single source of truth),
//  - all metadata is `inline constexpr`: zero runtime cost, no lookup tables,
//  - the backend is replaceable behind this facade (refl-cpp today, C++26
//    `std::meta` or Boost.Describe as escape hatches later).
//
// Build: `REFRAIN_USE_REFLECTION=ON` links the backend packages to
// `refrain-kernel` (PUBLIC) and exposes them as SYSTEM includes. When the
// option is OFF this header is not available on purpose.

#include <cstddef>
#include <cstdint> // refl-cpp 0.12.4 uses intptr_t without including <cstdint>.
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <refl.hpp>

#include <refrain/reflect/annotations.hpp>

#if !defined(REFRAIN_USE_REFLECTION) || !REFRAIN_USE_REFLECTION
#error                                                                         \
    "refrain/reflect.hpp requires REFRAIN_USE_REFLECTION=ON (CMake: -DREFRAIN_USE_REFLECTION=ON; links refl-cpp/magic_enum to refrain-kernel)"
#endif

// ---------------------------------------------------------------------------
// Annotation macros (thin aliases so business code never spells refl-cpp
// macros). `REFR_REFLECT` mirrors `REFL_AUTO`, `REFR_FIELD` mirrors `field`.
//
//   struct player_stats { ... };
//   REFR_REFLECT(type(player_stats), REFR_FIELD(health), REFR_FIELD(xp))
//
// `REFR_ENUM` is a no-op placeholder: magic_enum needs no annotation, but the
// macro keeps enum sites source-compatible with a future `std::meta` backend.
// ---------------------------------------------------------------------------
#define REFR_REFLECT(...) REFL_AUTO(__VA_ARGS__)
#define REFR_FIELD(...) field(__VA_ARGS__)
#define REFR_ENUM(...)

namespace refrain::reflect {

/// True when the backend can reflect `T`. CV/ref-qualified spellings resolve
/// to the underlying type. refl-cpp pre-reflects primitives and some standard
/// types (they expose only a `name`, zero fields); user types require
/// `REFR_REFLECT`, otherwise this is false.
/// Descriptor for a manually registered field (see `custom_traits`).
template <typename T, typename V> struct custom_field {
    std::string_view name;
    V T::*pointer;
};

template <typename T, typename V>
custom_field(std::string_view, V T::*) -> custom_field<T, V>;

/// Escape hatch for types that cannot be annotated with `REFR_REFLECT`
/// (third-party types, C ABI structs, generated code). Specialize it with:
///
///   template <> struct refrain::reflect::custom_traits<vec3> {
///       static constexpr std::string_view type_name = "vec3";
///       static constexpr auto fields = std::make_tuple(
///           refrain::reflect::custom_field{"x", &vec3::x}, ...);
///   };
///
/// The primary template intentionally has no `fields`, so unregistered types
/// stay unreflected.
template <typename T> struct custom_traits {};

template <typename T>
concept custom_reflected =
    requires { custom_traits<std::remove_cvref_t<T>>::fields; };

template <typename T>
concept reflected =
    custom_reflected<T> ||
    bool{::refl::trait::is_reflectable_v<std::remove_cvref_t<T>>};

namespace detail {

template <typename T>
[[nodiscard]] constexpr auto type_name_impl() noexcept -> std::string_view {
    static_assert(reflected<T>,
                  "refrain::reflect: type is not annotated with REFR_REFLECT");
    if constexpr (custom_reflected<T>) {
        return custom_traits<std::remove_cvref_t<T>>::type_name;
    } else {
        constexpr auto descriptor = ::refl::reflect<std::remove_cvref_t<T>>();
        return std::string_view{descriptor.name.c_str(), descriptor.name.size};
    }
}

} // namespace detail

/// Compile-time metadata for a reflected type.
template <typename T>
    requires reflected<T>
inline constexpr std::string_view name = detail::type_name_impl<T>();

/// Number of reflected fields (declared + inherited), known at compile time.
template <typename T>
    requires reflected<T>
inline constexpr std::size_t field_count = [] {
    if constexpr (custom_reflected<T>) {
        return std::tuple_size_v<std::remove_cvref_t<
            decltype(custom_traits<std::remove_cvref_t<T>>::fields)>>;
    } else {
        return static_cast<std::size_t>(
            ::refl::member_list<std::remove_cvref_t<T>>::size);
    }
}();

/// Name of the field at `Index` (fields keep declaration order).
template <typename T, std::size_t Index>
    requires reflected<T>
[[nodiscard]] constexpr auto field_name() noexcept -> std::string_view {
    static_assert(Index < field_count<T>,
                  "refrain::reflect::field_name: index out of range");
    if constexpr (custom_reflected<T>) {
        return std::get<Index>(custom_traits<std::remove_cvref_t<T>>::fields)
            .name;
    } else {
        using member =
            ::refl::trait::get_t<Index,
                                 ::refl::member_list<std::remove_cvref_t<T>>>;
        return std::string_view{member::name.c_str(), member::name.size};
    }
}

/// Member pointer for the field at `Index` (backend or custom traits).
template <typename T, std::size_t Index>
    requires reflected<T> && (Index < field_count<T>)
[[nodiscard]] constexpr auto field_pointer() {
    if constexpr (custom_reflected<T>) {
        return std::get<Index>(custom_traits<std::remove_cvref_t<T>>::fields)
            .pointer;
    } else {
        using member =
            ::refl::trait::get_t<Index,
                                 ::refl::member_list<std::remove_cvref_t<T>>>;
        return member::pointer;
    }
}

/// True when the field carries `Attribute` (see `REFR_FIELD(name, attr...)`).
template <typename T, std::size_t Index, typename Attribute>
    requires reflected<T> && (Index < field_count<T>)
[[nodiscard]] constexpr auto has_field_attribute() noexcept -> bool {
    using member =
        ::refl::trait::get_t<Index,
                             ::refl::member_list<std::remove_cvref_t<T>>>;
    return ::refl::trait::contains_base_v<Attribute,
                                          typename member::attribute_types>;
}

/// The field's `Attribute` value. Requires `has_field_attribute()`; the
/// backend fails the compile otherwise.
template <typename T, std::size_t Index, typename Attribute>
    requires reflected<T> && (Index < field_count<T>)
constexpr auto field_attribute() -> Attribute {
    using member =
        ::refl::trait::get_t<Index,
                             ::refl::member_list<std::remove_cvref_t<T>>>;
    return std::get<Attribute>(member::attributes);
}

/// Invoke `fn(std::string_view field_name, auto&& value)` for every reflected
/// field of `object`, in declaration order. Compile-time expansion: no runtime
/// table, no allocation, and `value` references the actual field (a mutable
/// object yields mutable references, a const object const references).
template <typename T, typename Fn>
constexpr void for_each_field(T &&object, Fn &&fn) {
    using target = std::remove_cvref_t<T>;
    static_assert(reflected<target>,
                  "refrain::reflect::for_each_field: type is not annotated "
                  "with REFR_REFLECT");
    if constexpr (custom_reflected<target>) {
        std::apply(
            [&object, &fn](const auto &...field) {
                (fn(field.name, object.*(field.pointer)), ...);
            },
            custom_traits<target>::fields);
    } else {
        ::refl::util::for_each(
            ::refl::reflect<target>().members, [&object, &fn](auto member) {
                fn(std::string_view{member.name.c_str(), member.name.size},
                   member.get(object));
            });
    }
}

// ---------------------------------------------------------------------------
// Enums (magic_enum wrapper; no annotation required).
// ---------------------------------------------------------------------------

/// Name of an enumerator, or an empty view for a value outside the enum.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr auto enum_name(E value) noexcept -> std::string_view {
    return ::magic_enum::enum_name(value);
}

/// Parse an enumerator name (exact match, no case folding). `std::nullopt`
/// when the name does not belong to the enum.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr auto enum_cast(std::string_view text) noexcept
    -> std::optional<E> {
    return ::magic_enum::enum_cast<E>(text);
}

/// Every enumerator, in declaration order (contiguous enums only; the same
/// range magic_enum itself supports).
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr auto enum_values() noexcept {
    return ::magic_enum::enum_values<E>();
}

/// Visit every enumerator in declaration order.
template <typename E, typename Fn> constexpr void for_each_enumerator(Fn &&fn) {
    for (const auto value : ::magic_enum::enum_values<E>()) {
        fn(value);
    }
}

} // namespace refrain::reflect
