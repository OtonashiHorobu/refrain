// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 29871d2bb99d3b05

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_CORE_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_CORE_HPP

#include <refrain/contracts/host/core_abi.h>
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

namespace refrain::host::core {

enum class host_error : std::uint32_t { // enum 'host_error'
    invalid_argument = 1,
    not_found = 2,
    permission_denied = 3,
    version_mismatch = 4,
    unsupported = 5,
    not_initialized = 6,
    out_of_memory = 7,
    limit_exceeded = 8,
    thread_violation = 9,
    internal = 10,
    already_exists = 11,
    io_error = 12,
    /// The cached provider generation is no longer committed: the service
    /// instance was replaced, retired or is quiescing. The caller must
    /// re-query instead of calling the table it holds.
    stale = 13,
};

using host_version = ::refrain_host_core_host_version;
/// Zero-initialized `host_version` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_host_version() noexcept -> host_version {
    host_version value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// A service provider table plus its instance. `vtable` and `self` are
/// opaque: the consumer casts them to the generated vtable of the requested
/// service. The host validates the vtable header before returning a ref.
/// 
/// A `provider_ref` carries no composition state. It stays usable only for a
/// sealed/process-lifetime provider, or while the caller knows the queried
/// slot has not been replaced; `query_service_handle` wraps it with the
/// generation that answers that question at a tick boundary.
using provider_ref = ::refrain_host_core_provider_ref;
/// Zero-initialized `provider_ref` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_provider_ref() noexcept -> provider_ref {
    provider_ref value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// A provider table together with the composition state it was read from.
/// 
/// `generation` is the committed generation of the `(service, instance)` slot
/// that produced `provider`. A non-zero generation is the value a caller
/// hands back to `validate_handle` when it crosses a tick boundary or a
/// composition epoch; zero identifies a sealed, process-lifetime provider
/// table, which never changes and needs no re-query. `epoch` is the committed
/// composition epoch at query time, for diagnostics and audit.
using provider_handle = ::refrain_host_core_provider_handle;
/// Zero-initialized `provider_handle` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_provider_handle() noexcept -> provider_handle {
    provider_handle value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(host_version) == 16);
static_assert(alignof(host_version) == 4);
static_assert(offsetof(host_version, struct_size) == 0);
static_assert(offsetof(host_version, major) == 4);
static_assert(offsetof(host_version, minor) == 8);
static_assert(offsetof(host_version, patch) == 12);
static_assert(sizeof(provider_ref) == 32);
static_assert(alignof(provider_ref) == 8);
static_assert(offsetof(provider_ref, struct_size) == 0);
static_assert(offsetof(provider_ref, vtable) == 8);
static_assert(offsetof(provider_ref, self) == 16);
static_assert(offsetof(provider_ref, abi_major) == 24);
static_assert(offsetof(provider_ref, reserved) == 28);
static_assert(sizeof(provider_handle) == 56);
static_assert(alignof(provider_handle) == 8);
static_assert(offsetof(provider_handle, struct_size) == 0);
static_assert(offsetof(provider_handle, provider) == 8);
static_assert(offsetof(provider_handle, generation) == 40);
static_assert(offsetof(provider_handle, epoch) == 48);

inline constexpr std::string_view service_id = "refrain:host.core";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 1;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::both;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::sealed;
inline constexpr std::string_view capabilities = "log,config,services,clock,alloc";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:host.core"};

