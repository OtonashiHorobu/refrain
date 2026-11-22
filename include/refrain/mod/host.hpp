// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/kernel/service_registry.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/entry.h>
#include <refrain/mod/lifecycle.hpp>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/dynamic_library.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

// ---------------------------------------------------------------------------
// chain wrapper slots
// ---------------------------------------------------------------------------
//
// A `chain` composition binding is one base provider plus ordered wrappers. The
// base occupies the reserved default instance of the service; every wrapper
// occupies its own instance named `chain:<position>`, where the position is the
// wrapper's index in the declared wrapper list (innermost first, the same order
// the gamepack names them). Query order is outermost first, so a consumer walks
// the wrapper positions in descending order and calls the base last. The slot
// name is a function of the composition binding, not of the provider
// generation: a reloaded wrapper takes over the same slot at the composition
// epoch, and the base slot is never touched by a wrapper change. A wrapper
// whose declared position moves (because another wrapper was inserted or
// removed before it) is refused by the host staging step until it is rebuilt.

/// Instance name prefix of a chain wrapper slot; the rest of the name is the
/// wrapper position.
inline constexpr std::string_view chain_wrapper_instance_prefix{"chain:"};

/// Largest number of wrapper positions one `chain` binding may declare, and the
/// bound the wrapper slot name parser enforces: a position is valid only when
/// it is smaller than this value. A composition that declares more wrappers is
/// refused by the plan verification, so a position the host can publish is
/// always a position the registry reader can decode.
inline constexpr std::size_t max_chain_wrappers{1'000'000};

/// Instance name of the chain wrapper at `position` in the declared wrapper
/// list. `position` must be smaller than `max_chain_wrappers`.
[[nodiscard]] auto chain_wrapper_instance(std::size_t position) -> std::string;

/// Position encoded in `instance`, or nullopt when the name is not a chain
/// wrapper slot (in particular for the reserved default instance) or encodes a
/// position outside `[0, max_chain_wrappers)`.
[[nodiscard]] auto chain_wrapper_position(std::string_view instance) noexcept
    -> std::optional<std::size_t>;

// Mod host: loads a resolved `mod::composition` against the
// embedded built-in content image (RFSI v1) and the static mod registry.
//
// Load scope:
//   1. freeze the static mod registry (deferred registration errors abort) and
//      register the host-owned `builtin` and `native` provider instances of
//      the instanced `refrain:mod.loader` service;
//   2. bootstrap phase: load the mods the gamepack declares as
//      `refrain:mod.loader` providers (`composition.loaders`) together with
//      their transitive hard dependencies, in plan load order. A
//      loader-providing mod runs through a host-owned loader (`builtin` or
//      `native`) only, and its factory registers the loader instance through
//      `register_provider("refrain:mod.loader", <loader id>, api)`. A
//      declared loader that no mod registered fails the load naming the
//      provider mod and the active loader ids;
//   3. for every other mod in the gamepack load order, read its
//      `manifest.toml` from the built-in RFSI image or the installed package,
//      parse it, and enforce the same gates as before: manifest present,
//      id/version match the gamepack pin, ABI == runtime ABI, declared side
//      compatible with the host, host runtime requirements, and no
//      sealed-service claim;
//   4. resolve the manifest's effective loader id to its active
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
//   5. fail closed when a selected provider did not become active.
//
// The loader's declared unload semantics (`clean`/`retire`/`restart_required`)
// and sandbox flag are recorded on every `loaded_mod` for the lifecycle layer.
// A sandboxed loader cannot register providers on behalf of its guest, and a
// mod loaded through a sandboxed loader can never register a loader instance.
// A loader-providing mod is always restart-required: the loader service and
// the instances other mods are dispatched through cannot be replaced while the
// process runs, so the hot lifecycle refuses to unload or reload it. A failed
// initial load clears everything it assembled and a later `load()` returns
// `unsupported` (recreate the host; no partial activation is exposed). After a
// successful load the composition is changed through the hot lifecycle
// (`stage_composition()`/`stage_uninstall()` + `commit_epoch()`), not through a
// second `load()`.
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
    /// ABI major the vtable was built against. `register_provider()`,
    /// `register_sealed_provider()` and `register_mod_loader()` verify that
    /// `vtable` starts with the generated 4 x `uint32_t` header
    /// (`struct_size`, `abi_major`, `abi_minor`, `abi_patch`) and that the
    /// header's major equals this value; zero is refused.
    std::uint32_t abi_major{0};
};

