// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/kernel/manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace refrain::mod {
class host;
} // namespace refrain::mod

namespace refrain::kernel {

// Static (internal) mod registry.
//
// Internal mods are first-party mods whose native code is linked directly into
// the host binary: no dlopen, no `.rfmod`, no network. The built-in RFSI image
// (`builtin.image`) carries their manifests/data/assets; this registry pairs
// those manifests with the linked-in code so the host can enforce the same
// lifecycle/ABI/version checks as for external mods.
//
// Registration happens during static initialization through REFR_MOD_STATIC().
// The host freezes the registry before loading (freeze() reports deferred
// registration errors such as duplicate ids), then walks the resolved load
// order and, for each internal mod, calls its factory (if any). Content-only
// internal mods have no code and are simply not registered here; their manifest
// is still validated from the built-in archive.

// Host-side mod loading context (implemented by `refrain::mod::host`). Only
// forward-declared above: kernel does not depend on the mod layer.
// Factories receive it to register providers/listeners during load.
/// Entry point of a statically linked mod. Returning an error aborts startup
/// with that error; factories run once, in resolved load order.
using static_mod_factory = auto (*)(::refrain::mod::host &host) -> status;

/// Registration record for one statically linked mod. `id`/`version` must point
/// to static storage (string literals); the registry stores string_views.
struct static_mod_descriptor {
    std::string_view id{};
    std::string_view version{};
    std::uint32_t abi{0};
    static_mod_factory factory{nullptr}; ///< nullptr for data-only mods
};

/// Process-wide registry populated by REFR_MOD_STATIC() registrars.
///
/// Thread-safety: registration is only legal during static initialization (the
/// loader may freeze the registry afterwards); after freeze() the registry is
/// read-only and safe for concurrent lookup.
class static_mod_registry {
  public:
    /// Process-wide instance (Meyers singleton: safe from static initializers).
    [[nodiscard]] static auto global() noexcept -> static_mod_registry &;

    static_mod_registry(const static_mod_registry &) = delete;
    auto operator=(const static_mod_registry &)
        -> static_mod_registry & = delete;
    static_mod_registry(static_mod_registry &&) = delete;
    auto operator=(static_mod_registry &&) -> static_mod_registry & = delete;
    ~static_mod_registry() = default;

    /// Add a descriptor. After freeze() fails with `permission_denied`; a
    /// duplicate id fails with `already_exists`; invalid id/version/abi fails
    /// with `invalid_argument`.
    auto add(const static_mod_descriptor &descriptor) -> status;

    /// Descriptor for `id`, or nullptr when no static code is registered.
    [[nodiscard]] auto find(std::string_view id) const noexcept
        -> const static_mod_descriptor *;

    /// Registered descriptors in registration order (deterministic per link
    /// order; the host still loads in gamepack load order).
    [[nodiscard]] auto all() const noexcept
        -> std::span<const static_mod_descriptor> {
        return mods_;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return mods_.size();
    }

    [[nodiscard]] auto is_frozen() const noexcept -> bool { return frozen_; }

    /// Freeze the registry and report any registration error deferred during
    /// static initialization (duplicate/ABI-invalid descriptors are recorded
    /// instead of terminating the process from a static constructor).
    auto freeze() -> status;

    /// Match a manifest against registered static code:
    ///  * nullptr when no static code exists for this id (content-only mod);
    ///  * `version_mismatch` when the registered version/ABI differs from the
    ///    manifest (drift guard: macro constants must match manifest.toml);
    ///  * the descriptor on success.
    [[nodiscard]] auto match(const mod_manifest &manifest) const
        -> result<const static_mod_descriptor *>;

    /// Clear all registrations and deferred errors (test/re-init only).
    void clear() noexcept;

    /// Isolated registry instance for tests/embedding (the host uses
    /// `global()`). Same behavior as the process-wide registry.
    static_mod_registry() = default;

  private:
    std::vector<static_mod_descriptor> mods_{};
    error deferred_error_{};
    bool has_deferred_error_{false};
    bool frozen_{false};
};

/// Register one statically linked mod during static initialization.
///
///   REFR_MOD_STATIC("refrain.data.vanilla", "1.0.0", 1, make_data_vanilla)
///
/// `id` and `version` must be string literals (static storage). The macro may
/// be used at namespace scope in a mod source file; several mods per binary are
/// supported. Registration failures are deferred to static_mod_registry::
/// freeze(), so a bad descriptor surfaces at host startup instead of during
/// static initialization.
#define REFR_MOD_STATIC_CONCAT_(lhs, rhs) lhs##rhs
#define REFR_MOD_STATIC_CONCAT(lhs, rhs) REFR_MOD_STATIC_CONCAT_(lhs, rhs)
#define REFR_MOD_STATIC(id_literal, version_literal, abi_value, factory_fn)    \
    namespace {                                                                \
    struct REFR_MOD_STATIC_CONCAT(refr_static_mod_registrar_, __LINE__) {      \
        REFR_MOD_STATIC_CONCAT(refr_static_mod_registrar_, __LINE__)() {       \
            static_cast<void>(                                                 \
                ::refrain::kernel::static_mod_registry::global().add(          \
                    ::refrain::kernel::static_mod_descriptor{                  \
                        .id = (id_literal),                                    \
                        .version = (version_literal),                          \
                        .abi = static_cast<std::uint32_t>(abi_value),          \
                        .factory = (factory_fn)}));                            \
        }                                                                      \
    } REFR_MOD_STATIC_CONCAT(refr_static_mod_registrar_instance_, __LINE__);   \
    }

} // namespace refrain::kernel
