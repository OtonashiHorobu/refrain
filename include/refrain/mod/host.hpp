// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/kernel/service_registry.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {
class builtin_image;
} // namespace refrain::data

namespace refrain::mod {

namespace kernel = refrain::kernel;

// Mod host: loads a resolved `mod::composition` against the
// embedded built-in content image (RFSI v1) and the static mod registry.
//
// Load scope (internal/static mods only):
//   1. freeze the static mod registry (deferred registration errors abort);
//   2. for every mod in the gamepack load order, read its `manifest.toml` from
//      the built-in RFSI image, parse it, and enforce the same gates as an
//      external package would get: manifest present, id/version match the
//      gamepack pin, ABI == runtime ABI, declared side compatible with the
//      host, and a static factory for every internal mod the gamepack selected
//      as a provider (data-only content mods may omit code);
//   3. call the static factory of every selected provider in load order, giving
//      it this host for provider/event registration. A mod that declares
//      `[provides]` but is not selected by `composition.providers` keeps
//      loading as a data/content mod with its code inactive (fail-closed
//      composition: only planned providers may claim a service);
//   4. fail closed when a selected provider did not become active.
//
// External mods ([entry] native/wasm) are rejected with `unsupported`: dynamic
// loading is not implemented. The host is single-use: a failed
// load clears everything it assembled and a later `load()` returns
// `unsupported` (recreate the host; no partial activation is exposed).
//
// The built-in image is passed as borrowed bytes so the same host code works
// with the embedded `builtin.image` (server/tests) and with a file read by
// `refrain-tools mods load --image <path>`.

/// Opaque generated-contract API table plus its provider instance (the pair a
/// typed `refrain::services::<contract>::provider` wrapper is built from).
/// `vtable` points at the generated stable C ABI vtable; both pointers must
/// stay valid for the process lifetime (static storage / host-owned).
struct provider_api {
    const void *vtable{nullptr};
    void *self{nullptr};
    /// ABI major the vtable was built against. `0` skips the header check
    /// (factories written before the field existed); any other value makes
    /// `register_provider()` verify that `vtable` starts with the generated
    /// 4 x `uint32_t` header (`struct_size`, `abi_major`, `abi_minor`,
    /// `abi_patch`) and that the header's major equals this value.
    std::uint32_t abi_major{0};
};

/// One successfully loaded mod.
///
/// Besides the identity/gate results, this carries the manifest metadata the
/// `auto_scan` runtime wiring needs to build a `mod::discovered_mod`
/// for the builtin composition without the mod headers depending on the
/// data layer: the manifest bytes are hashed at load time and exposed as hex.
struct loaded_mod {
    std::string id{};
    std::string version{};
    std::uint32_t abi{0};
    bool external{false}; ///< false = internal/static (always false here)
    bool has_code{false}; ///< a static factory was registered and called
    std::string provides_summary{};
    /// Lowercase BLAKE3 hex (64 chars) of the raw `manifest.toml` bytes read
    /// from the builtin image. Discovery re-hashes the on-disk manifests, so
    /// this is the reference value for `discovered_mod.manifest_hash`; parse
    /// it with `refrain::data::parse_digest()`.
    std::string manifest_hash_hex{};
    /// Manifest `[requires]` hard dependencies (actual composition inputs,
    /// not just diagnostics; copied by the `auto_scan` wiring).
    std::vector<mod::mod_requirement> required{};
    /// Manifest `[optional_requires]` (version-checked when present).
    std::vector<mod::mod_requirement> optional{};
};

class host {
  public:
    /// `registry` is the static mod registry to load against; defaults to the
    /// process-wide registry populated by REFR_MOD_STATIC. Tests/embedders can
    /// inject an isolated registry to keep the global one intact.
    explicit host(
        mod::static_mod_registry &registry = mod::static_mod_registry::global())
        : registry_{registry} {}
    host(host &&) noexcept = default;
    auto operator=(host &&) noexcept -> host & = default;
    host(const host &) = delete;
    auto operator=(const host &) -> host & = delete;
    ~host() = default;