/// Type-safe wrapper around `refrain_host_core_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_host_core_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_host_core_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    auto version(::refrain::host::core::host_version &value) const -> void {
        ::refrain::contracts::init_struct(value);
        table_->version(self_, &value);
    }

    auto log(std::uint32_t level, std::string_view message) const -> void {
        table_->log(self_, level, ::refrain::contracts::to_borrowed(message));
    }

    [[nodiscard]] auto now_ns() const -> std::int64_t {
        return static_cast<std::int64_t>(table_->now_ns(self_));
    }

    [[nodiscard]] auto tick_now() const -> std::int64_t {
        return static_cast<std::int64_t>(table_->tick_now(self_));
    }

    /// Read a value from the named config namespace. Namespaces are mounted by
    /// the host composition root (`host`, `network`, `mod/<id>`, ...); a
    /// native mod may read any mounted namespace, while a sandboxed guest is
    /// restricted by the host import layer. The string is borrowed until the
    /// next call on the same thread.
    [[nodiscard]] auto config_get_i64(std::string_view ns, std::string_view key) const -> std::expected<std::int64_t, ::refrain::host::core::host_error> {
        ::int64_t value{};
        if (const auto code = table_->config_get_i64(self_, ::refrain::contracts::to_borrowed(ns), ::refrain::contracts::to_borrowed(key), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return static_cast<std::int64_t>(value);
    }

    [[nodiscard]] auto config_get_bool(std::string_view ns, std::string_view key) const -> std::expected<bool, ::refrain::host::core::host_error> {
        ::uint8_t value{};
        if (const auto code = table_->config_get_bool(self_, ::refrain::contracts::to_borrowed(ns), ::refrain::contracts::to_borrowed(key), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return (value != 0);
    }

    [[nodiscard]] auto config_get_string(std::string_view ns, std::string_view key) const -> std::expected<std::string_view, ::refrain::host::core::host_error> {
        ::refrain_str value{};
        if (const auto code = table_->config_get_string(self_, ::refrain::contracts::to_borrowed(ns), ::refrain::contracts::to_borrowed(key), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return ::refrain::contracts::as_string_view(value);
    }

    /// Look up a service provider without composition state. `instance` is
    /// empty for the default instance.
    /// 
    /// This is the legacy/process-lifetime form: the returned table is the
    /// provider committed at the call, and nothing in the result tells the
    /// caller whether a later composition epoch replaced it. Only a sealed
    /// host table (which never changes) and a caller that never crosses a
    /// composition boundary may keep the result. Use `query_service_handle`
    /// for an ordinary service the caller caches across ticks.
    /// 
    /// Errors: `not_found` when the instance has no active provider, `stale`
    /// while the instance is quiescing for a staged replacement.
    [[nodiscard]] auto query_service(std::string_view service, std::string_view instance) const -> std::expected<::refrain::host::core::provider_ref, ::refrain::host::core::host_error> {
        ::refrain_host_core_provider_ref value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->query_service(self_, ::refrain::contracts::to_borrowed(service), ::refrain::contracts::to_borrowed(instance), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return value;
    }

    /// Register the calling mod's provider during load. The host checks the
    /// manifest `[provides]`, the gamepack plan and the vtable ABI header.
    /// Sealed services are rejected here; they are registered by the host.
    [[nodiscard]] auto register_provider(std::string_view service, std::string_view instance, const ::refrain::host::core::provider_ref &ref) const -> std::expected<void, ::refrain::host::core::host_error> {
        const auto abi_ref = ::refrain::contracts::with_struct_size(ref);
        if (const auto code = table_->register_provider(self_, ::refrain::contracts::to_borrowed(service), ::refrain::contracts::to_borrowed(instance), &abi_ref); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return {};
    }

    /// Allocate a host-owned block. The caller releases it with `free`.
    [[nodiscard]] auto alloc(std::uint64_t size, std::uint64_t alignment) const -> std::expected<std::span<std::uint8_t>, ::refrain::host::core::host_error> {
        ::refrain_mut_bytes value{};
        if (const auto code = table_->alloc(self_, size, alignment, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return std::span<std::uint8_t>{value.data, static_cast<std::size_t>(value.len)};
    }

    auto free(std::span<std::uint8_t> block) const -> void {
        table_->free(self_, ::refrain::contracts::to_mut(block));
    }

    /// Thread-local diagnostic for the last failed host call on this thread.
    [[nodiscard]] auto last_error_message() const -> std::expected<std::string_view, ::refrain::host::core::host_error> {
        ::refrain_str value{};
        if (const auto code = table_->last_error_message(self_, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return ::refrain::contracts::as_string_view(value);
    }

    /// Look up a service provider together with the generation it was read
    /// from. `instance` is empty for the default instance.
    /// 
    /// A sealed host table is returned with `generation == 0`: it is
    /// process-lifetime and never needs validation. Any other service returns
    /// the generation of its committed slot; a caller caches the handle for
    /// one tick and either calls `validate_handle` at the next tick boundary or
    /// re-queries around a composition epoch.
    /// 
    /// Errors: `not_found` when the instance has no active provider (never
    /// active or retired), `stale` while the instance is quiescing for a
    /// staged replacement.
    [[nodiscard]] auto query_service_handle(std::string_view service, std::string_view instance) const -> std::expected<::refrain::host::core::provider_handle, ::refrain::host::core::host_error> {
        ::refrain_host_core_provider_handle value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->query_service_handle(self_, ::refrain::contracts::to_borrowed(service), ::refrain::contracts::to_borrowed(instance), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return value;
    }

    /// Check a cached provider generation against the committed composition.
    /// 
    /// Success means `generation` is the current committed generation, or that
    /// `service` is a sealed host table and `generation` is zero. `stale`
    /// means the slot was replaced, retired or is quiescing: the caller must
    /// re-query and use the new table instead of the one it cached. The
    /// generation identifies a slot of one service instance, so the caller
    /// passes the same `service` and `instance` it queried with.
    [[nodiscard]] auto validate_handle(std::string_view service, std::string_view instance, std::uint64_t generation) const -> std::expected<void, ::refrain::host::core::host_error> {
        if (const auto code = table_->validate_handle(self_, ::refrain::contracts::to_borrowed(service), ::refrain::contracts::to_borrowed(instance), generation); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::core::host_error>(code));
        }
        return {};
    }

  private:
    const ::refrain_host_core_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::host::core

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_CORE_HPP
