// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/core/version.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/dynamic_library.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::app {

// Native loading of first-party mods built as shared modules.
//
// A profile's `dynamic` list builds the same mod sources as a shared object
// under `<executable dir>/modules` instead of linking them into the host. The
// loader below opens every module this binary was built with, validates the
// exported descriptor against the build-time profile and adds it to the static
// mod registry. From that point on the composition loader treats the module
// exactly like a statically linked mod: the factory runs in resolved load order
// and registers its providers through the normal host API.
//
// The modules stay mapped for the process lifetime (they are part of the
// binary's composition, not a hot-installed package), so there is no unload
// path here.

/// One module opened and validated by `load_dynamic_module()`.
struct loaded_dynamic_module {
    std::string id{};
    std::string path{};
    mod::dynamic_module_descriptor descriptor{};
    std::optional<refrain::platform::dynamic_library> library{};

    [[nodiscard]] auto valid() const noexcept -> bool {
        return descriptor.factory != nullptr && !id.empty();
    }
};

/// Open one module file, call its entry point and validate the descriptor
/// (`id`, SemVer version, positive ABI and a factory). Does not register it.
[[nodiscard]] auto load_dynamic_module(std::string_view path)
    -> result<loaded_dynamic_module>;

/// Loaded modules, owner of the mappings their factories live in.
using loaded_dynamic_modules =
    std::vector<std::unique_ptr<loaded_dynamic_module>>;

/// Opens one module file. Production uses `load_dynamic_module()`; a caller
/// that already has the module descriptors (a test, an embedder) can
/// substitute a loader and drive the registration without shared objects.
using dynamic_module_loader =
    std::function<result<loaded_dynamic_module>(std::string_view path)>;

/// Two-phase registration of an explicit module list, in the order given.
///
/// Phase one opens and validates every record and rejects a duplicate id in
/// the list or an id the registry already holds. Phase two registers the
/// complete set. A failure in phase one leaves `registry` untouched and
/// `loaded` empty: the modules are closed and no factory that pointed into
/// them was ever added. That is the invariant a partial load must not break,
/// because the owning libraries cannot be unloaded while a registry entry
/// still names their factory.
///
/// `loaded` owns the modules after the call and must stay alive for as long
/// as the registry entries are: it is the mapping the factories live in. A
/// phase-two failure can only come from an internal invariant violation; the
/// entries already added cannot be removed, so the corresponding modules stay
/// in `loaded` and the caller keeps them mapped instead of unloading them.
[[nodiscard]] auto load_dynamic_module_list(
    mod::static_mod_registry &registry,
    std::span<const version::dynamic_module_record> records,
    std::string_view directory, const dynamic_module_loader &load,
    loaded_dynamic_modules &loaded) -> status;

/// Load every dynamic module this binary was built with and register its
/// factory in the process-wide static mod registry. Must run before the first
/// composition load (the registry is frozen by the host at that point).
///
/// Idempotent. A missing module file, a descriptor that does not match the
/// build profile or a registration conflict is an error that names the module
/// and the profile, and leaves the registry without any entry from a module
/// file the call could not keep mapped.
[[nodiscard]] auto register_dynamic_modules() -> status;

/// Directory the loader expects the modules in: `<executable dir>/modules`.
[[nodiscard]] auto dynamic_module_directory() -> result<std::string>;

} // namespace refrain::app