/// One service slot a loaded mod's provider occupies in the registry.
///
/// `instance` names the composition slot: the reserved default instance for an
/// exclusive provider or a chain base, the plan's instance name for an
/// `instanced` binding, and the wrapper's `chain:<position>` instance for a
/// chain wrapper. `serial` identifies the exact registry candidate: a reloaded
/// generation of the same provider has the same service/id/version text but a
/// different serial, so the host removes only the retired candidate after a
/// commit.
struct provided_slot {
    std::string service{};
    /// Registry instance the provider was activated on; empty is the reserved
    /// default instance.
    std::string instance{};
    std::uint64_t serial{0};
};

/// One successfully loaded mod.
///
/// Besides the identity/gate results, this carries the manifest metadata the
/// `auto_scan` runtime wiring needs to build a `mod::candidate` for the builtin
/// composition without the mod headers depending on the data layer: the
/// manifest bytes are hashed at load time and exposed as hex.
struct loaded_mod {
    std::string id{};
    std::string version{};
    std::uint32_t abi{0};
    bool external{false}; ///< false = internal/static (always false here)
    bool has_code{false}; ///< a static factory was registered and called
    std::string provides_summary{};
    /// Custom service contracts this generation declares (manifest
    /// `[contracts]`), recorded before its loader runs and pubished with the
    /// committed composition.
    std::vector<mod::contract_definition> contracts{};
    /// Custom service contracts this generation consumes (manifest
    /// `[contract_requires]`), checked against the definitions of the
    /// composition it joins.
    std::vector<mod::contract_requirement> contract_requires{};
    /// Lowercase BLAKE3 hex (64 chars) of the raw `manifest.toml` bytes read
    /// from the builtin image. Discovery re-hashes the on-disk manifests, so
    /// this is the reference value for `candidate.manifest_hash`; parse it
    /// with `refrain::data::parse_digest()`.
    std::string manifest_hash_hex{};
    /// Manifest `[requires]` hard dependencies (actual composition inputs,
    /// not just diagnostics; copied by the `auto_scan` wiring).
    std::vector<mod::mod_requirement> required{};
    /// Manifest `[optional_requires]` (version-checked when present).
    std::vector<mod::mod_requirement> optional{};
    /// Effective loader id that loaded this package (`builtin`, `native`, ...).
    std::string loader{};
    /// Removal guarantee the active loader declared when the mod was loaded.
    /// The lifecycle layer must not assume a stronger one.
    mod_unload_semantics loader_unload{mod_unload_semantics::restart_required};
    /// True when the loader executes its guests in a sandbox.
    bool loader_sandboxed{false};
    /// Handle the loader returned for this instance; 0 for a package that runs
    /// no code (data-only).
    std::uint64_t instance_handle{0};
    /// Lifecycle state in the committed composition. A freshly loaded mod is
    /// `active`; the record is `retired`/`removed` only on the lifecycle layer,
    /// which keeps the tombstone instead of a `loaded_mod`.
    mod_lifecycle_state state{mod_lifecycle_state::active};
    /// Monotonic generation of this mod id in the process; 1 for the initial
    /// install and incremented on every hot reload. Callers use the service
    /// registry generation for stale detection; this one names the mod version
    /// instance in the audit record.
    std::uint64_t generation{1};
    /// Registry slots this generation activated; retired together with it.
    std::vector<provided_slot> provided_slots{};
};

