// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 29871d2bb99d3b05

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SCRIPT_HOST_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SCRIPT_HOST_HPP

#include <refrain/contracts/services/script_host_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/contracts/services/mod_loader.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::script_host {

/// Script host failures. Zero is success, so entries start at one. The codes
/// distinguish a caller mistake (`invalid_argument`, `not_found`) from guest
/// misbehaviour (`budget_exceeded`, `deadline_exceeded`, `guest_trapped`) and
/// the state a trapped instance enters (`quarantined`).
enum class script_error : std::uint32_t { // enum 'script_error'
    invalid_argument = 1,
    not_found = 2,
    permission_denied = 3,
    unsupported = 4,
    /// The guest exhausted the per-call instruction budget.
    budget_exceeded = 5,
    /// The guest outlived the per-call wall-clock deadline.
    deadline_exceeded = 6,
    /// The guest trapped for a reason other than a budget, for example an
    /// unreachable instruction or an out-of-bounds memory access.
    guest_trapped = 7,
    /// The instance is quarantined after an earlier failed call. A quarantined
    /// instance runs no further guest code and is released by unloading it.
    quarantined = 8,
    internal = 9,
};

/// One callback invocation.
using script_call_request = ::refrain_services_script_host_script_call_request;
/// Zero-initialized `script_call_request` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_script_call_request() noexcept -> script_call_request {
    script_call_request value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Result of one callback invocation.
using script_call_result = ::refrain_services_script_host_script_call_result;
/// Zero-initialized `script_call_result` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_script_call_result() noexcept -> script_call_result {
    script_call_result value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(script_call_request) == 40);
static_assert(alignof(script_call_request) == 8);
static_assert(offsetof(script_call_request, struct_size) == 0);
static_assert(offsetof(script_call_request, callback) == 8);
static_assert(offsetof(script_call_request, arguments) == 24);
static_assert(sizeof(script_call_result) == 8);
static_assert(alignof(script_call_result) == 4);
static_assert(offsetof(script_call_result, struct_size) == 0);
static_assert(offsetof(script_call_result, value) == 4);

inline constexpr std::string_view service_id = "refrain:script.host";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::main;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::sealed;
inline constexpr std::string_view capabilities = "call-dispatch,budgeted-calls";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:script.host"};

/// Type-safe wrapper around `refrain_services_script_host_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_script_host_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_script_host_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Invoke one exported guest callback. The call runs under the loader's
    /// per-call instruction budget and wall-clock deadline and is refused for
    /// a quarantined instance. A guest that traps is quarantined: the instance
    /// stays registered but executes no further guest code until it is
    /// unloaded, and `unload` releases the whole store either way.
    [[nodiscard]] auto call(::refrain::services::mod_loader::mod_instance instance, const ::refrain::services::script_host::script_call_request &request) const -> std::expected<::refrain::services::script_host::script_call_result, ::refrain::services::script_host::script_error> {
        const auto abi_request = ::refrain::contracts::with_struct_size(request);
        ::refrain_services_script_host_script_call_result value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->call(self_, instance.raw(), &abi_request, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::script_host::script_error>(code));
        }
        return value;
    }

  private:
    const ::refrain_services_script_host_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::script_host

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SCRIPT_HOST_HPP