    /// Load `plan` through `resolve`, which supplies one package source per
    /// candidate (builtin RFSI image or directory tree; `.rfmod` archives are
    /// not resolved here yet).
    ///
    /// Fails with:
    ///  * `unsupported` when the host already failed a load (recreate it);
    ///  * `already_exists` when the host already loaded successfully;
    ///  * `parse_error` for a corrupt image or malformed manifest;
    ///  * `not_found` when a planned mod has no manifest in the image, or the
    ///    plan's selected provider did not become active;
    ///  * `version_mismatch` for version/ABI/side drift between the gamepack
    ///    pin, the manifest and the registered static code, or when a provider
    ///    vtable's ABI header disagrees with `provider_api::abi_major`;
    ///  * `unsupported` for external mods or for a mod the plan selected
    ///    as provider that declares `[provides]` without a static factory;
    ///  * `permission_denied` from a factory that registers a service its
    ///    manifest does not provide or the plan did not select it for;
    ///  * whatever else a static factory returns (startup aborts).
    ///
    /// On any failure the host drops the providers/API copies/image/mod list it
    /// assembled, clears the service registry and becomes permanently failed.
    auto load(const composition &plan, const package_resolver &resolve)
        -> status;

    /// Convenience overload: load against the embedded builtin image with the
    /// standard resolver (`make_package_resolver()`). Directory candidates are
    /// still read from disk; builtin candidates from `image`.
    auto load(const composition &plan, std::span<const std::byte> image)
        -> status;

    /// Register a service provider during a static mod factory. The provider is
    /// recorded as a candidate and activated on the default instance (all
    /// service contracts are `exclusive`, so no other instance exists); the
    /// service registry is frozen when `load()` completes. `api` is host-owned
    /// (copied into stable storage); the vtable and `self` must outlive the
    /// host (static provider instances).
    ///
    /// The provider id is the currently loading mod id; `version` defaults to
    /// the mod version. Fails with `not_initialized` when called outside a
    /// load, with `permission_denied` unless `service` is covered by the mod's
    /// manifest `[provides]` *and* the gamepack plan selected this mod for a
    /// matching service slot, with `version_mismatch` when
    /// `provider_api::abi_major` disagrees with the vtable's generated ABI
    /// header, or with the service-registry errors
    /// (duplicate/unsupported/frozen).
    auto register_provider(std::string_view service, const provider_api &api,
                           std::string_view version = {},
                           kernel::capability_bits capabilities = 0) -> status;

    /// Instance-aware registration (an `instanced` composition binding):
    /// activate the provider on the plan's instance slot. The plan must have
    /// selected this mod for `service` on exactly that instance name; for an
    /// exclusive or chain binding the only legal value is an empty string (the
    /// reserved `default` slot). Anything else is `permission_denied`
    /// (fail-closed).
    auto register_provider(std::string_view service, std::string_view instance,
                           const provider_api &api,
                           std::string_view version = {},
                           kernel::capability_bits capabilities = 0) -> status;

    /// Register the process-owned provider for a `composition = sealed`
    /// service. Only the composition root may call this, and only before
    /// `load()`: the call fails with `permission_denied` while a mod factory is
    /// running (the sealed provider is part of the host bootstrap, not a mod).
    ///
    /// The provider is recorded under the reserved id `host` with
    /// `source = "host"`, activated on the default instance and marked sealed;
    /// later `register_provider()` calls from mods are rejected for that
    /// service. The service registry is frozen when `load()` completes, so the
    /// sealed provider is immutable for the rest of the process.
    auto register_sealed_provider(std::string_view service,
                                  const provider_api &api,
                                  std::string_view version = {},
                                  kernel::capability_bits capabilities = 0)
        -> status;

    /// True when `service` was registered through `register_sealed_provider()`.
    [[nodiscard]] auto
    is_sealed_service(std::string_view service) const noexcept -> bool;