/// One loader registered on the instanced `refrain:mod.loader` service. The
/// recorded capabilities are the contract the host dispatches every mod
/// through; `unload` is the removal guarantee the lifecycle layer honours for
/// the mods this loader owns.
struct registered_loader {
    std::string id{};
    std::string version{};
    provider_api api{};
    std::uint32_t abi_major{0};
    mod_unload_semantics unload{mod_unload_semantics::restart_required};
    bool sandboxed{false};
    bool supports_hot_install{false};
    /// Mod package that registered the loader (empty for a host-owned loader).
    std::string provider{};
    /// True when the loader was registered before `load()` by the host
    /// bootstrap instead of by a planned loader-providing mod.
    bool host_owned{true};
};

class host;

namespace loader_detail {
struct bridge;
} // namespace loader_detail

/// Options for one hot composition change staged by
/// `host::stage_composition()` / `host::stage_uninstall()`.
struct stage_options {
    /// Mod ids to rebuild even when the next composition resolves them to the
    /// same version and manifest hash (a development reload).
    std::vector<std::string> reload{};
    /// Bounded wait for calls that are still inside the retired generation.
    std::chrono::milliseconds drain_timeout{default_drain_timeout};
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
    ///  * `not_found` when a planned mod has no manifest in the image, a
    ///    declared loader provider did not register the loader it was selected
    ///    for, the plan's selected provider did not become active, or the
    ///    manifest names a third-party loader the host has no active provider
    ///    for;
    ///  * `unsupported` when the manifest names a first-party loader that is
    ///    not built into this host (`wasm`, `lua`, ...), when a
    ///    loader-providing mod would itself run through a loader other than the
    ///    host-owned `builtin`/`native`, or for a mod the plan selected as
    ///    provider that declares `[provides]` without a static factory;
    ///  * `version_mismatch` for version/ABI/side drift between the gamepack
    ///    pin, the manifest and the registered static code, for a loader whose
    ///    declared capabilities drift from its registration, for a
    ///    package-provided loader that names a different provider mod than the
    ///    one that registered it, or when a provider vtable's ABI header
    ///    disagrees with `provider_api::abi_major`;
    ///  * `permission_denied` from a factory that registers a service its
    ///    manifest does not provide or the plan did not select it for, from a
    ///    sandboxed loader trying to register a provider, from a mod that tries
    ///    to register a loader it is not declared for, or from a provider of a
    ///    custom contract that no loaded mod declares or that the declaration
    ///    owner does not trust;
    ///  * `version_mismatch` when a custom contract provider's vtable header
    ///    disagrees with the declared contract version or vtable size, when a
    ///    consumer's required range falls outside the declared compatibility
    ///    band, or when two mods declare one custom service id with different
    ///    ABI versions; the message names both mods and both versions;
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

    // --- hot lifecycle -----------------------------------------------------
    //
    // After a successful `load()` the composition can be changed while the
    // server runs. Every change is staged first and published by
    // `commit_epoch()` at a tick boundary: a tick that is already running keeps
    // calling the committed generation, and the switch is one atomic registry
    // epoch. Staging does the IO (reading packages, opening libraries) and is
    // therefore never called from inside the tick; committing only swaps maps
    // and calls unload hooks.
    //
    // Restart-required mods (static linked code, or any provider of a core
    // sim/world/storage/protocol/runtime service) refuse the change with a
    // message naming the reason. A mod whose `on_unload` fails, or whose
    // in-flight calls do not drain within the bounded wait, is quarantined: it
    // is never called again and needs a process restart to be re-installed.

    /// Composition committed by the last `load()`/`commit_epoch()`.
    [[nodiscard]] auto active_composition() const noexcept
        -> const refrain::mod::composition & {
        return plan_;
    }

    /// Committed composition epoch (1 after a successful load, +1 per commit).
    [[nodiscard]] auto epoch() const noexcept -> std::uint64_t {
        return epoch_;
    }

    /// BLAKE3 hex digest over the committed epoch, the gamepack identity, every
    /// active mod generation and every active provider generation. Two commits
    /// that publish the same composition epoch-level state produce the same
    /// digest; it is the audit/replay identity of the hot composition.
    [[nodiscard]] auto composition_digest() const -> std::string;

