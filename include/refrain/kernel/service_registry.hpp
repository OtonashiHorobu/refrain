// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::kernel {

/// Capability bits are declared by providers and queried by the host/mods.
/// 64 bits are reserved for kernel-known capabilities; providers may define
/// contract-local meanings per service.
using capability_bits = std::uint64_t;

struct provider_info {
    std::string service{}; ///< e.g. "refrain:worldgen"
    std::string id{};      ///< e.g. "refrain.worldgen.flat@1"
    std::string version{}; ///< semantic version of the provider
    capability_bits capabilities{0};
    void *instance{nullptr}; ///< provider API table / object (opaque)
    std::string source{};    ///< library path or "builtin" for diagnostics
    /// Generation of the active slot this copy was read from. Zero for a
    /// candidate reported by `find()`/`providers()`: a candidate is not
    /// active, so it has no generation.
    std::uint64_t generation{0};
};

/// One active provider plus the identity a caller caches for the duration of
/// a tick. `generation` advances monotonically per `(service, instance)` slot
/// whenever a new payload becomes visible there: immediately when
/// `select_active()` replaces an active provider, or at `commit_epoch()` when a
/// staged change is published. A handle taken before the switch is stale
/// without the caller holding a pointer into the registry. `epoch` is the
/// committed composition epoch the handle was read at; a commit that leaves the
/// slot unchanged does not invalidate the handle.
struct provider_handle {
    provider_info provider{};
    std::string instance{}; ///< resolved slot name (`default` for the reserved
                            ///< instance)
    std::uint64_t generation{0};
    std::uint64_t epoch{0};
};

/// Guard for one in-flight call into an active provider.
///
/// A lease is acquired from the registry and releases its reference count when
/// destroyed. A reload/uninstall quiesces the slot (new leases fail) and then
/// drains the counter with a bounded wait, so the old generation is never
/// released while a call is still inside it. The lease carries no pointer to
/// the provider: it only keeps the count alive.
class provider_lease {
  public:
    provider_lease() = default;
    provider_lease(const provider_lease &) = delete;
    auto operator=(const provider_lease &) -> provider_lease & = delete;
    provider_lease(provider_lease &&other) noexcept
        : in_flight_{std::move(other.in_flight_)} {}
    auto operator=(provider_lease &&other) noexcept -> provider_lease & {
        if (this != &other) {
            release();
            in_flight_ = std::move(other.in_flight_);
        }
        return *this;
    }
    ~provider_lease() { release(); }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return in_flight_ != nullptr;
    }

    /// Release the reference count early (the destructor then does nothing).
    void release() noexcept {
        if (in_flight_ != nullptr) {
            in_flight_->fetch_sub(1, std::memory_order_acq_rel);
            in_flight_.reset();
        }
    }

  private:
    friend class service_registry;
    explicit provider_lease(
        std::shared_ptr<std::atomic<std::uint64_t>> in_flight) noexcept
        : in_flight_{std::move(in_flight)} {}

    std::shared_ptr<std::atomic<std::uint64_t>> in_flight_{};
};

