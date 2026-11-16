// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/services/world_migration_abi.h>
#include <refrain/core/error.hpp>

#include <string>
#include <string_view>

namespace refrain::server {

/// Active `refrain:world.migration` provider, as collected from a composition.
/// A default-constructed handle means the composition has no migration
/// provider.
struct world_migration_handle {
    const refrain_services_world_migration_vtable *vtable{nullptr};
    void *self{nullptr};
    /// Provider id, for the operator-facing log and error lines.
    std::string provider{};
};

/// Data version preflight for the world open path.
///
/// A world records the game data version its records were written with; the
/// storage model, chunk model and generator interpret one version. Before the
/// runtime opens an RWF world (`meta.rfw` present) this checks the recorded
/// version:
///
/// * a world already at the target version opens unchanged;
/// * an older world is upgraded through `migration` and re-checked;
/// * a version outside the provider's window fails closed with the recorded
///   version and the supported window in the message;
/// * without a migration provider an older world fails closed and the message
///   names the missing provider, so disabling the migration mod leaves such a
///   world untouched and unopened.
///
/// A directory without `meta.rfw` is not an RWF world; this preflight leaves it
/// to the port that owns its format.
[[nodiscard]] auto prepare_world_open(std::string_view world_path,
                                      const world_migration_handle &migration)
    -> status;

} // namespace refrain::server
