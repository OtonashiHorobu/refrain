// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace refrain::core::byte_order {

/// The primitives below assume a host with one pure byte order. A mixed-endian
/// host would need an explicit byte permutation, and nothing in the project
/// builds on one.
static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big);

/// Integer widths used by every on-disk and on-wire fixed field.
template <typename T>
concept fixed_width_integer =
    std::integral<T> && !std::same_as<T, bool> &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

/// Reverse the byte order of an integer.
///
/// The value passes through its unsigned representation, so a negative input
/// keeps its exact two's-complement bits. A single-byte value is unchanged.
template <fixed_width_integer T>
[[nodiscard]] constexpr auto byteswap(T value) noexcept -> T {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else {
        using unsigned_type = std::make_unsigned_t<T>;
        return std::bit_cast<T>(
            std::byteswap(static_cast<unsigned_type>(value)));
    }
}

namespace detail {

/// Copy `sizeof(T)` bytes at `offset` into an integer in host byte order.
/// The array makes the load independent of the source buffer's alignment.
template <fixed_width_integer T>
[[nodiscard]] constexpr auto load_native(std::span<const std::byte> bytes,
                                         std::size_t offset) noexcept -> T {
    std::array<std::byte, sizeof(T)> raw{};
    for (std::size_t index{0}; index < sizeof(T); ++index) {
        raw[index] = bytes[offset + index];
    }
    return std::bit_cast<T>(raw);
}

/// Write an integer's host-order bytes at `offset`.
template <fixed_width_integer T>
constexpr void store_native(std::span<std::byte> out, std::size_t offset,
                            T value) noexcept {
    const auto raw{std::bit_cast<std::array<std::byte, sizeof(T)>>(value)};
    for (std::size_t index{0}; index < sizeof(T); ++index) {
        out[offset + index] = raw[index];
    }
}

} // namespace detail

/// Load `T` from little-endian bytes.
///
/// The caller guarantees that `[offset, offset + sizeof(T))` is in bounds; the
/// packet, NBT and table parsers validate the length before calling. A value
/// is copied through `bit_cast` and swapped only when the host order differs,
/// so the load neither allocates nor depends on the buffer's alignment.
template <fixed_width_integer T>
[[nodiscard]] constexpr auto load_le(std::span<const std::byte> bytes,
                                     std::size_t offset = 0) noexcept -> T {
    const auto native{detail::load_native<T>(bytes, offset)};
    if constexpr (sizeof(T) == 1 ||
                  std::endian::native == std::endian::little) {
        return native;
    } else {
        return byteswap(native);
    }
}

/// Load `T` from big-endian bytes, with the same bounds contract as `load_le`.
template <fixed_width_integer T>
[[nodiscard]] constexpr auto load_be(std::span<const std::byte> bytes,
                                     std::size_t offset = 0) noexcept -> T {
    const auto native{detail::load_native<T>(bytes, offset)};
    if constexpr (sizeof(T) == 1 || std::endian::native == std::endian::big) {
        return native;
    } else {
        return byteswap(native);
    }
}

/// Store `T` as little-endian bytes, with the same bounds contract as
/// `load_le`.
template <fixed_width_integer T>
constexpr void store_le(std::span<std::byte> out, std::size_t offset,
                        T value) noexcept {
    if constexpr (sizeof(T) != 1 && std::endian::native == std::endian::big) {
        value = byteswap(value);
    }
    detail::store_native<T>(out, offset, value);
}

/// Store `T` as big-endian bytes, with the same bounds contract as `load_le`.
template <fixed_width_integer T>
constexpr void store_be(std::span<std::byte> out, std::size_t offset,
                        T value) noexcept {
    if constexpr (sizeof(T) != 1 &&
                  std::endian::native == std::endian::little) {
        value = byteswap(value);
    }
    detail::store_native<T>(out, offset, value);
}

/// Fixed-width wrappers for the field sizes of the packet and file formats.
[[nodiscard]] constexpr auto load_le16(std::span<const std::byte> bytes,
                                       std::size_t offset = 0) noexcept
    -> std::uint16_t {
    return load_le<std::uint16_t>(bytes, offset);
}

[[nodiscard]] constexpr auto load_le32(std::span<const std::byte> bytes,
                                       std::size_t offset = 0) noexcept
    -> std::uint32_t {
    return load_le<std::uint32_t>(bytes, offset);
}

[[nodiscard]] constexpr auto load_le64(std::span<const std::byte> bytes,
                                       std::size_t offset = 0) noexcept
    -> std::uint64_t {
    return load_le<std::uint64_t>(bytes, offset);
}

[[nodiscard]] constexpr auto load_be16(std::span<const std::byte> bytes,
                                       std::size_t offset = 0) noexcept
    -> std::uint16_t {
    return load_be<std::uint16_t>(bytes, offset);
}

[[nodiscard]] constexpr auto load_be32(std::span<const std::byte> bytes,
                                       std::size_t offset = 0) noexcept
    -> std::uint32_t {
    return load_be<std::uint32_t>(bytes, offset);
}

[[nodiscard]] constexpr auto load_be64(std::span<const std::byte> bytes,
                                       std::size_t offset = 0) noexcept
    -> std::uint64_t {
    return load_be<std::uint64_t>(bytes, offset);
}

constexpr void store_le16(std::span<std::byte> out, std::size_t offset,
                          std::uint16_t value) noexcept {
    store_le<std::uint16_t>(out, offset, value);
}

constexpr void store_le32(std::span<std::byte> out, std::size_t offset,
                          std::uint32_t value) noexcept {
    store_le<std::uint32_t>(out, offset, value);
}

constexpr void store_le64(std::span<std::byte> out, std::size_t offset,
                          std::uint64_t value) noexcept {
    store_le<std::uint64_t>(out, offset, value);
}

constexpr void store_be16(std::span<std::byte> out, std::size_t offset,
                          std::uint16_t value) noexcept {
    store_be<std::uint16_t>(out, offset, value);
}

constexpr void store_be32(std::span<std::byte> out, std::size_t offset,
                          std::uint32_t value) noexcept {
    store_be<std::uint32_t>(out, offset, value);
}

constexpr void store_be64(std::span<std::byte> out, std::size_t offset,
                          std::uint64_t value) noexcept {
    store_be<std::uint64_t>(out, offset, value);
}

} // namespace refrain::core::byte_order
