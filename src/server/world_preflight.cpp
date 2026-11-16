// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/world_preflight.hpp>

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/world_migration.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/world/store.hpp>
#include <refrain/world/version.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

constexpr refrain::log::logger log_world{"world"};

namespace migration = refrain::services::world_migration;
namespace world = refrain::world;

[[nodiscard]] auto to_error_code(migration::world_migration_error code)
    -> refrain::error_code {
    using enum migration::world_migration_error;
    switch (code) {
    case unsupported_version:
        return refrain::error_code::unsupported;
    case invalid_request:
        return refrain::error_code::invalid_argument;
    case io_error:
        return refrain::error_code::io_error;
    case corrupt_world:
        return refrain::error_code::parse_error;
    case migration_failed:
        return refrain::error_code::io_error;
    }
    return refrain::error_code::unknown;
}

[[nodiscard]] auto to_text(migration::world_migration_error code)
    -> std::string_view {
    using enum migration::world_migration_error;
    switch (code) {
    case unsupported_version:
        return "unsupported data version";
    case invalid_request:
        return "invalid migration request";
    case io_error:
        return "world read or write failed";
    case corrupt_world:
        return "the world is not a readable save of the recorded version";
    case migration_failed:
        return "a migration step failed";
    }
    return "unknown migration failure";
}

} // namespace

namespace refrain::server {

auto prepare_world_open(std::string_view world_path,
                        const world_migration_handle &migration) -> status {
    if (world_path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "world path is empty")};
    }
    const std::string base{world_path};
    if (!core::io::exists(fmt::format("{}/meta.rfw", base))) {
        // Not an RWF world: its port owns the format and any upgrade.
        return status_ok();
    }
    auto recorded{world::read_world_data_version(base)};
    if (!recorded) {
        return std::unexpected{recorded.error()};
    }
    const auto target{world::target_data_version};
    if (*recorded == target) {
        return status_ok();
    }
    if (migration.vtable == nullptr || migration.self == nullptr) {
        return std::unexpected{make_error(
            error_code::unsupported,
            fmt::format(
                "world '{}' records data version {} but this build reads {}; "
                "no world migration provider is active, so enable the built-in "
                "'refrain.world.datafix' mod or upgrade the world in "
                "Minecraft 26.3 before opening it",
                base, *recorded, target))};
    }

    const migration::provider provider{*migration.vtable, migration.self};
    auto capabilities{provider.capabilities()};
    if (!capabilities) {
        return std::unexpected{make_error(
            to_error_code(capabilities.error()),
            fmt::format("world '{}': migration provider '{}' failed to report "
                        "its data version window ({})",
                        base, migration.provider,
                        to_text(capabilities.error())))};
    }
    const auto window_min{capabilities->min_version};
    const auto window_target{capabilities->target_version};
    if (window_target != target) {
        return std::unexpected{make_error(
            error_code::unsupported,
            fmt::format("world '{}' records data version {}; migration "
                        "provider '{}' upgrades to data version {} but this "
                        "build reads {}",
                        base, *recorded, migration.provider, window_target,
                        target))};
    }
    auto planned{provider.plan(*recorded, target)};
    if (!planned) {
        return std::unexpected{make_error(
            to_error_code(planned.error()),
            fmt::format("world '{}' records data version {}, outside the "
                        "source window [{}, {}] the active migration provider "
                        "'{}' supports; upgrade the world in Minecraft 26.3 "
                        "before opening it here",
                        base, *recorded, window_min, window_target,
                        migration.provider))};
    }

    log_world.info(
        "migrating world '{}' from data version {} to {} with {} step(s)", base,
        *recorded, target, planned->steps);
    auto request{migration::make_world_migration_request()};
    request.world_path = contracts::to_borrowed(std::string_view{base});
    request.from_version = *recorded;
    request.to_version = target;
    request.dry_run = 0;
    auto report{provider.migrate(request)};
    if (!report) {
        return std::unexpected{make_error(
            to_error_code(report.error()),
            fmt::format("world '{}' migration from data version {} to {} "
                        "failed: {}",
                        base, *recorded, target, to_text(report.error())))};
    }
    if (report->no_op) {
        log_world.info("world '{}' already recorded data version {}", base,
                       target);
    } else {
        log_world.info(
            "migrated world '{}' from data version {} to {} ({} chunks, {} "
            "player records, {} steps)",
            base, report->from_version, report->to_version,
            report->chunks_migrated, report->player_records_migrated,
            report->steps);
    }

    // Re-read the stamp from disk: the migration is only complete when the
    // world itself records the version this build interprets.
    auto stored{world::read_world_data_version(base)};
    if (!stored) {
        return std::unexpected{stored.error()};
    }
    if (*stored != target) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("world '{}' still records data version {} after "
                        "migration; refusing to open it",
                        base, *stored))};
    }
    return status_ok();
}

} // namespace refrain::server