/// Registry of candidate providers plus the active provider per service
/// *instance*.
///
/// Composition modes: a contract declares `composition = exclusive | instanced
/// | chain | registry`. The registry is mode-agnostic runtime bookkeeping; the
/// upper resolver (gamepack plan) picks the composition semantics and then
/// activates the resolved providers here:
///  - `exclusive`: exactly one provider is active for the service, addressed as
///    the *default instance* (the upper layer selects it with the single
///    instance-free form);
///  - `instanced`: several providers of the same service are active at once and
///    each is addressed by a caller-chosen instance name (`"primary"`,
///    `"debug"`, ...); the same provider may even be active under several
///    instances;
///  - `chain`/`registry`: ordered/list composition stays an upper-layer
///    concern; each link/entry is just another active instance here.
///
/// Instances: `select_active(service, provider_id, version)` (empty `instance`)
/// targets the reserved default instance `default_instance` ("default"). A
/// non-empty instance string addresses an additional slot for the same service.
/// Re-selecting an instance that is already active *updates* it in place; it is
/// not an error.
///
/// ## Immutable startup composition and hot epochs
///
/// A process starts with one composition loaded once and then never changed
/// (`select_active()` + `commit_epoch()`). Sealed services - the host-owned
/// `refrain:host.*` runtime tables and the mod loader bootstrap - are sealed
/// with `seal()` and can never be selected, staged or retired again; `freeze()`
/// stays available for embedders/tools that want the whole registry immutable
/// forever.
///
/// Ordinary services support hot replacement in epochs:
///  - a replacement provider is registered as a candidate with
///    `add_staged_provider()` (candidates may coexist with the running
///    generation, including the same service/id/version text);
///  - `stage_active()` / `stage_retire()` record the change without publishing
///    it: queries still see the current composition, so a tick that is already
///    running keeps calling a consistent implementation;
///  - `commit_epoch()` publishes every staged change atomically and advances
///    the epoch once. Each slot whose staged payload is published gets a new
///    generation; a slot that is retired keeps its generation counter, so a
///    later reactivation can never reuse an older generation number.
///
/// Callers cache a `provider_handle` for the duration of one tick and check it
/// with `validate()` (or take a `provider_lease` with `acquire()`) before
/// crossing into provider code. `validate()` reports `error_code::stale` for a
/// handle whose generation is no longer the current one; the caller must
/// re-`query()` instead of calling the retired implementation. The registry is
/// lock-protected, so a handle never points into registry storage itself.
///
/// Quiescing and draining: `quiesce()` stops new leases for a slot;
/// `drain()` waits a bounded time for outstanding leases to release and fails
/// with `error_code::timeout` when they do not. The lifecycle layer treats a
/// drain timeout and a failed `on_unload` as quarantine conditions.
///
/// Thread safety: every operation is internally synchronized; queries return
/// *copies* (`std::optional<provider_info>`, `std::vector<...>`), never
/// pointers into the registry storage, so a concurrent
/// `add_provider()`/`clear()` can never dangle a caller-held pointer.
class service_registry {
  public:
    /// Reserved instance name used by the single-argument forms of
    /// `select_active()`/`active_provider()`; an empty instance string
    /// normalizes to it. The name is reserved: `select_active(..., "default")`
    /// and `select_active(...)` address the same slot.
    static constexpr std::string_view default_instance{"default"};

    service_registry() = default;
    service_registry(const service_registry &) = delete;
    auto operator=(const service_registry &) = delete;
    service_registry(service_registry &&) = delete;
    auto operator=(service_registry &&) = delete;
    ~service_registry() = default;

    /// Register a candidate provider. Fails with `already_exists` on duplicate
    /// service/id/version tuples and with `unsupported` once frozen or when the
    /// service is sealed.
    [[nodiscard]] auto add_provider(provider_info info) -> status;

    /// Register a candidate and return its stable serial. Same duplicate rule
    /// and errors as `add_provider()`. The serial identifies this exact
    /// candidate (two generations may share the same id/version), so the hot
    /// lifecycle can remove the retired candidate after a commit.
    [[nodiscard]] auto add_provider_serial(provider_info info)
        -> result<std::uint64_t>;

    /// Register a candidate that may duplicate an existing
    /// `(service, id, version)` tuple and return its stable serial.
    ///
    /// This is the hot-epoch registration: a replacement generation of the same
    /// provider coexists with the running candidate until `commit_epoch()`
    /// switches the active slot to it. The serial lets the caller remove the
    /// retired candidate after the commit (`remove_provider()`), so the
    /// candidate table does not grow with every reload. Fails with
    /// `unsupported` once frozen or when the service is sealed.
    [[nodiscard]] auto add_staged_provider(provider_info info)
        -> result<std::uint64_t>;

