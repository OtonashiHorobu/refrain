// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:world.migration` provider for `refrain.world.datafix`.
//
// The provider is a thin contract adapter over the in-process migration engine
// in `refrain/world/migration.hpp`. The engine reads and writes the world
// directory itself: it runs before a storage provider opens the world, and its
// window and version stamps are what make a save of another data version a
// supported input instead of a misread one.
//
// Across the ABI only status codes travel, so the failure detail (the path, the
// chunk coordinate, the recorded version) is logged by the engine adapter here
// before the code is returned to the open path.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/world_migration.hpp>

#include <refrain/core/error.hpp>
#include <refrain/core/log.hpp>
#include <refrain/host/abi_bridge.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/world/migration.hpp>
#include <refrain/world/version.hpp>

#include <cstdint>
#include <memory>

namespace {

namespace abi = refrain::host::abi;
namespace migration = refrain::services::world_migration;
namespace world = refrain::world;

constexpr refrain::log::logger log_datafix{"world-datafix"};

/// Status rows for the failure classes the contract distinguishes; every other
/// error is an internal failure.
constexpr abi::status_entry migration_statuses[]{
    {refrain::error_code::unsupported,
     REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_UNSUPPORTED_VERSION},
    {refrain::error_code::invalid_argument,
     REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_INVALID_REQUEST},
    {refrain::error_code::not_found,
     REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_IO_ERROR},
    {refrain::error_code::io_error,
     REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_IO_ERROR},
    {refrain::error_code::parse_error,
     REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_CORRUPT_WORLD},
    {refrain::error_code::version_mismatch,
     REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_CORRUPT_WORLD},
};

struct world_migration_provider {
    static auto capabilities(
        void *self,
        refrain_services_world_migration_world_migration_capabilities *out)
        -> refrain_status {
        if (self == nullptr || out == nullptr ||
            out->struct_size !=
                REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_CAPABILITIES_SIZE) {
            return REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_INVALID_REQUEST;
        }
        out->min_version = world::oldest_migratable_data_version;
        out->target_version = world::target_data_version;
        return REFRAIN_STATUS_OK;
    }

    static auto plan(void *self, std::int32_t from_version,
                     std::int32_t to_version,
                     refrain_services_world_migration_world_migration_plan *out)
        -> refrain_status {
        if (self == nullptr || out == nullptr ||
            out->struct_size !=
                REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_PLAN_SIZE) {
            return REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_INVALID_REQUEST;
        }
        auto planned{world::plan_data_migration(from_version, to_version)};
        if (!planned) {
            log_datafix.debug("refused to plan a migration from data version "
                              "{} to {}: {}",
                              from_version, to_version,
                              planned.error().message);
            return abi::to_status(
                planned.error(), migration_statuses,
                REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_MIGRATION_FAILED);
        }
        out->from_version = planned->from_version;
        out->to_version = planned->to_version;
        out->steps = planned->steps;
        return REFRAIN_STATUS_OK;
    }

    static auto migrate(
        void *self,
        const refrain_services_world_migration_world_migration_request *request,
        refrain_services_world_migration_world_migration_report *out)
        -> refrain_status {
        if (self == nullptr || request == nullptr || out == nullptr) {
            return REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_INVALID_REQUEST;
        }
        if (request->struct_size !=
                REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_REQUEST_SIZE ||
            out->struct_size !=
                REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_REPORT_SIZE) {
            return REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_INVALID_REQUEST;
        }
        const auto path{abi::to_view(request->world_path)};
        if (path.empty()) {
            return REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_INVALID_REQUEST;
        }
        auto report{world::migrate_world_data(
            path, request->from_version, request->to_version,
            world::data_migration_options{
                .dry_run = request->dry_run != 0,
            })};
        if (!report) {
            log_datafix.error("migrating world '{}' from data version {} to "
                              "{} failed: {}",
                              path, request->from_version, request->to_version,
                              report.error().message);
            return abi::to_status(
                report.error(), migration_statuses,
                REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_MIGRATION_FAILED);
        }
        out->from_version = report->from_version;
        out->to_version = report->to_version;
        out->steps = report->steps;
        out->chunks_scanned = report->chunks_scanned;
        out->chunks_migrated = report->chunks_migrated;
        out->player_records_scanned = report->player_records_scanned;
        out->player_records_migrated = report->player_records_migrated;
        out->no_op = report->no_op ? 1 : 0;
        out->dry_run = report->dry_run ? 1 : 0;
        return REFRAIN_STATUS_OK;
    }
};

auto migration_vtable() -> const refrain_services_world_migration_vtable & {
    static const refrain_services_world_migration_vtable table{
        .struct_size = REFRAIN_SERVICES_WORLD_MIGRATION_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_WORLD_MIGRATION_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_WORLD_MIGRATION_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_WORLD_MIGRATION_ABI_PATCH,
        .capabilities = &world_migration_provider::capabilities,
        .plan = &world_migration_provider::plan,
        .migrate = &world_migration_provider::migrate,
    };
    return table;
}

auto make_world_datafix(refrain::mod::host &host) -> refrain::status {
    static std::unique_ptr<world_migration_provider> provider{};
    provider = std::make_unique<world_migration_provider>();
    const refrain::mod::provider_api api{
        .vtable = &migration_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_WORLD_MIGRATION_ABI_MAJOR,
    };
    auto status{host.register_provider(migration::service_id, api, "1.0.0")};
    if (!status) {
        provider.reset();
        return status;
    }
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.world.datafix", "1.0.0", 1, make_world_datafix);
