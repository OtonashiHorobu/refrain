// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 1bc5417b6dec5904

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_TAB_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_TAB_HPP

#include <refrain/contracts/services/session_tab_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::session_tab {

using tab_request = ::refrain_services_session_tab_tab_request;
/// Zero-initialized `tab_request` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_tab_request() noexcept -> tab_request {
    tab_request value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using tab_view = ::refrain_services_session_tab_tab_view;
/// Zero-initialized `tab_view` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_tab_view() noexcept -> tab_view {
    tab_view value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(tab_request) == 32);
static_assert(alignof(tab_request) == 8);
static_assert(offsetof(tab_request, struct_size) == 0);
static_assert(offsetof(tab_request, connection) == 8);
static_assert(offsetof(tab_request, player_name) == 16);
static_assert(sizeof(tab_view) == 40);
static_assert(alignof(tab_view) == 8);
static_assert(offsetof(tab_view, struct_size) == 0);
static_assert(offsetof(tab_view, header) == 8);
static_assert(offsetof(tab_view, footer) == 24);

inline constexpr std::string_view service_id = "refrain:session.tab";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::worker;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::instanced;
inline constexpr std::string_view capabilities = "dynamic-tab";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:session.tab"};

/// Type-safe wrapper around `refrain_services_session_tab_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_session_tab_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_session_tab_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Return true when this provider handled the player. Strings are
    /// borrowed until the next call on this provider instance.
    [[nodiscard]] auto tab(const ::refrain::services::session_tab::tab_request &request, ::refrain::services::session_tab::tab_view &view) const -> bool {
        const auto abi_request = ::refrain::contracts::with_struct_size(request);
        ::refrain::contracts::init_struct(view);
        return (table_->tab(self_, &abi_request, &view) != 0);
    }

  private:
    const ::refrain_services_session_tab_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::session_tab

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_TAB_HPP
