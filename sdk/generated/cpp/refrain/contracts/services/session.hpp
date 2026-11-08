// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 25b2c961ad0ac277

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_HPP

#include <refrain/contracts/services/session_abi.h>
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

namespace refrain::services::session {

using session_descriptor = ::refrain_services_session_session_descriptor;
/// Zero-initialized `session_descriptor` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_session_descriptor() noexcept -> session_descriptor {
    session_descriptor value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(session_descriptor) == 56);
static_assert(alignof(session_descriptor) == 8);
static_assert(offsetof(session_descriptor, struct_size) == 0);
static_assert(offsetof(session_descriptor, family) == 8);
static_assert(offsetof(session_descriptor, edition) == 24);
static_assert(offsetof(session_descriptor, capabilities) == 40);

inline constexpr std::string_view service_id = "refrain:session";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::main;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::instanced;
inline constexpr std::string_view capabilities = "tick,console";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:session"};

/// Type-safe wrapper around `refrain_services_session_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_session_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_session_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    auto descriptor(::refrain::services::session::session_descriptor &value) const -> void {
        ::refrain::contracts::init_struct(value);
        table_->descriptor(self_, &value);
    }

  private:
    const ::refrain_services_session_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::session

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_HPP
