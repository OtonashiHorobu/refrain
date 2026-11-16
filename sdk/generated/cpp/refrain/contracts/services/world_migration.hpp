// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 05a109909008d815

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLD_MIGRATION_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLD_MIGRATION_HPP

#include <refrain/contracts/services/world_migration_abi.h>
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

namespace refrain::services::world_migration {

/// Failure classes the caller can act on. Every value starts at 1 so that a
/// zero status is success on the ABI.
enum class world_migration_error : std::uint32_t { // enum 'world_migration_error'
    /// The source or target version lies outside the provider's window.
    unsupported_version = 1,
    /// The request is malformed (empty path, inverted range, ...).
    invalid_request = 2,
    /// Reading or writing the world failed.
    io_error = 3,
    /// The world is not a readable save of the requested version.
    corrupt_world = 4,
    /// A migration step failed after the world was validated.
    migration_failed = 5,
};

/// Source versions one provider accepts and the version it upgrades them to.
using world_migration_capabilities = ::refrain_services_world_migration_world_migration_capabilities;
/// Zero-initialized `world_migration_capabilities` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_world_migration_capabilities() noexcept -> world_migration_capabilities {
    world_migration_capabilities value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// What a migration from `from_version` to `to_version` runs. The count covers
/// every upgrade step plus the version stamp; zero means the world already
/// records `to_version`.
using world_migration_plan = ::refrain_services_world_migration_world_migration_plan;
/// Zero-initialized `world_migration_plan` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_world_migration_plan() noexcept -> world_migration_plan {
    world_migration_plan value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// One migration request. `dry_run` validates the world and reports the work
/// that would be done without writing anything.
using world_migration_request = ::refrain_services_world_migration_world_migration_request;
/// Zero-initialized `world_migration_request` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_world_migration_request() noexcept -> world_migration_request {
    world_migration_request value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Outcome of one successful migration (or of its dry run).
using world_migration_report = ::refrain_services_world_migration_world_migration_report;
/// Zero-initialized `world_migration_report` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_world_migration_report() noexcept -> world_migration_report {
    world_migration_report value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(world_migration_capabilities) == 12);
static_assert(alignof(world_migration_capabilities) == 4);
static_assert(offsetof(world_migration_capabilities, struct_size) == 0);
static_assert(offsetof(world_migration_capabilities, min_version) == 4);
static_assert(offsetof(world_migration_capabilities, target_version) == 8);
static_assert(sizeof(world_migration_plan) == 16);
static_assert(alignof(world_migration_plan) == 4);
static_assert(offsetof(world_migration_plan, struct_size) == 0);
static_assert(offsetof(world_migration_plan, from_version) == 4);
static_assert(offsetof(world_migration_plan, to_version) == 8);
static_assert(offsetof(world_migration_plan, steps) == 12);
static_assert(sizeof(world_migration_request) == 40);
static_assert(alignof(world_migration_request) == 8);
static_assert(offsetof(world_migration_request, struct_size) == 0);
static_assert(offsetof(world_migration_request, world_path) == 8);
static_assert(offsetof(world_migration_request, from_version) == 24);
static_assert(offsetof(world_migration_request, to_version) == 28);
static_assert(offsetof(world_migration_request, dry_run) == 32);
static_assert(sizeof(world_migration_report) == 56);
static_assert(alignof(world_migration_report) == 8);
static_assert(offsetof(world_migration_report, struct_size) == 0);
static_assert(offsetof(world_migration_report, from_version) == 4);
static_assert(offsetof(world_migration_report, to_version) == 8);
static_assert(offsetof(world_migration_report, steps) == 12);
static_assert(offsetof(world_migration_report, chunks_scanned) == 16);
static_assert(offsetof(world_migration_report, chunks_migrated) == 24);
static_assert(offsetof(world_migration_report, player_records_scanned) == 32);
static_assert(offsetof(world_migration_report, player_records_migrated) == 40);
static_assert(offsetof(world_migration_report, no_op) == 48);
static_assert(offsetof(world_migration_report, dry_run) == 49);

inline constexpr std::string_view service_id = "refrain:world.migration";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "data-version,chunk-nbt,player-data";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:world.migration"};

/// Type-safe wrapper around `refrain_services_world_migration_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_world_migration_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_world_migration_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Versions this provider upgrades from and to. Callers plan only inside
    /// the returned window; a wider window needs a provider upgrade.
    [[nodiscard]] auto capabilities() const -> std::expected<::refrain::services::world_migration::world_migration_capabilities, ::refrain::services::world_migration::world_migration_error> {
        ::refrain_services_world_migration_world_migration_capabilities value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->capabilities(self_, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world_migration::world_migration_error>(code));
        }
        return value;
    }

    /// Validate that `from_version` can be upgraded to `to_version` and return
    /// the step count. Fails with `unsupported_version` when either version
    /// lies outside the window, and with `invalid_request` when `from_version`
    /// is newer than `to_version`.
    [[nodiscard]] auto plan(std::int32_t from_version, std::int32_t to_version) const -> std::expected<::refrain::services::world_migration::world_migration_plan, ::refrain::services::world_migration::world_migration_error> {
        ::refrain_services_world_migration_world_migration_plan value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->plan(self_, from_version, to_version, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world_migration::world_migration_error>(code));
        }
        return value;
    }

    /// Upgrade the world, or report the work without writing when `dry_run` is
    /// set. A world already at `to_version` succeeds as a no-op. Each rewritten
    /// record is written through a temporary file and renamed into place, and
    /// the version stamp is written last, so a failed migration leaves the
    /// world recorded at its original version.
    [[nodiscard]] auto migrate(const ::refrain::services::world_migration::world_migration_request &request) const -> std::expected<::refrain::services::world_migration::world_migration_report, ::refrain::services::world_migration::world_migration_error> {
        const auto abi_request = ::refrain::contracts::with_struct_size(request);
        ::refrain_services_world_migration_world_migration_report value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->migrate(self_, &abi_request, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world_migration::world_migration_error>(code));
        }
        return value;
    }

  private:
    const ::refrain_services_world_migration_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::world_migration

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLD_MIGRATION_HPP
