// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 77d3b54f96579605

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_MOD_LOADER_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_MOD_LOADER_HPP

#include <refrain/contracts/services/mod_loader_abi.h>
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

namespace refrain::services::mod_loader {

/// How a loaded mod can be removed. The host records the declaration and the
/// lifecycle layer never assumes a stronger guarantee than this.
enum class mod_loader_unload : std::uint8_t { // enum 'mod_loader_unload'
    /// Guest state and host registrations are released; the mod can be
    /// unloaded and loaded again in place.
    clean = 0,
    /// No new calls are dispatched, but resources stay allocated until the
    /// process exits.
    retire = 1,
    /// The process must restart; nothing is released while it runs.
    restart_required = 2,
};

/// Loader failures. Zero is success, so entries start at 1.
enum class mod_loader_error : std::uint32_t { // enum 'mod_loader_error'
    invalid_argument = 1,
    not_found = 2,
    already_exists = 3,
    permission_denied = 4,
    io_error = 5,
    unsupported = 6,
    version_mismatch = 7,
    /// The loader refused an operation its sandbox policy forbids, for example
    /// a guest asking to register a provider.
    sandbox_violation = 8,
    internal = 9,
};

/// Why the host is asking a loader to unload one instance.
enum class mod_unload_reason : std::uint32_t { // enum 'mod_unload_reason'
    /// The host is shutting down.
    shutdown = 0,
    /// An explicit administrative request.
    explicit_request = 1,
    /// The instance is being replaced by a reload.
    reload = 2,
    /// The instance is being removed because its package was uninstalled.
    uninstall = 3,
};

namespace detail {
struct mod_instance_tag;
} // namespace detail

/// Opaque handle for one mod instance a loader owns. Zero is invalid.
using mod_instance = ::refrain::contracts::handle<detail::mod_instance_tag>;

/// What a loader can do. The host validates the declarations before it
/// dispatches anything through the loader, and a provider that changes them
/// while running fails the next load instead of silently reconfiguring.
using mod_loader_capabilities = ::refrain_services_mod_loader_mod_loader_capabilities;
/// Zero-initialized `mod_loader_capabilities` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_mod_loader_capabilities() noexcept -> mod_loader_capabilities {
    mod_loader_capabilities value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Manifest subset the host hands to a loader. Strings are borrowed for the
/// duration of the call.
using mod_entry_manifest = ::refrain_services_mod_loader_mod_entry_manifest;
/// Zero-initialized `mod_entry_manifest` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_mod_entry_manifest() noexcept -> mod_entry_manifest {
    mod_entry_manifest value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// One load attempt for an already validated manifest. The loader owns the
/// instance from `load` until `unload` or process exit.
using mod_load_request = ::refrain_services_mod_loader_mod_load_request;
/// Zero-initialized `mod_load_request` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_mod_load_request() noexcept -> mod_load_request {
    mod_load_request value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(mod_loader_capabilities) == 32);
static_assert(alignof(mod_loader_capabilities) == 8);
static_assert(offsetof(mod_loader_capabilities, struct_size) == 0);
static_assert(offsetof(mod_loader_capabilities, loader_id) == 8);
static_assert(offsetof(mod_loader_capabilities, abi_major) == 24);
static_assert(offsetof(mod_loader_capabilities, unload) == 28);
static_assert(offsetof(mod_loader_capabilities, sandboxed) == 29);
static_assert(offsetof(mod_loader_capabilities, supports_hot_install) == 30);
static_assert(sizeof(mod_entry_manifest) == 136);
static_assert(alignof(mod_entry_manifest) == 8);
static_assert(offsetof(mod_entry_manifest, struct_size) == 0);
static_assert(offsetof(mod_entry_manifest, mod_id) == 8);
static_assert(offsetof(mod_entry_manifest, version) == 24);
static_assert(offsetof(mod_entry_manifest, abi) == 40);
static_assert(offsetof(mod_entry_manifest, side) == 44);
static_assert(offsetof(mod_entry_manifest, loader) == 48);
static_assert(offsetof(mod_entry_manifest, package_root) == 64);
static_assert(offsetof(mod_entry_manifest, native_path) == 80);
static_assert(offsetof(mod_entry_manifest, wasm_path) == 96);
static_assert(offsetof(mod_entry_manifest, symbol) == 112);
static_assert(offsetof(mod_entry_manifest, activate_code) == 128);
static_assert(sizeof(mod_load_request) == 144);
static_assert(alignof(mod_load_request) == 8);
static_assert(offsetof(mod_load_request, struct_size) == 0);
static_assert(offsetof(mod_load_request, manifest) == 8);