    /// Lifecycle snapshot per loaded mod, sorted by id.
    [[nodiscard]] auto lifecycle_records() const
        -> std::vector<mod_lifecycle_record>;
    [[nodiscard]] auto lifecycle_record(std::string_view id) const
        -> std::optional<mod_lifecycle_record>;

    /// Mods the lifecycle layer quarantined (failed unload or drain timeout).
    [[nodiscard]] auto quarantined_mods() const noexcept
        -> std::span<const std::string> {
        return quarantined_mods_;
    }

    /// Retired native mappings kept alive (never dlclose'd): total and per mod.
    [[nodiscard]] auto retired_native_generations() const noexcept
        -> std::size_t {
        return retired_native_libraries_.size();
    }
    [[nodiscard]] auto retired_native_generations(std::string_view id) const
        -> std::size_t;
    /// Native reload cap per mod id (default 8 generations).
    [[nodiscard]] auto native_retire_limit() const noexcept -> std::size_t {
        return native_retire_limit_;
    }
    void set_native_retire_limit(std::size_t limit) noexcept;

    /// Staged change waiting for `commit_epoch()`, if any.
    [[nodiscard]] auto staged_change() const
        -> const std::optional<staged_composition_change> & {
        return staged_change_;
    }

    /// Stage a full composition replacement against the running one.
    ///
    /// `next` is a complete resolved composition that shares the gamepack
    /// identity with the running one. The host diffs the mod lists:
    ///  * ids only in `next` are loaded through their loaders (staged);
    ///  * ids whose version/manifest hash changed, or that are named by
    ///    `options.reload`, are rebuilt through their loader's `reload` slot;
    ///  * ids only in the running composition are quiesced and drained, and
    ///    their loader instance is retired after the commit;
    ///  * a declared chain wrapper the next composition stops declaring has its
    ///    `chain:<position>` provider slot retired at the commit even when its
    ///    package stays loaded;
    ///  * every other mod is left untouched.
    ///
    /// Only changes requested at the tick boundary are published: the registry
    /// is still calling the committed generation when this returns. Failure
    /// rolls the registry staging back and releases every instance loaded by
    /// the failed attempt; the committed epoch and the running composition do
    /// not change. `resolve` is only needed when the diff adds or replaces a
    /// mod.
    ///
    /// Errors: `not_initialized` before a successful load, `already_exists`
    /// when a change is already staged, `unsupported` for a restart-required or
    /// quarantined mod (the message names the reason), `version_mismatch` for a
    /// different gamepack identity, `not_found` for a dependency the running
    /// composition would lose, and whatever the package/loader returns.
    [[nodiscard]] auto stage_composition(const refrain::mod::composition &next,
                                         const package_resolver &resolve,
                                         const stage_options &options = {})
        -> result<staged_composition_change>;

    /// Stage the removal of the active version of `id` (uninstall). The caller
    /// removes the install receipt after this returns and before/after the
    /// commit; the running composition no longer references the mod once the
    /// commit succeeds. Same restart-required/dependency gates as
    /// `stage_composition()`.
    [[nodiscard]] auto stage_uninstall(std::string_view id,
                                       const stage_options &options = {})
        -> result<staged_composition_change>;

    /// Drop the staged change without publishing it. The running epoch is
    /// untouched; instances loaded for the aborted change are released.
    void abort_staged() noexcept;

    /// Publish the staged change atomically. Called at a tick boundary only.
    ///
    /// The registry commit switches every provider slot at once; the retired
    /// generations then run their loader `unload` hook with the reason
    /// (`reload`/`uninstall`), native mappings are moved to the retired list
    /// and the candidate records of the old generation are dropped. A mod whose
    /// unload fails is quarantined and reported, but the commit itself still
    /// succeeds (the new generation is already safe to call). With nothing
    /// staged the committed epoch is returned unchanged.
    [[nodiscard]] auto commit_epoch() -> result<composition_commit>;