    /// Frozen service registry after a successful `load()`. Empty when the host
    /// has not loaded yet or when `load()` failed (the failure path clears all
    /// candidates and active instances). Callers use
    /// `active_provider(service)` + `provider_api`.
    [[nodiscard]] auto services() const noexcept
        -> const kernel::service_registry & {
        return services_;
    }

    /// Mod currently being loaded (empty outside `load()`).
    [[nodiscard]] auto current_mod() const noexcept -> std::string_view {
        return current_mod_;
    }

    /// Bytes of the loaded built-in image (host-owned copy); empty before a
    /// successful load. Providers such as `refrain.data.vanilla` mount this as
    /// their lowest-priority pack.
    [[nodiscard]] auto builtin_image_bytes() const noexcept
        -> std::span<const std::byte> {
        return builtin_image_bytes_;
    }

    /// True after a successful `load()`.
    [[nodiscard]] auto loaded() const noexcept -> bool { return loaded_flag_; }

    /// Mods in load order (empty before a successful load).
    [[nodiscard]] auto mods() const noexcept -> std::span<const loaded_mod> {
        return mods_;
    }

    [[nodiscard]] auto find(std::string_view id) const noexcept
        -> const loaded_mod *;

    /// Lowercase hex BLAKE3 digest of the loaded built-in image (empty before
    /// load). Kept as text so the mod headers do not depend on the
    /// data-layer digest type.
    [[nodiscard]] auto builtin_image_digest() const noexcept
        -> std::string_view {
        return builtin_image_digest_hex_;
    }

    /// Stable multi-line dump for `refrain-tools mods load` / server startup.
    [[nodiscard]] auto format() const -> std::string;

  private:
    /// Plan-derived provider selection for one `load()`: every mod id named by
    /// `composition.providers` (exclusive candidate, chain base, chain wrapper
    /// or instanced provider) and, per mod id, the `[providers]` short slots it
    /// was selected for (`data`, `world`, ...).
    struct planned_providers {
        std::set<std::string> mods{};
        std::map<std::string, std::set<std::string>> slots{};
        /// mod id -> `[providers]` slot -> planned instance names. Only
        /// `instanced` bindings populate this; exclusive/chain bindings keep it
        /// empty (only the reserved default instance is legal).
        std::map<std::string, std::map<std::string, std::set<std::string>>>
            instances{};
    };

    /// Read + gate one mod and (when the plan selected it as provider) call its
    /// static factory.
    auto load_one(const composition &plan, const package_source &package,
                  const candidate &entry, const planned_providers &planned)
        -> result<loaded_mod>;

    /// Fail-closed check that every provider the plan selected became active
    /// before the registry is frozen.
    [[nodiscard]] auto verify_plan_activation(const composition &plan) const
        -> status;

    /// Drop everything a failed `load()` assembled and mark the host
    /// permanently unusable (`services()` is left empty, not frozen).
    /// Not `noexcept`: it clears the service registry, which locks.
    void enter_failed_state();

    mod::static_mod_registry &registry_;
    std::vector<loaded_mod> mods_{};
    std::string builtin_image_digest_hex_{};
    std::vector<std::byte> builtin_image_bytes_{};
    kernel::service_registry services_{};
    std::vector<std::unique_ptr<provider_api>> provider_apis_{};
    /// Service ids owned by the host bootstrap; mods cannot provide them.
    std::set<std::string> sealed_services_{};
    std::string current_mod_{};
    std::string current_mod_version_{};
    /// Manifest `[provides]` of the mod whose factory is running (empty
    /// outside `load()`).
    std::vector<std::string> current_mod_provides_{};
    /// `[providers]` short slots the current mod was selected for (empty for
    /// mods the plan does not use as providers).
    std::set<std::string> current_mod_planned_slots_{};
    /// Planned instance names per slot for the mod whose factory is running.
    std::map<std::string, std::set<std::string>>
        current_mod_planned_instances_{};
    bool loaded_flag_{false};
    bool failed_{false};
};

} // namespace refrain::mod
