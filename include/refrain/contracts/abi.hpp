// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// C++ conveniences layered on top of the stable C ABI (include/refrain/
// contracts/abi.h). Generated contract headers include this file; it is
// hand-maintained and template/header-only so mods need no engine library.

#pragma once

#include <refrain/contracts/abi.h>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace refrain::contracts {

/// Strongly typed opaque handle. `Tag` is an incomplete marker type declared
/// by the generated module header; the handle itself is always a uint64_t.
///
/// `operator==` is explicitly defaulted: a defaulted `operator<=>` does not
/// implicitly synthesize equality, so handles could not be compared with `==`
/// without this line. Both rewrites (`==`/`!=`) and all relational
/// operators are available, matching the C `refrain_handle` value semantics.
template <typename Tag> struct handle {
    std::uint64_t value{};

    constexpr handle() noexcept = default;
    constexpr explicit handle(std::uint64_t raw) noexcept : value{raw} {}

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return value != REFRAIN_HANDLE_INVALID;
    }

    [[nodiscard]] constexpr auto raw() const noexcept -> std::uint64_t {
        return value;
    }

    friend constexpr bool operator==(const handle &,
                                     const handle &) noexcept = default;
    friend constexpr auto operator<=>(const handle &,
                                      const handle &) noexcept = default;
};

using status = std::uint32_t;

inline constexpr status status_ok = REFRAIN_STATUS_OK;
inline constexpr std::uint32_t abi_major = REFRAIN_ABI_MAJOR;
inline constexpr std::uint32_t abi_minor = REFRAIN_ABI_MINOR;

[[nodiscard]] inline auto as_string_view(const refrain_str &text) noexcept
    -> std::string_view {
    if (text.data == nullptr) {
        return {};
    }
    return std::string_view{reinterpret_cast<const char *>(text.data),
                            static_cast<std::size_t>(text.len)};
}

[[nodiscard]] inline auto as_bytes(const refrain_bytes &data) noexcept
    -> std::span<const std::uint8_t> {
    if (data.data == nullptr) {
        return {};
    }
    return std::span<const std::uint8_t>{data.data,
                                         static_cast<std::size_t>(data.len)};
}

[[nodiscard]] inline auto to_borrowed(std::string_view text) noexcept
    -> refrain_str {
    return refrain_str{reinterpret_cast<const std::uint8_t *>(text.data()),
                       static_cast<std::uint64_t>(text.size())};
}

[[nodiscard]] inline auto
to_borrowed(std::span<const std::uint8_t> data) noexcept -> refrain_bytes {
    return refrain_bytes{data.data(), static_cast<std::uint64_t>(data.size())};
}

[[nodiscard]] inline auto to_mut(std::span<std::uint8_t> data) noexcept
    -> refrain_mut_bytes {
    return refrain_mut_bytes{data.data(),
                             static_cast<std::uint64_t>(data.size())};
}

/// Generated ABI structs always start with `uint32_t struct_size`.
template <typename T>
concept sized_abi_struct = requires(T &value) {
    { value.struct_size } -> std::convertible_to<std::uint32_t>;
};

template <sized_abi_struct T> constexpr void init_struct(T &value) noexcept {
    static_assert(std::is_standard_layout_v<T>);
    value.struct_size = static_cast<std::uint32_t>(sizeof(T));
}

/// Copy of a generated ABI struct with the mandatory `struct_size` filled in.
/// The generated C++ wrappers pass input contract structs through this helper,
/// so a caller that forgot `init_struct()` can never send `struct_size == 0`
/// across the ABI boundary (the provider may validate it).
template <sized_abi_struct T>
[[nodiscard]] constexpr auto with_struct_size(const T &value) noexcept -> T {
    auto copy{value};
    copy.struct_size = static_cast<std::uint32_t>(sizeof(T));
    return copy;
}

/// Service metadata (generated headers expose these as constexpr values).
enum class side_kind : std::uint32_t {
    none = REFRAIN_SIDE_NONE,
    server = REFRAIN_SIDE_SERVER,
    client = REFRAIN_SIDE_CLIENT,
    both = REFRAIN_SIDE_BOTH,
};

enum class thread_kind : std::uint32_t {
    any = REFRAIN_THREAD_ANY,
    main = REFRAIN_THREAD_MAIN,
    sim = REFRAIN_THREAD_SIM,
    render = REFRAIN_THREAD_RENDER,
    worker = REFRAIN_THREAD_WORKER,
    io = REFRAIN_THREAD_IO,
    net = REFRAIN_THREAD_NET,
};

/// Contract composition mode. Generated headers expose it as a
/// `composition` constexpr and as the C macro `<PREFIX>_COMPOSITION`; the
/// default is `exclusive` (one active provider per service).
enum class composition_kind : std::uint32_t {
    exclusive = REFRAIN_COMPOSITION_EXCLUSIVE,
    instanced = REFRAIN_COMPOSITION_INSTANCED,
    chain = REFRAIN_COMPOSITION_CHAIN,
    registry = REFRAIN_COMPOSITION_REGISTRY,
    sealed = REFRAIN_COMPOSITION_SEALED,
};

} // namespace refrain::contracts
