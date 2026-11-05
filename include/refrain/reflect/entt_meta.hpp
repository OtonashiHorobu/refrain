// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// EnTT meta glue (editor/script runtime path). Registration is explicit:
//
//   REFR_REFLECT(...)
//   refrain::reflect::register_entt_meta<my::type>();
//
// Field names/pointers come from the reflection facade; this header is not
// included by `reflect.hpp` so the core facade stays free of EnTT.

#pragma once

#include <refrain/reflect.hpp>

#include <entt/core/hashed_string.hpp>
#include <entt/meta/factory.hpp>
#include <entt/meta/resolve.hpp>

#include <cstddef>
#include <utility>

namespace refrain::reflect {

namespace detail {

template <typename T, std::size_t... I>
auto register_entt_fields(std::index_sequence<I...>) -> void {
    auto type = entt::meta_factory<T>{}.type(entt::type_hash<T>::value());
    ((type.template data<field_pointer<T, I>()>(entt::hashed_string::value(
         field_name<T, I>().data(), field_name<T, I>().size()))),
     ...);
}

} // namespace detail

/// Register every reflected field of `T` with EnTT meta once. Safe to call
/// repeatedly; later calls are no-ops.
template <typename T>
    requires reflected<T>
auto register_entt_meta() -> void {
    static const bool registered = [] {
        detail::register_entt_fields<T>(
            std::make_index_sequence<field_count<T>>{});
        return true;
    }();
    static_cast<void>(registered);
}

} // namespace refrain::reflect