    /// Register a service provider during a static mod factory. The provider is
    /// recorded as a candidate and activated on the instance the plan binds
    /// this mod to: the reserved default instance for an exclusive provider or
    /// a chain base, and the mod's own `chain:<position>` instance when the
    /// plan declares it as a chain wrapper. A chain wrapper therefore never
    /// occupies the base slot, and a wrapper generation can be reloaded or
    /// retired without touching the running base. A mod the plan binds as both
    /// the chain base and a wrapper of one service fails closed.
    ///
    /// `api` is host-owned (copied into stable storage); the vtable and `self`
    /// must outlive the host (static provider instances).
    ///
    /// The provider id is the currently loading mod id; `version` defaults to
    /// the mod version. Fails with `not_initialized` when called outside a
    /// load, with `permission_denied` unless `service` is covered by the mod's
    /// manifest `[provides]` *and* the gamepack plan selected this mod for a
    /// matching service slot, with `version_mismatch` when
    /// `provider_api::abi_major` disagrees with the vtable's generated ABI
    /// header, or with the service-registry errors
    /// (duplicate/unsupported/frozen).
    ///
    /// A service id outside the reserved `refrain` namespace is a custom
    /// contract: the table is accepted only when a loaded mod declares the
    /// service in its manifest `[contracts]`, the registering mod is the
    /// declaration owner or is listed in the declaration's
    /// `trusted_providers`, and the vtable header matches the declared
    /// contract version and vtable size exactly. The host never reads the
    /// contract's function slots, only the generated header.
    auto register_provider(std::string_view service, const provider_api &api,
                           std::string_view version = {},
                           kernel::capability_bits capabilities = 0) -> status;

