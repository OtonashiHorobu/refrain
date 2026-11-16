// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/dynamic_library.hpp>

#include <optional>
#include <string>
#include <string_view>

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

/// Load every dynamic module this binary was built with and register its
/// factory in the process-wide static mod registry. Must run before the first
/// composition load (the registry is frozen by the host at that point).
///
/// Idempotent. A missing module file, a descriptor that does not match the
/// build profile or a registration conflict is an error that names the module
/// and the profile.
[[nodiscard]] auto register_dynamic_modules() -> status;

/// Directory the loader expects the modules in: `<executable dir>/modules`.
[[nodiscard]] auto dynamic_module_directory() -> result<std::string>;

} // namespace refrain::app