    /// Drop a candidate by serial. Fails with `already_exists` while a
    /// committed active slot references it (retire the slot first); a serial
    /// that is not registered is `not_found`.
    [[nodiscard]] auto remove_provider(std::uint64_t serial) -> status;

    /// Activate `provider_id` for `service` on `instance` (the default instance
    /// when empty), or update the provider already active there.
    ///
    /// `provider_id` identifies the provider id (e.g. "flat@1"); `version`
    /// optionally disambiguates when multiple versions of the same id are
    /// registered (empty = the first registered candidate for that id).
    /// `instance` addresses the composition slot; for an `exclusive` service
    /// the upper resolver passes no instance, for an `instanced` service it
    /// passes the caller-visible instance name.
    ///
    /// Errors: `invalid_argument` on an empty service/provider id; `not_found`
    /// when no registered candidate matches (the instance does *not* need to
    /// pre-exist - selecting it creates/updates the slot); `unsupported` once
    /// frozen or when the service is sealed.
    [[nodiscard]] auto select_active(std::string_view service,
                                     std::string_view provider_id,
                                     std::string_view version = {},
                                     std::string_view instance = {}) -> status;

    /// Stage the activation of one registered candidate (addressed by its
    /// serial) for `(service, instance)`. Nothing is published until
    /// `commit_epoch()`. Errors: `invalid_argument` when the serial belongs to
    /// another service or the ids are empty, `not_found` for an unknown serial,
    /// `unsupported` once frozen / when sealed.
    [[nodiscard]] auto stage_active(std::uint64_t serial,
                                    std::string_view service,
                                    std::string_view instance = {}) -> status;

    /// Stage the activation of `provider_id` (the first registered candidate
    /// for the id/version tuple, same rule as `select_active()`). Convenience
    /// over `stage_active(serial, ...)` for callers that do not track serials.
    [[nodiscard]] auto stage_active(std::string_view service,
                                    std::string_view provider_id,
                                    std::string_view version = {},
                                    std::string_view instance = {}) -> status;

    /// Alias of `stage_retire()` for lifecycle callers that speak of retiring
    /// the active provider.
    [[nodiscard]] auto retire_active(std::string_view service,
                                     std::string_view instance = {}) -> status {
        return stage_retire(service, instance);
    }

    /// Stage the retirement of `(service, instance)`: after the next commit the
    /// slot has no active provider and the old generation is stale. The slot
    /// does not need to be active for the commit to be a no-op; an unknown
    /// service name is `not_found`.
    [[nodiscard]] auto stage_retire(std::string_view service,
                                    std::string_view instance = {}) -> status;

    /// True when at least one change is staged and waiting for a commit.
    [[nodiscard]] auto has_staged() const -> bool;

    /// Drop every staged change and quiesce marker without touching the
    /// committed composition. Used by a failed hot-install/reload rollback.
    void abort_staging();

    /// Publish every staged change atomically and advance the epoch once.
    ///
    /// Returns the committed epoch. A staged slot whose payload is published
    /// gets the next generation of that slot. For a slot published directly by
    /// `select_active()`, the commit only assigns the first generation it has
    /// not handed out yet, so the boot composition gets generation 1 in the
    /// first commit. Selection changes made since the previous commit are
    /// accounted for in the same epoch advance. A commit with nothing staged
    /// and no such pending change keeps the epoch unchanged.
    [[nodiscard]] auto commit_epoch() -> result<std::uint64_t>;

    /// Committed composition epoch; zero before the first commit.
    [[nodiscard]] auto epoch() const -> std::uint64_t;

    /// Seal one service: that service can never be staged, selected or retired
    /// again (host-owned runtime surfaces). Idempotent; sealing is permanent.
    [[nodiscard]] auto seal(std::string_view service) -> status;
    [[nodiscard]] auto is_sealed(std::string_view service) const -> bool;