inline constexpr std::string_view service_id = "refrain:mod.loader";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::main;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::instanced;
inline constexpr std::string_view capabilities = "builtin,native,sandboxed,hot-install";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:mod.loader"};

/// Type-safe wrapper around `refrain_services_mod_loader_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_mod_loader_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_mod_loader_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Fill in this loader's capabilities. The host calls it once at
    /// registration and again before every dispatch to detect drift.
    auto capabilities(::refrain::services::mod_loader::mod_loader_capabilities &caps) const -> void {
        ::refrain::contracts::init_struct(caps);
        table_->capabilities(self_, &caps);
    }

    /// Check the manifest before any mod code runs. A loader rejects the entry
    /// shape it does not own; the host has already validated identity,
    /// version, side and host requirements.
    [[nodiscard]] auto validate(const ::refrain::services::mod_loader::mod_entry_manifest &entry) const -> std::expected<void, ::refrain::services::mod_loader::mod_loader_error> {
        const auto abi_entry = ::refrain::contracts::with_struct_size(entry);
        if (const auto code = table_->validate(self_, &abi_entry); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::mod_loader::mod_loader_error>(code));
        }
        return {};
    }

    /// Load one validated manifest and return the handle the host records.
    [[nodiscard]] auto load(const ::refrain::services::mod_loader::mod_load_request &request) const -> std::expected<::refrain::services::mod_loader::mod_instance, ::refrain::services::mod_loader::mod_loader_error> {
        const auto abi_request = ::refrain::contracts::with_struct_size(request);
        ::refrain_services_mod_loader_handle_mod_instance value{};
        if (const auto code = table_->load(self_, &abi_request, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::mod_loader::mod_loader_error>(code));
        }
        return ::refrain::services::mod_loader::mod_instance{value};
    }

    /// Release one instance. The loader's declared unload semantics bound what
    /// it may actually release.
    [[nodiscard]] auto unload(::refrain::services::mod_loader::mod_instance instance, ::refrain::services::mod_loader::mod_unload_reason reason) const -> std::expected<void, ::refrain::services::mod_loader::mod_loader_error> {
        if (const auto code = table_->unload(self_, instance.raw(), static_cast<refrain_services_mod_loader_mod_unload_reason>(reason)); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::mod_loader::mod_loader_error>(code));
        }
        return {};
    }

    /// Build a replacement generation for one instance and return the new
    /// handle. The old instance must stay alive and callable until the host
    /// calls `unload` for it after the composition epoch commits: a staged
    /// replacement cannot release code the running composition still calls.
    /// A loader that cannot build a replacement without releasing the old
    /// instance reports `unsupported`.
    [[nodiscard]] auto reload(::refrain::services::mod_loader::mod_instance instance, const ::refrain::services::mod_loader::mod_load_request &request) const -> std::expected<::refrain::services::mod_loader::mod_instance, ::refrain::services::mod_loader::mod_loader_error> {
        const auto abi_request = ::refrain::contracts::with_struct_size(request);
        ::refrain_services_mod_loader_handle_mod_instance value{};
        if (const auto code = table_->reload(self_, instance.raw(), &abi_request, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::mod_loader::mod_loader_error>(code));
        }
        return ::refrain::services::mod_loader::mod_instance{value};
    }

  private:
    const ::refrain_services_mod_loader_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::mod_loader

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_MOD_LOADER_HPP
