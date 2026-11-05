// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Named `refrain::attr::*` annotations used with `REFR_FIELD`. This header is
// included by `refrain/reflect.hpp`; business code includes only the facade.
//
// Each annotation derives from `refrain::reflect::field_attribute_tag` so the
// refl-cpp backend accepts it on `field(...)`.

#pragma once

#include <string_view>

namespace refrain::reflect {

/// Base tag for field attributes. Exposed here so annotations and business
/// attributes share the same backend-agnostic spelling.
using field_attribute_tag = ::refl::attr::usage::field;
using type_attribute_tag = ::refl::attr::usage::type;

} // namespace refrain::reflect

namespace refrain::attr {

/// Inclusive numeric range for editors/validation (display metadata only; not
/// wire semantics).
struct range : reflect::field_attribute_tag {
    int min{};
    int max{};
    constexpr range(int lower, int upper) : min{lower}, max{upper} {}
};

/// Field is log/debug output only; never part of a save or wire projection.
struct debug_only : reflect::field_attribute_tag {};

/// Field is part of a network-visible projection (the projection itself is
/// still declared by schema/codegen, not inferred from this annotation).
struct net_field : reflect::field_attribute_tag {};

/// Logging must redact this value.
struct sensitive : reflect::field_attribute_tag {};

/// Hide from editor property panels.
struct editor_hidden : reflect::field_attribute_tag {};

/// Display unit such as "m/s" or "ticks".
struct unit : reflect::field_attribute_tag {
    std::string_view symbol{};
    constexpr explicit unit(std::string_view value) : symbol{value} {}
};

} // namespace refrain::attr
