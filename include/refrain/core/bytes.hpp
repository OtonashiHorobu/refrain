// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/byte_order.hpp>

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace refrain::core::bytes {

/// Read an integer from little-endian bytes. The caller guarantees that
/// `[offset, offset + sizeof(T))` is in bounds; format parsers validate sizes
/// before calling this. The value is loaded through `byte_order`, so it is the
/// same on every host byte order.
template <byte_order::fixed_width_integer T>
[[nodiscard]] constexpr auto read_le(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept -> T {
    return byte_order::load_le<T>(bytes, offset);
}

/// Write an integer as little-endian bytes. Same bounds contract as `read_le`.
template <byte_order::fixed_width_integer T>
constexpr void write_le(std::span<std::byte> out, std::size_t offset,
                        T value) noexcept {
    byte_order::store_le<T>(out, offset, value);
}

/// Convenience wrappers for the fixed-width fields of the on-disk formats.
[[nodiscard]] constexpr auto read_u16(std::span<const std::byte> bytes,
                                      std::size_t offset) noexcept
    -> std::uint16_t {
    return read_le<std::uint16_t>(bytes, offset);
}

[[nodiscard]] constexpr auto read_u32(std::span<const std::byte> bytes,
                                      std::size_t offset) noexcept
    -> std::uint32_t {
    return read_le<std::uint32_t>(bytes, offset);
}

[[nodiscard]] constexpr auto read_u64(std::span<const std::byte> bytes,
                                      std::size_t offset) noexcept
    -> std::uint64_t {
    return read_le<std::uint64_t>(bytes, offset);
}

/// Round `value` up to the next multiple of `alignment`, reporting failures.
///
/// Returns `std::nullopt` when `alignment` is zero or not a power of two, or
/// when `value + alignment - 1` does not fit in 64 bits. Callers that may see
/// unchecked/hostile sizes must use this instead of `align_up`.
[[nodiscard]] constexpr auto align_up_checked(std::uint64_t value,
                                              std::uint64_t alignment) noexcept
    -> std::optional<std::uint64_t> {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0) {
        return std::nullopt;
    }
    const auto mask{alignment - 1U};
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
        return std::nullopt;
    }
    return (value + mask) & ~mask;
}

/// Round `value` up to the next multiple of `alignment` (a power of two).
///
/// Precondition: `alignment` is a non-zero power of two and the rounded value
/// fits in 64 bits. A violation is a caller bug, so it trips an assertion in
/// debug builds and returns 0 in release builds; input that is not already
/// validated must go through `align_up_checked` instead.
[[nodiscard]] constexpr auto align_up(std::uint64_t value,
                                      std::uint64_t alignment) noexcept
    -> std::uint64_t {
    const auto aligned{align_up_checked(value, alignment)};
    assert(aligned.has_value());
    return aligned.value_or(0);
}

} // namespace refrain::core::bytes
