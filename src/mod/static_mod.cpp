// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/static_mod.hpp>

#include <refrain/core/semver.hpp>

#include <algorithm>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace refrain::mod {

namespace {

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

} // namespace

auto static_mod_registry::global() noexcept -> static_mod_registry & {
    static static_mod_registry instance{};
    return instance;
}

auto static_mod_registry::add(const static_mod_descriptor &descriptor)
    -> status {
    const auto reject = [this](error failure) -> status {
        // Registration happens in static initializers, where returning an
        // error to the caller is not an option: remember the first failure so
        // freeze() (host startup) reports it deterministically.
        if (!has_deferred_error_) {
            deferred_error_ = failure;
            has_deferred_error_ = true;
        }
        return std::unexpected{std::move(failure)};
    };

    if (frozen_) {
        return reject(fail(
            error_code::permission_denied,
            fmt::format("static mod '{}' registered after the registry was "
                        "frozen",
                        descriptor.id)));
    }
    if (!is_valid_mod_id(descriptor.id)) {
        return reject(
            fail(error_code::invalid_argument,
                 fmt::format("static mod registration has an invalid id '{}'",
                             descriptor.id)));
    }
    if (!core::semver::parse(descriptor.version)) {
        return reject(fail(
            error_code::invalid_argument,
            fmt::format("static mod '{}' has an invalid version '{}' (expected "
                        "SemVer major.minor.patch)",
                        descriptor.id, descriptor.version)));
    }
    if (descriptor.abi == 0) {
        return reject(fail(error_code::invalid_argument,
                           fmt::format("static mod '{}' has abi 0 (expected a "
                                       "positive ABI major)",
                                       descriptor.id)));
    }
    const auto duplicate = std::find_if(
        mods_.begin(), mods_.end(),
        [&descriptor](const auto &mod) { return mod.id == descriptor.id; });
    if (duplicate != mods_.end()) {
        return reject(
            fail(error_code::already_exists,
                 fmt::format("static mod '{}' is registered more than once",
                             descriptor.id)));
    }
    mods_.push_back(descriptor);
    return status_ok();
}

auto static_mod_registry::find(std::string_view id) const noexcept
    -> const static_mod_descriptor * {
    const auto match =
        std::find_if(mods_.begin(), mods_.end(),
                     [id](const auto &mod) { return mod.id == id; });
    return match == mods_.end() ? nullptr : &*match;
}

auto static_mod_registry::freeze() -> status {
    // Freeze first: a failed freeze still seals the registry (the host aborts
    // startup after reporting), otherwise late registrations could sneak in
    // after a duplicate-id error.
    frozen_ = true;
    if (has_deferred_error_) {
        return std::unexpected{deferred_error_};
    }
    return status_ok();
}

auto static_mod_registry::match(const mod_manifest &manifest) const
    -> result<const static_mod_descriptor *> {
    const auto *descriptor{find(manifest.common.id)};
    if (descriptor == nullptr) {
        // Content-only / external mod: no linked-in code to pair with.
        return descriptor;
    }
    if (descriptor->version != manifest.common.version) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("static mod '{}' was built at version {} but "
                        "manifest.toml declares {} (REFR_MOD_STATIC drift)",
                        manifest.common.id, descriptor->version,
                        manifest.common.version))};
    }
    if (descriptor->abi != manifest.common.abi) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format(
                "static mod '{}' was built for abi {} but manifest.toml "
                "declares abi {}",
                manifest.common.id, descriptor->abi, manifest.common.abi))};
    }
    return descriptor;
}

void static_mod_registry::clear() noexcept {
    mods_.clear();
    deferred_error_ = error{};
    has_deferred_error_ = false;
    frozen_ = false;
}

} // namespace refrain::mod
