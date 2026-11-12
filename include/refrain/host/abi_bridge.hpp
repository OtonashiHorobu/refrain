// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The single conversion layer between the C ABI primitives (`refrain_str`,
// `refrain_bytes`, `refrain_status`) and the C++ values used inside the host
// and its mods. Every contract adapter and provider calls these helpers; a
// second copy of any of them is a silent ABI bug waiting to diverge.
//
// Everything here is inline and header-only, so a statically linked mod and a
// dynamically loaded plugin compile the same code from the installed headers
// and need no engine symbol. The borrowed-span conversions cannot throw and
// are marked noexcept. A null pointer and a zero length both read as the empty
// view: a caller may pass `{.data = nullptr, .len = 0}` and never gets a
// string_view or span built over a null buffer.
//
// The underlying primitives (borrowing a view, borrowing bytes) live in
// <refrain/contracts/abi.hpp>; this header adds the provider-facing names, the
// owning copy and the error -> status table so the adapters share one
// implementation instead of one per module.

#pragma once

#include <refrain/contracts/abi.hpp>
#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace refrain::host::abi {

/// ABI string -> borrowed view. A null pointer or a zero length both read as
/// the empty view, so the result never points at a null buffer.
[[nodiscard]] inline auto to_view(const refrain_str &text) noexcept
    -> std::string_view {
    if (text.len == 0) {
        return {};
    }
    return contracts::as_string_view(text);
}

/// Owned copy of an ABI string. Embedded NUL bytes are preserved: the length
/// is the ABI length, never a terminator. This is the only conversion that
/// allocates, so it is the only one that is not noexcept; the caller reports
/// the allocation failure the same way it reports any other.
[[nodiscard]] inline auto to_string(const refrain_str &text) -> std::string {
    return std::string{to_view(text)};
}

/// ABI bytes -> borrowed span, with the same null/empty rule as to_view.
[[nodiscard]] inline auto to_span(const refrain_bytes &data) noexcept
    -> std::span<const std::uint8_t> {
    if (data.len == 0) {
        return {};
    }
    return contracts::as_bytes(data);
}

/// Borrowed span -> ABI bytes. The result points into `data`, which must stay
/// alive for as long as the callee may read it.
[[nodiscard]] inline auto to_bytes(std::span<const std::uint8_t> data) noexcept
    -> refrain_bytes {
    return refrain_bytes{.data = data.data(), .len = data.size()};
}

/// Borrowed view -> ABI string, the inverse of to_view. The result points into
/// `text`, which must stay alive for as long as the callee may read it.
using contracts::to_borrowed;

/// One row of a provider's error -> status table.
struct status_entry {
    error_code code;
    refrain_status status;
};

/// Maps a kernel error to the status a contract returns. A contract only
/// distinguishes a handful of codes, so each provider supplies the rows it
/// maps plus one fallback for everything else; the lookup itself lives here
/// and nowhere else. The table is a constant of a few rows on the failure
/// path.
template <std::size_t N>
[[nodiscard]] constexpr auto to_status(error_code code,
                                       const status_entry (&table)[N],
                                       refrain_status fallback) noexcept
    -> refrain_status {
    for (const auto &entry : table) {
        if (entry.code == code) {
            return entry.status;
        }
    }
    return fallback;
}

template <std::size_t N>
[[nodiscard]] constexpr auto to_status(const refrain::error &failure,
                                       const status_entry (&table)[N],
                                       refrain_status fallback) noexcept
    -> refrain_status {
    return to_status(failure.code, table, fallback);
}

} // namespace refrain::host::abi
