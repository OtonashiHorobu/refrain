// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8a20087cf2517c49

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_STATUS_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_STATUS_HPP

#include <refrain/contracts/services/session_status_abi.h>
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

namespace refrain::services::session_status {

using status_request = ::refrain_services_session_status_status_request;
/// Zero-initialized `status_request` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_status_request() noexcept -> status_request {
    status_request value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using status_view = ::refrain_services_session_status_status_view;
/// Zero-initialized `status_view` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_status_view() noexcept -> status_view {
    status_view value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(status_request) == 32);
static_assert(alignof(status_request) == 8);
static_assert(offsetof(status_request, struct_size) == 0);
static_assert(offsetof(status_request, protocol) == 4);
static_assert(offsetof(status_request, host) == 8);
static_assert(offsetof(status_request, port) == 24);
static_assert(offsetof(status_request, online_players) == 28);
static_assert(sizeof(status_view) == 32);
static_assert(alignof(status_view) == 8);
static_assert(offsetof(status_view, struct_size) == 0);
static_assert(offsetof(status_view, description) == 8);
static_assert(offsetof(status_view, online_players) == 24);
static_assert(offsetof(status_view, max_players) == 28);

inline constexpr std::string_view service_id = "refrain:session.status";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::worker;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::instanced;
inline constexpr std::string_view capabilities = "dynamic-motd,dynamic-player-count";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:session.status"};

/// Type-safe wrapper around `refrain_services_session_status_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_session_status_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_session_status_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Return true when this provider handled the request. `description` and
    /// counts are borrowed until the next call on this provider instance.
    [[nodiscard]] auto status(const ::refrain::services::session_status::status_request &request, ::refrain::services::session_status::status_view &view) const -> bool {
        const auto abi_request = ::refrain::contracts::with_struct_size(request);
        ::refrain::contracts::init_struct(view);
        return (table_->status(self_, &abi_request, &view) != 0);
    }

  private:
    const ::refrain_services_session_status_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::session_status

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_STATUS_HPP