    /// Copy of the active provider for `service` on `instance` (the default
    /// instance when `instance` is empty), or `std::nullopt` when that instance
    /// has no active provider. An unknown instance is simply absent: use
    /// `active_instances()` to distinguish "not active" from "typo".
    ///
    /// The copy carries the current generation of the slot. It is the
    /// weakly-checked form kept for boot/tooling callers; callers that cross a
    /// tick boundary use `query()`/`validate()` instead.
    [[nodiscard]] auto active_provider(std::string_view service,
                                       std::string_view instance = {}) const
        -> std::optional<provider_info>;

    /// Copy of the active provider plus the epoch/generation identity, or
    /// `stale` when the slot is quiescing and `not_found` when it is absent.
    [[nodiscard]] auto query(std::string_view service,
                             std::string_view instance = {}) const
        -> result<provider_handle>;

    /// Check a cached handle against the current composition: success when it
    /// is the current generation, `error_code::stale` when the slot was
    /// replaced or retired, is quiescing, or no longer exists.
    [[nodiscard]] auto validate(const provider_handle &handle) const -> status;

    /// Staged decision for one slot: `true` when an activation is staged,
    /// `false` when a retirement is staged, `nullopt` when the committed
    /// composition is unchanged.
    [[nodiscard]] auto staged_decision(std::string_view service,
                                       std::string_view instance = {}) const
        -> std::optional<bool>;

    /// Provider the staged view would expose for `(service, instance)`: the
    /// staged activation when one is pending, nothing when a retirement is
    /// pending, and the committed provider otherwise. Used to verify a
    /// composition plan *before* committing it.
    [[nodiscard]] auto
    staged_active_provider(std::string_view service,
                           std::string_view instance = {}) const
        -> std::optional<provider_info>;

    /// Current generation of the active provider for `(service, instance)`, or
    /// nullopt when the slot has no active provider. A slot whose initial
    /// composition has not been committed yet reports generation zero.
    [[nodiscard]] auto generation(std::string_view service,
                                  std::string_view instance = {}) const
        -> std::optional<std::uint64_t>;

    /// Sorted names of the instances with an active provider for `service`
    /// (empty when the service has none). This is the runtime view of the
    /// active composition: one name for `exclusive`, N names for `instanced`.
    [[nodiscard]] auto active_instances(std::string_view service) const
        -> std::vector<std::string>;

    /// Copy of a registered candidate. With an empty `version` this is the
    /// first registered candidate for the service/id tuple (registration-order
    /// deterministic; a registry never loses that information).
    [[nodiscard]] auto find(std::string_view service,
                            std::string_view provider_id,
                            std::string_view version = {}) const
        -> std::optional<provider_info>;

    [[nodiscard]] auto has_service(std::string_view service) const -> bool;

    /// All provider candidates for a service (copy; diagnostics/host boot).
    [[nodiscard]] auto providers(std::string_view service) const
        -> std::vector<provider_info>;

    /// Services with at least one registered provider, sorted.
    [[nodiscard]] auto services() const -> std::vector<std::string>;

    /// Services with at least one active provider instance, sorted.
    [[nodiscard]] auto active_services() const -> std::vector<std::string>;

    /// Human-readable registry dump (used by `refrain-tools gamepack dump`).
    /// Deterministic: candidates sorted by (service, id, version) and the
    /// active instances of each candidate are listed sorted.
    [[nodiscard]] auto dump() const -> std::string;

    /// Mark `(service, instance)` quiescing: new calls through
    /// `acquire()`/`query()` fail with `stale`, while outstanding leases keep
    /// working. Idempotent; `resume()` reverses it (rollback of a staged
    /// change). `not_found` when the slot is not active.
    [[nodiscard]] auto quiesce(std::string_view service,
                               std::string_view instance = {}) -> status;
    [[nodiscard]] auto resume(std::string_view service,
                              std::string_view instance = {}) -> status;

    /// Take an in-flight lease for the current generation of the slot. Fails
    /// with `stale` when the slot is quiescing.
    [[nodiscard]] auto acquire(std::string_view service,
                               std::string_view instance = {}) const
        -> result<provider_lease>;
    /// Lease for a validated handle; fails with `stale` exactly when
    /// `validate()` does.
    [[nodiscard]] auto acquire(const provider_handle &handle) const
        -> result<provider_lease>;

