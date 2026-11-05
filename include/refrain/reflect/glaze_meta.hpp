// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Glaze `glz::meta` glue built from `refrain::reflect` field metadata. This is
// for JSON/config/tooling only; wire/archive formats remain schema codegen.
//
//   REFR_GLAZE_META(my::type)
//
// generates `glz::meta<my::type>` with every reflected field in declaration
// order. Types with `custom_traits` work too.

#pragma once

#include <refrain/reflect.hpp>

#include <glaze/glaze.hpp>

#include <cstddef>
#include <utility>

namespace refrain::reflect {

namespace detail {

template <typename T, std::size_t K> [[nodiscard]] constexpr auto glaze_arg() {
    constexpr std::size_t index{K / 2U};
    if constexpr (K % 2U == 0U) {
        return field_name<T, index>();
    } else {
        return field_pointer<T, index>();
    }
}

template <typename T, std::size_t... K>
[[nodiscard]] constexpr auto glaze_object_impl(std::index_sequence<K...>) {
    return glz::object(glaze_arg<T, K>()...);
}

} // namespace detail

/// `glz::object(...)` for every reflected field of `T`.
template <typename T>
    requires reflected<T>
inline constexpr auto glaze_object = detail::glaze_object_impl<T>(
    std::make_index_sequence<2U * field_count<T>>{});

} // namespace refrain::reflect

/// Define `glz::meta<T>` from the reflection facade.
#define REFR_GLAZE_META(T)                                                     \
    namespace glz {                                                            \
    template <> struct meta<T> {                                               \
        using type = T;                                                        \
        static constexpr auto value = ::refrain::reflect::glaze_object<T>;     \
    };                                                                         \
    }
