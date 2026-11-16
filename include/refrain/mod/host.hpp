// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/kernel/service_registry.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/entry.h>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/dynamic_library.hpp>

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

/// The host allocation table handed to every mod in `refrain_mod_init`.
///
/// Memory that crosses the mod boundary must come from here: the host frees
/// only what this table allocated, so a mod that allocates with its own
/// allocator and returns the buffer would otherwise be freed by a different
/// one. `alignment` must be a power of two and is raised to
/// `alignof(std::max_align_t)` when smaller; a non-power-of-two request
/// returns nullptr.
[[nodiscard]] auto host_allocator() noexcept -> refrain_allocator;

namespace kernel = refrain::kernel;

// Mod host: loads a resolved `mod::composition` against the
// embedded built-in content image (RFSI v1) and the static mod registry.
//
// Load scope:
//   1. freeze the static mod registry (deferred registration errors abort) and
//      register the host-owned `builtin` and `native` provider instances of
//      the instanced `refrain:mod.loader` service;
//   2. for every mod in the gamepack load order, read its `manifest.toml` from
//      the built-in RFSI image or the on-disk package, parse it, and enforce
//      the same gates as before: manifest present, id/version match the
//      gamepack pin, ABI == runtime ABI, declared side compatible with the
//      host, host runtime requirements, and no sealed-service claim;
//   3. resolve the manifest's effective loader id to its active
//      `refrain:mod.loader` provider instance, validate the loader's declared
//      capabilities, call the loader's `validate`, then its `load`. The host
//      has no execution branch of its own: the builtin loader calls the static
//      factory of every selected provider, and the native loader opens the
//      shared library and calls `on_load` with the sealed `refrain:host.core`
//      table. A mod that declares `[provides]` but is not selected by
//      `composition.providers` keeps loading as a data/content mod with its
//      code inactive (fail-closed composition: only planned providers may
//      claim a service). A loader id with no active provider fails the load
//      before any mod code runs, naming the id and the active loaders;
//   4. fail closed when a selected provider did not become active.
//
// The loader's declared unload semantics (`clean`/`retire`/`restart_required`)
// and sandbox flag are recorded on every `loaded_mod` for the lifecycle layer.
// A sandboxed loader cannot register providers on behalf of its guest. The host
// is single-use: a failed load clears everything it assembled and a later
// `load()` returns `unsupported` (recreate the host; no partial activation is
// exposed).
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
    /// Effective loader id that loaded this package (`builtin`, `native`, ...).
    std::string loader{};
    /// True when the manifest omitted `[entry].loader` and the parser inferred
    /// the loader from the entry shape or from the absence of an entry table.
    bool loader_inferred{false};
    /// Removal guarantee the active loader declared when the mod was loaded.
    /// The lifecycle layer must not assume a stronger one.
    mod_unload_semantics loader_unload{mod_unload_semantics::restart_required};
    /// True when the loader executes its guests in a sandbox.
    bool loader_sandboxed{false};
    /// Handle the loader returned for this instance; 0 for a package that runs
    /// no code (data-only).
    std::uint64_t instance_handle{0};
};

/// One loader the host bootstrap registered on the instanced
/// `refrain:mod.loader` service. The recorded capabilities are the contract
/// the host dispatches every mod through; `unload` is the removal guarantee
/// the lifecycle layer honours for the mods this loader owns.
struct registered_loader {
    std::string id{};
    std::string version{};
    provider_api api{};
    std::uint32_t abi_major{0};
    mod_unload_semantics unload{mod_unload_semantics::restart_required};
    bool sandboxed{false};
    bool supports_hot_install{false};
};

class host;