    /// Wait up to `timeout` for every lease of `(service, instance)` to be
    /// released. Returns `timeout` (naming the remaining calls) when the calls
    /// do not drain in time. The slot must be quiesced first or a new call can
    /// always arrive.
    [[nodiscard]] auto drain(std::string_view service,
                             std::string_view instance,
                             std::chrono::milliseconds timeout) const -> status;
    /// `drain()` of the reserved default instance.
    [[nodiscard]] auto drain(std::string_view service,
                             std::chrono::milliseconds timeout) const -> status;

    /// Outstanding leases of the current generation of a slot.
    [[nodiscard]] auto in_flight(std::string_view service,
                                 std::string_view instance = {}) const
        -> std::uint64_t;

    /// Drop all candidates and active instances. Rejected when frozen (freeze
    /// is permanent; a frozen registry can never be cleared back into a mutable
    /// one).
    [[nodiscard]] auto clear() -> status;

    /// Freeze the whole registry permanently: later mutations fail with
    /// `unsupported`. Idempotent and safe to call from an embedder that wants
    /// no hot lifecycle at all. Queries are unaffected.
    void freeze();

    [[nodiscard]] auto is_frozen() const -> bool;

    [[nodiscard]] auto provider_count() const -> std::size_t;

    /// Number of active `(service, instance)` slots across all services.
    [[nodiscard]] auto active_count() const -> std::size_t;

  private:
    using active_key = std::pair<std::string, std::string>;

    /// Active slot payload; the `(service, instance)` pair is the map key.
    struct active_entry {
        std::uint64_t serial{0};
        std::uint64_t generation{0};
    };

    struct candidate_entry {
        provider_info info{};
        std::uint64_t serial{0};
    };

    [[nodiscard]] static auto resolve_instance(std::string_view instance)
        -> std::string_view;

    /// `(service, instance)` -> candidate serial for a staged change; an
    /// engaged optional without a serial is a staged retirement.
    using staged_entry = std::optional<std::uint64_t>;
    using staged_map = std::map<active_key, staged_entry>;

    /// Empty instance strings address the reserved default composition slot.
    [[nodiscard]] static auto key_of(std::string_view service,
                                     std::string_view instance) -> active_key;

    /// Caller holds the lock.
    [[nodiscard]] auto find_candidate_locked(std::string_view service,
                                             std::string_view provider_id,
                                             std::string_view version) const
        -> const candidate_entry *;
    [[nodiscard]] auto find_serial_locked(std::uint64_t serial) const
        -> const candidate_entry *;
    [[nodiscard]] auto active_locked(const active_key &key) const
        -> const active_entry *;
    [[nodiscard]] auto provider_copy_locked(const active_entry &entry) const
        -> provider_info;
    [[nodiscard]] auto in_flight_locked(const active_key &key) const
        -> std::shared_ptr<std::atomic<std::uint64_t>>;

    mutable std::shared_mutex mutex_{};
    std::vector<candidate_entry> candidates_{};
    std::map<active_key, active_entry> active_{};
    /// Last generation handed out per slot; never decreases, so a reactivated
    /// slot cannot reuse an older generation number.
    std::map<active_key, std::uint64_t> slot_generations_{};
    staged_map staged_{};
    /// Slots published by an immediate `select_active()` that the next commit
    /// has to account for: it stamps a slot's first generation and advances the
    /// epoch.
    std::set<active_key> dirty_{};
    std::set<active_key> quiescing_{};
    std::set<std::string, std::less<>> sealed_services_{};
    mutable std::map<active_key, std::shared_ptr<std::atomic<std::uint64_t>>>
        in_flight_{};
    std::uint64_t epoch_{0};
    std::uint64_t next_serial_{1};
    bool frozen_{false};
};

} // namespace refrain::kernel
