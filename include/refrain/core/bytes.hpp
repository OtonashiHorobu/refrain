// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace refrain::core::bytes {

/// Read an integer from little-endian bytes. The caller guarantees that
/// `[offset, offset + sizeof(T))` is in bounds; format parsers validate sizes
/// before calling this. The byte-at-a-time loop yields the same value on every
/// host byte order.
template <std::integral T>
[[nodiscard]] constexpr auto read_le(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept -> T {
    static_assert(std::is_integral_v<T>);
    using value_type = std::make_unsigned_t<T>;
    value_type value{0};
    for (std::size_t index{0}; index < sizeof(T); ++index) {
        const auto byte_value{static_cast<value_type>(
            std::to_integer<unsigned int>(bytes[offset + index]))};
        value |= static_cast<value_type>(byte_value << (8U * index));
    }
    return static_cast<T>(value);
}

/// Write an integer as little-endian bytes. Same bounds contract as `read_le`.
template <std::integral T>
constexpr void write_le(std::span<std::byte> out, std::size_t offset,
                        T value) noexcept {
    static_assert(std::is_integral_v<T>);
    using value_type = std::make_unsigned_t<T>;
    const auto converted{static_cast<value_type>(value)};
    for (std::size_t index{0}; index < sizeof(T); ++index) {
        out[offset + index] =
            static_cast<std::byte>((converted >> (8U * index)) & 0xFFU);
    }
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
