// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8597ddcddbb41e5d

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_KERNEL_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_KERNEL_HPP

#include <refrain/contracts/host/kernel_abi.h>
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

namespace refrain::host::kernel {

enum class kernel_error : std::uint32_t { // enum 'kernel_error'
    invalid_argument = 1,
    not_found = 2,
    permission_denied = 3,
    unsupported = 4,
    out_of_memory = 5,
    cancelled = 6,
    timeout = 7,
    thread_violation = 8,
    internal = 9,
};

namespace detail {
struct job_tag;
struct subscription_tag;
} // namespace detail

using job = ::refrain::contracts::handle<detail::job_tag>;
using subscription = ::refrain::contracts::handle<detail::subscription_tag>;

/// `void (*)(void *userdata)`.
using job_callback = void (*)(void * userdata);

/// `void (*)(void *userdata, void *payload, u64 size)`; the payload is
/// borrowed for the duration of the callback.
using event_callback = void (*)(void * userdata, void * payload, std::uint64_t size);



inline constexpr std::string_view service_id = "refrain:host.kernel";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::both;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::sealed;
inline constexpr std::string_view capabilities = "jobs,events";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:host.kernel"};

/// Type-safe wrapper around `refrain_host_kernel_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_host_kernel_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_host_kernel_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Submit a background job. Priority: 0 interactive, 1 player-visible,
    /// 2 normal, 3 background. The callback runs on a host worker thread.
    [[nodiscard]] auto job_submit(std::uint32_t priority, ::refrain::host::kernel::job_callback callback, void * userdata) const -> std::expected<::refrain::host::kernel::job, ::refrain::host::kernel::kernel_error> {
        ::refrain_host_kernel_handle_job value{};
        if (const auto code = table_->job_submit(self_, priority, static_cast<refrain_host_kernel_job_callback>(callback), userdata, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::kernel::kernel_error>(code));
        }
        return ::refrain::host::kernel::job{value};
    }

    /// Mark a queued job as cancelled. A job already running is not
    /// interrupted.
    [[nodiscard]] auto job_cancel(::refrain::host::kernel::job handle) const -> std::expected<void, ::refrain::host::kernel::kernel_error> {
        if (const auto code = table_->job_cancel(self_, handle.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::kernel::kernel_error>(code));
        }
        return {};
    }

    /// Block until the job's callback returns. Must not be called from a job
    /// callback (deadlock).
    [[nodiscard]] auto job_wait(::refrain::host::kernel::job handle) const -> std::expected<void, ::refrain::host::kernel::kernel_error> {
        if (const auto code = table_->job_wait(self_, handle.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::kernel::kernel_error>(code));
        }
        return {};
    }

    /// Subscribe to `event_type` (an application-defined 64-bit id). Returns a
    /// handle released by `event_unsubscribe`.
    [[nodiscard]] auto event_subscribe(std::uint64_t event_type, ::refrain::host::kernel::event_callback callback, void * userdata) const -> std::expected<::refrain::host::kernel::subscription, ::refrain::host::kernel::kernel_error> {
        ::refrain_host_kernel_handle_subscription value{};
        if (const auto code = table_->event_subscribe(self_, event_type, static_cast<refrain_host_kernel_event_callback>(callback), userdata, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::kernel::kernel_error>(code));
        }
        return ::refrain::host::kernel::subscription{value};
    }

    [[nodiscard]] auto event_unsubscribe(::refrain::host::kernel::subscription handle) const -> std::expected<void, ::refrain::host::kernel::kernel_error> {
        if (const auto code = table_->event_unsubscribe(self_, handle.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::kernel::kernel_error>(code));
        }
        return {};
    }

    /// Publish to every current subscriber on the calling thread. The payload
    /// pointer is borrowed for the duration of the call.
    auto event_publish(std::uint64_t event_type, void * payload, std::uint64_t size) const -> void {
        table_->event_publish(self_, event_type, payload, size);
    }

  private:
    const ::refrain_host_kernel_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::host::kernel

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_KERNEL_HPP