    /// Instance-aware registration (an `instanced` or `chain` composition
    /// binding): activate the provider on the plan's instance slot. The plan
    /// must have selected this mod for `service` on exactly that instance name.
    /// For a chain wrapper the only legal name is the wrapper's own
    /// `chain:<position>` slot (an empty `instance` selects it automatically);
    /// the reserved default instance belongs to the chain base, so passing it
    /// for a wrapper is `permission_denied` (fail-closed). Anything else is
    /// `permission_denied` as well.
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
    /// A loader-providing mod does not use this call: its factory registers
    /// the instance through `register_provider("refrain:mod.loader", <id>,
    /// api)`, which the host gates on the gamepack `[loaders]` declaration,
    /// the loading mod's package source and the loader that executes it.
    ///
    /// The provider's `capabilities()` is queried immediately and must report
    /// `loader_id` itself, the contract ABI major the host was built against, a
    /// known unload semantics and a plausible capability struct; the recorded
    /// values are re-checked before every dispatch so a loader cannot change
    /// its contract at runtime. `version` defaults to `1.0.0`. A bootstrap
    /// loader is host-owned, so its capabilities must leave `provider` empty.
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
        /// `instanced` bindings populate this.
        std::map<std::string, std::map<std::string, std::set<std::string>>>
            instances{};
        /// mod id -> `[providers]` slot -> chain wrapper instances the plan
        /// binds this mod to. A chain wrapper position belongs to the plan, not
        /// to the base slot, so a wrapper generation can be staged, replaced or
        /// retired while the base keeps answering.
        std::map<std::string, std::map<std::string, std::set<std::string>>>
            chain_slots{};
        /// mod id -> `[providers]` slots the plan binds as the reserved default
        /// (exclusive provider or chain base).
        std::map<std::string, std::set<std::string>> default_slots{};
    };

    /// One custom contract definition recorded from a loaded mod's manifest.
    /// `owner` is the declaring mod; `definition` is the validated descriptor
    /// the host conformance-checks provider tables and consumer ranges
    /// against.
    struct contract_record {
        std::string owner{};
        mod::contract_definition definition{};
    };

    /// Record the custom contract definitions of `manifest` for the mod being
    /// loaded. Fails closed when a different loaded mod already declares the
    /// same service: an incompatible ABI is `version_mismatch`, an identical
    /// declaration is `already_exists`. Both errors name both mods. The
    /// record is added to the committed map during an initial load and to the
    /// staged overlay during a hot epoch.
    auto record_contract_definitions(const mod_manifest &manifest) -> status;

    /// Definition of `service` visible to the mod being loaded or staged: its
    /// own manifest first, then the staged epoch, then the committed
    /// composition. Nullptr when no loaded mod declares the service.
    [[nodiscard]] auto find_contract_definition(std::string_view service) const
        -> const contract_record *;

    /// Validate a custom-contract provider table against the declared
    /// descriptor: the generated vtable header (`struct_size`, `abi_major`,
    /// `abi_minor`, `abi_patch`) must match the declaration exactly. Fails
    /// with `version_mismatch` naming both versions and sizes.
    [[nodiscard]] auto check_contract_provider(const contract_record &record,
                                               const provider_api &api) const
        -> status;

    /// Validate the contract definitions and requirements of the composition
    /// being published (committed composition during load, or the staged
    /// result during a hot epoch): one owner per service id, and every
    /// required range inside the owner's compatible range. Errors name both
    /// mods and both versions.
    [[nodiscard]] auto verify_contract_composition() const -> status;

    /// Project a composition's provider selection onto the per-mod slot map
    /// the factory permission checks and the plan verification use. Fails with
    /// `unsupported` when a chain binding declares more wrappers than the
    /// position bound, before a slot name beyond the reader's bound is formed.
    [[nodiscard]] static auto
    plan_providers(const refrain::mod::composition &plan)
        -> result<planned_providers>;

    /// Read + gate one mod and (when the plan selected it as provider) call its
    /// static factory.
    auto load_one(const refrain::mod::composition &plan,
                  const package_source &package, const candidate &entry,
                  const planned_providers &planned) -> result<loaded_mod>;

    /// One external native mod instance. The shared library is never
    /// `dlclose`d while the process runs: retiring an instance calls
    /// `on_unload` and moves the mapping to the host's retired list.
    struct external_mod_state {
        std::unique_ptr<platform::dynamic_library> library{};
        refrain_mod_on_unload_ex on_unload_ex{nullptr};
        refrain_mod_on_unload on_unload{nullptr};
        void *user{nullptr};
        std::uint64_t handle{0};
        std::string id{};
        std::string version{};
        /// `on_unload` already ran (or the instance is quarantined): the state
        /// is a tombstone the destructor must not unload a second time.
        bool released{false};
        bool quarantined{false};
    };

    /// State of one yielded in-flight generation; a staged load appends to
    /// `external_mods_` so a commit/abort can find the instance by handle.
    [[nodiscard]] auto find_external_mod(std::uint64_t handle)
        -> external_mod_state *;

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

    /// Register one loader instance on the loader service and record its
    /// contract. `provider` is the mod package that registered the instance
    /// (empty for a host-owned loader); the contract capabilities must agree
    /// with it. Shared by the host bootstrap and the package-provided
    /// registration path, so both are validated and recorded identically.
    /// `out_serial` receives the registry candidate serial when non-null.
    auto add_loader_instance(std::string_view loader_id,
                             const provider_api &api, std::string_view version,
                             std::string_view provider, bool host_owned,
                             std::uint64_t *out_serial = nullptr) -> status;

    /// Package-provided loader registration: called from `register_provider()`
    /// when the currently loading mod registers an instance of the loader
    /// service. Enforces the bootstrap-only loader ids, the gamepack
    /// `[loaders]` declaration, the manifest `[provides]` claim, the
    /// first-party loader of the providing mod and the install source.
    auto register_mod_loader_from_mod(std::string_view loader_id,
                                      const provider_api &api,
                                      std::string_view version) -> status;

    /// Active `refrain:mod.loader` provider for `loader_id` plus a snapshot of
    /// its bootstrap registration. The snapshot owns its storage because a
    /// loader-providing mod can register another loader while this binding is
    /// being dispatched, and that registration reallocates the host's loader
    /// records.
    struct loader_binding {
        registered_loader record{};
        provider_api api{};
    };

    /// Package root handed to the loader contract: the installed package's
    /// receipt directory for an installed package, empty for a manifest read
    /// from the built-in image. A loader reads its guest members through
    /// `<root>/<version>.json` and the content store.
    [[nodiscard]] auto loader_package_root() const -> std::string;

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

    /// Dispatch a staged replacement through the loader's `reload` slot. The
    /// old handle stays valid until the host unloads it after the commit.
    auto reload_with_loader(const loader_binding &binding,
                            const mod_manifest &manifest, bool activate_code,
                            std::uint64_t old_handle, std::uint64_t &new_handle)
        -> status;

    /// Run a loader's `unload` slot for one loaded generation.
    auto dispatch_loader_unload(const loaded_mod &mod, unload_reason reason)
        -> status;

    /// Host-owned implementation behind the `native` loader's `unload` slot:
    /// call `on_unload` (with the reason when the descriptor advertises it),
    /// then retire the mapping. A failed callback quarantines the instance.
    auto release_loader_instance(std::uint64_t instance, unload_reason reason)
        -> status;

    /// Host-owned implementation behind the `native` loader's `reload` slot:
    /// open the replacement library from the current package context and call
    /// its `on_load`, without touching the old instance.
    auto native_reload_instance(std::uint64_t instance, std::uint64_t &out_new)
        -> status;

    /// Load one native instance from the current dispatch context. Used both
    /// by the first load and by a staged reload (the old instance is not
    /// touched here; the host retires it after the commit).
    auto open_native_instance(std::uint64_t &handle) -> status;

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

    /// Fail-closed check that every provider the plan selected became active.
    /// `staged` verifies the staged view (a pending registry change plus the
    /// staged mod list) instead of the committed composition.
    [[nodiscard]] auto
    verify_plan_activation(const refrain::mod::composition &plan,
                           bool staged = false) const -> status;

    /// Fail-closed check that every loader the plan declares was registered by
    /// the declared provider mod during the bootstrap phase. Fails with
    /// `not_found` naming the provider mod and the active loader ids, or with
    /// `version_mismatch` when a loader was registered by another package.
    [[nodiscard]] auto
    verify_declared_loaders(const refrain::mod::composition &plan) const
        -> status;

    /// Mod ids the bootstrap phase loads: every declared loader provider plus
    /// its transitive hard-dependency closure. Returned as a set for membership
    /// checks; callers walk `plan.load_order` to keep the load order.
    [[nodiscard]] auto
    bootstrap_mod_ids(const refrain::mod::composition &plan) const
        -> result<std::set<std::string>>;

    /// `a, b` of every registered loader id, in registration order; `none`
    /// when empty.
    [[nodiscard]] auto registered_loader_ids() const -> std::string;

    /// `stage_composition()` body; the caller holds `lifecycle_mutex_`.
    [[nodiscard]] auto stage_composition_locked(
        const refrain::mod::composition &next, const package_resolver &resolve,
        const stage_options &options) -> result<staged_composition_change>;

    /// Drop everything a failed staging attempt assembled and restore the
    /// registry to its committed state. Safe to call more than once.
    void abort_staged_internals() noexcept;

    /// Refusal message when `mod` cannot be reloaded or uninstalled without a
    /// restart; empty when it can.
    [[nodiscard]] auto restart_required_reason_for(const loaded_mod &mod) const
        -> std::string;

    /// Canonical BLAKE3 text of the committed composition (see
    /// `composition_digest()`); exposed for tests that want the input.
    [[nodiscard]] auto composition_digest_text() const -> std::string;

    /// Record a quarantined mod id and log why. Idempotent.
    void record_quarantine(std::string_view id, std::string_view reason);

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
    /// Chain wrapper instances per slot the plan binds the mod being dispatched
    /// to (see `chain_wrapper_instance()`).
    std::map<std::string, std::set<std::string>> current_mod_chain_slots_{};
    /// Planned slots the mod being dispatched is the exclusive provider or
    /// chain base of.
    std::set<std::string> current_mod_default_slots_{};
    /// True when the plan selected the currently loading mod as a provider.
    bool current_mod_selected_provider_{false};
    /// Manifest and candidate of the mod being dispatched, borrowed for the
    /// duration of one `load_one()` call.
    const mod_manifest *current_manifest_{nullptr};
    /// Composition being loaded, borrowed for the duration of `load()`. The
    /// package-provided loader registration path reads the declared loader plan
    /// from it before `plan_` is published.
    const composition *current_plan_{nullptr};
    /// Custom contract definitions of the mod whose factory is running, copied
    /// so `find_contract_definition()` can return a stable address while the
    /// manifest is being dispatched.
    std::vector<contract_record> current_contract_records_{};
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
    /// Sequence for the unique materialised path of each native generation.
    /// Every reload writes a new file so the dynamic loader never returns a
    /// mapping of an earlier generation by path identity.
    std::uint64_t materialized_sequence_{0};
    /// Private directory holding materialised native entry libraries read from
    /// an installed package. Empty until the first external native load.
    std::string materialized_dir_{};
    core::config::store *config_{nullptr};
    /// External native mods in load order (libraries + unload hooks).
    std::vector<external_mod_state> external_mods_{};
    /// Retired native mappings, kept mapped until process exit. Never
    /// `dlclose`d: a replaced generation may still be executing a call that
    /// started before the commit.
    std::vector<std::unique_ptr<platform::dynamic_library>>
        retired_native_libraries_{};
    std::map<std::string, std::size_t> native_retired_generations_{};
    std::size_t native_retire_limit_{default_native_retire_limit};
    /// Committed composition and its epoch (1 after a successful load).
    composition plan_{};
    std::uint64_t epoch_{0};
    /// Last generation handed out per mod id; kept after removal so a
    /// reinstall continues the sequence instead of reusing generation 1.
    std::map<std::string, std::uint64_t> mod_generations_{};
    /// Staged change waiting for the tick-boundary commit.
    std::optional<staged_composition_change> staged_change_{};
    /// The complete plan the staged change will publish on commit.
    std::optional<composition> staged_next_composition_{};
    std::vector<loaded_mod> staged_mods_{};
    /// Custom contract definitions of the committed composition, by service
    /// id. Rebuilt from the loaded mods on every commit.
    std::map<std::string, contract_record> contract_definitions_{};
    /// Custom contract definitions recorded while staging a hot epoch; the
    /// committed map is authoritative until the commit publishes them.
    std::map<std::string, contract_record> staged_contract_definitions_{};
    bool staging_{false};
    /// Handle and generation of the instance a staged `load_one()` replaces,
    /// when the diff asked the loader to rebuild the mod.
    std::optional<std::uint64_t> current_replaced_handle_{};
    std::uint64_t current_replaced_generation_{0};
    std::string current_replaced_loader_{};
    /// Committed generations a staged change retires (replaced or removed),
    /// with the reason to report to the loader after the commit.
    std::vector<loaded_mod> staged_retire_mods_{};
    std::vector<std::pair<std::string, unload_reason>> staged_retire_reasons_{};
    /// Chain wrapper slots the staged composition stops declaring while their
    /// package stays loaded. They are retired at the commit like the slots of a
    /// retiring generation, so the composed chain never keeps calling a
    /// provider the plan no longer declares.
    std::vector<provided_slot> staged_retired_chain_slots_{};
    /// Bounded drain wait the staged change requested.
    std::chrono::milliseconds staged_drain_timeout_{default_drain_timeout};
    /// Registry slots the current mod factory activated.
    std::vector<provided_slot> current_mod_registered_slots_{};
    /// Mods quarantined by a failed unload or drain timeout; a quarantined id
    /// can never be installed or reloaded again in this process.
    std::vector<std::string> quarantined_mods_{};
    /// Serialises staging against the tick-boundary commit. Staging is the
    /// long IO half and runs on the command thread; the commit only tries the
    /// lock, so a tick never waits for a package installation.
    mutable std::mutex lifecycle_mutex_{};
    bool loaded_flag_{false};
    bool failed_{false};
};

} // namespace refrain::mod