namespace loader_detail {
struct bridge;
} // namespace loader_detail

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
    /// Calls external mod `on_unload` callbacks in reverse load order, then
    /// releases their shared libraries.
    ~host();

    /// Load `plan` through `resolve`, which supplies one package source per
    /// candidate (builtin RFSI image or directory tree; `.rfmod` archives are
    /// not resolved here yet).
    ///
    /// Fails with:
    ///  * `unsupported` when the host already failed a load (recreate it);
    ///  * `already_exists` when the host already loaded successfully, or when
    ///    a loader id is registered twice;
    ///  * `parse_error` for a corrupt image or malformed manifest;
    ///  * `not_found` when a planned mod has no manifest in the image, the
    ///    plan's selected provider did not become active, or the manifest names
    ///    a third-party loader the host has no active provider for;
    ///  * `unsupported` when the manifest names a first-party loader that is
    ///    not built into this host (`wasm`, `lua`, ...), or for a mod the plan
    ///    selected as provider that declares `[provides]` without a static
    ///    factory;
    ///  * `version_mismatch` for version/ABI/side drift between the gamepack
    ///    pin, the manifest and the registered static code, for a loader whose
    ///    declared capabilities drift from its registration, or when a provider
    ///    vtable's ABI header disagrees with `provider_api::abi_major`;
    ///  * `permission_denied` from a factory that registers a service its
    ///    manifest does not provide or the plan did not select it for, or from
    ///    a sandboxed loader trying to register a provider;
    ///  * whatever else a static factory or loader returns (startup aborts).
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

    /// Register a mod loader provider on the instanced `refrain:mod.loader`
    /// service under `loader_id`. This is host bootstrap API, the loader
    /// counterpart of `register_sealed_provider`: it is only legal before
    /// `load()` and fails with `permission_denied` while a mod factory is
    /// running. The host registers its own `builtin` and `native` loaders this
    /// way at the start of `load()`; tests and embedders use the same call to
    /// install a fake loader.
    ///
    /// The provider's `capabilities()` is queried immediately and must report
    /// `loader_id` itself, the contract ABI major the host was built against, a
    /// known unload semantics and a plausible capability struct; the recorded
    /// values are re-checked before every dispatch so a loader cannot change
    /// its contract at runtime. `version` defaults to `1.0.0`.
    auto register_mod_loader(std::string_view loader_id,
                             const provider_api &api,
                             std::string_view version = {}) -> status;

    /// Loaders in registration order (empty before `load()`).
    [[nodiscard]] auto loaders() const noexcept
        -> std::span<const registered_loader> {
        return loaders_;
    }

    /// Recorded loader for `loader_id`, or nullptr when the host bootstrap did
    /// not register it.
    [[nodiscard]] auto find_loader(std::string_view loader_id) const noexcept
        -> const registered_loader *;

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

    /// Host-owned configuration store. The composition root sets it before
    /// `load()`, so static mod factories can read their own namespace view.
    /// The store must outlive the host; the host never takes ownership.
    void set_config(core::config::store &store) noexcept { config_ = &store; }
    [[nodiscard]] auto config() const noexcept -> core::config::store * {
        return config_;
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

    /// One external native mod: the shared library stays loaded for the
    /// process lifetime; `on_unload` runs at host destruction.
    struct external_mod_state {
        std::unique_ptr<platform::dynamic_library> library{};
        refrain_mod_on_unload on_unload{nullptr};
        void *user{nullptr};
        std::uint64_t handle{0};
    };

    /// ABI `self` object of one host-internal loader provider. The bridge in
    /// `host.cpp` casts the opaque ABI pointer back to this type.
    struct loader_provider_state {
        host *owner{nullptr};
        std::string_view loader_id{};
        mod_unload_semantics unload{mod_unload_semantics::restart_required};
        bool sandboxed{false};
        bool supports_hot_install{false};
    };

    /// Register `builtin` and `native` on the `refrain:mod.loader` service.
    /// They wrap the static mod registry and the dynamic-library entry ABI the
    /// host itself owns, so they ship with the host instead of being a mod.
    auto register_bootstrap_loaders() -> status;

    /// Active `refrain:mod.loader` provider for `loader_id` plus its recorded
    /// bootstrap registration.
    struct loader_binding {
        const registered_loader *record{nullptr};
        provider_api api{};
    };

    /// Resolve `loader_id` to its active provider. Fails closed with
    /// `not_found` or `unsupported` naming the loader and the active loader
    /// ids when the host has no such loader.
    [[nodiscard]] auto resolve_loader(std::string_view loader_id) const
        -> result<loader_binding>;

    /// Query a loader's capabilities and validate them against the recorded
    /// contract (drift guard). Fails closed on a mismatched id, ABI, unload
    /// semantics, sandbox flag or capability struct size.
    [[nodiscard]] auto
    read_loader_capabilities(const loader_binding &binding) const
        -> result<mod_loader_capabilities>;

    /// Dispatch a validated manifest through the loader: `validate` first,
    /// then `load` when the plan selected provider code.
    auto validate_with_loader(const loader_binding &binding,
                              const mod_manifest &manifest, bool activate_code)
        -> status;
    auto load_with_loader(const loader_binding &binding,
                          const mod_manifest &manifest, bool activate_code,
                          std::uint64_t &handle) -> status;

    /// Implementations behind the builtin loader's ABI slots. They run with
    /// `current_manifest_` / `current_candidate_` set by `load_one`.
    auto builtin_validate() -> status;
    auto builtin_load(std::uint64_t &handle) -> status;

    /// Implementations behind the native loader's ABI slots: platform-triple
    /// payload validation, `dlopen`, entry symbol and descriptor ABI checks,
    /// then `on_load` with the sealed host core table.
    auto native_validate(std::string_view native_path,
                         std::string_view wasm_path, std::uint32_t abi,
                         bool activate_code) -> status;
    auto native_load(std::uint64_t &handle) -> status;

    /// Detail message of the last failed loader call. The loader ABI carries
    /// an error code only; host-internal loaders keep their message here so
    /// the host can report it instead of a generic refusal.
    void set_loader_dispatch_error(std::string message);
    void clear_loader_dispatch_error();
    [[nodiscard]] auto take_loader_dispatch_error() -> std::string;

    /// Remove the private directory that holds materialised native entries.
    /// Safe to call more than once; the shared libraries must be unloaded
    /// first.
    void remove_materialized_entries() noexcept;

    /// Call external `on_unload` callbacks in reverse load order, then drop
    /// the library handles. Safe to call more than once.
    void unload_external_mods() noexcept;

    /// Fail-closed check that every provider the plan selected became active
    /// before the registry is frozen.
    [[nodiscard]] auto verify_plan_activation(const composition &plan) const
        -> status;

    /// Drop everything a failed `load()` assembled and mark the host
    /// permanently unusable (`services()` is left empty, not frozen).
    /// Not `noexcept`: it clears the service registry, which locks.
    void enter_failed_state();

    friend struct loader_detail::bridge;

    mod::static_mod_registry &registry_;
    std::vector<loaded_mod> mods_{};
    std::string builtin_image_digest_hex_{};
    std::vector<std::byte> builtin_image_bytes_{};
    kernel::service_registry services_{};
    std::vector<std::unique_ptr<provider_api>> provider_apis_{};
    /// Service ids owned by the host bootstrap; mods cannot provide them.
    std::set<std::string> sealed_services_{};
    /// Loaders registered by the host bootstrap, in registration order.
    std::vector<registered_loader> loaders_{};
    /// ABI `self` objects of the `builtin` and `native` loaders.
    std::unique_ptr<loader_provider_state> builtin_loader_state_{};
    std::unique_ptr<loader_provider_state> native_loader_state_{};
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
    /// True when the plan selected the currently loading mod as a provider.
    bool current_mod_selected_provider_{false};
    /// Manifest and candidate of the mod being dispatched, borrowed for the
    /// duration of one `load_one()` call.
    const mod_manifest *current_manifest_{nullptr};
    const candidate *current_candidate_{nullptr};
    const package_source *current_package_{nullptr};
    /// Static descriptor matched by the builtin loader for the current mod.
    const static_mod_descriptor *current_static_descriptor_{nullptr};
    /// Effective loader of the mod being dispatched.
    std::string current_loader_id_{};
    bool current_loader_sandboxed_{false};
    /// True when the current loader actually executed the mod's code.
    bool current_load_has_code_{false};
    /// Detail message of the last failed loader call (see
    /// `set_loader_dispatch_error`).
    std::string loader_dispatch_error_{};
    /// Next loader-owned instance handle.
    std::uint64_t next_instance_handle_{1};
    /// Private directory holding materialised native entry libraries read from
    /// an installed package. Empty until the first external native load.
    std::string materialized_dir_{};
    core::config::store *config_{nullptr};
    /// External native mods in load order (libraries + unload hooks).
    std::vector<external_mod_state> external_mods_{};
    bool loaded_flag_{false};
    bool failed_{false};
};

} // namespace refrain::mod
