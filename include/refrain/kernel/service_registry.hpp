// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
/// Freeze: after boot/assembly the composition root calls `freeze()`. From then
/// on every mutation (`add_provider()`, `select_active()`, `clear()`) fails
/// with `error_code::unsupported` and leaves the registry unchanged; queries
/// keep working. `freeze()` is idempotent and permanent (the composition root
/// relies on an immutable registry after assembly). Error choice rationale: the
/// operation is not supported in the frozen lifecycle state, not a naming
/// collision (`already_exists`), so callers can distinguish a frozen registry
/// from a duplicate registration.
///
/// Thread safety: every operation is internally synchronized; queries return
/// *copies* (`std::optional<provider_info>`, `std::vector<...>`), never
/// pointers into the registry storage, so a concurrent
/// `add_provider()`/`clear()` can never dangle a caller-held pointer.
/// `provider_info` is a handful of small strings, so the copy cost is noise for
/// the boot/assembly paths this serves. Active providers are stored per
/// `(service, instance)` pair, so a query never observes a half-updated pair.
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
    /// service/id/version tuples and with `unsupported` once frozen.
    [[nodiscard]] auto add_provider(provider_info info) -> status;

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
    /// frozen.
    [[nodiscard]] auto select_active(std::string_view service,
                                     std::string_view provider_id,
                                     std::string_view version = {},
                                     std::string_view instance = {}) -> status;

    /// Copy of the active provider for `service` on `instance` (the default
    /// instance when `instance` is empty), or `std::nullopt` when that instance
    /// has no active provider. An unknown instance is simply absent: use
    /// `active_instances()` to distinguish "not active" from "typo".
    [[nodiscard]] auto active_provider(std::string_view service,
                                       std::string_view instance = {}) const
        -> std::optional<provider_info>;

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

    /// Drop all candidates and active instances.
    ///
    /// Errors: `unsupported` once frozen (freeze is permanent; a frozen
    /// registry can never be cleared back into a mutable one).
    [[nodiscard]] auto clear() -> status;

    /// Freeze the registry's active table and candidate list: later mutations
    /// fail with `unsupported`. Idempotent and permanent; safe to call from the
    /// composition root after boot. Queries are unaffected.
    void freeze();

    [[nodiscard]] auto is_frozen() const -> bool;

    [[nodiscard]] auto provider_count() const -> std::size_t;

    /// Number of active `(service, instance)` slots across all services.
    [[nodiscard]] auto active_count() const -> std::size_t;

  private:
    /// Active slot payload; the `(service, instance)` pair is the map key.
    struct active_entry {
        std::string provider_id{};
        std::string version{};
    };

    /// (service, instance) -> active provider identity. `std::map` keeps the
    /// iteration order deterministic (service-major, then instance) for
    /// `dump()`/`active_services()`.
    using active_key = std::pair<std::string, std::string>;
    using active_map = std::map<active_key, active_entry>;

    mutable std::shared_mutex mutex_{};
    std::vector<provider_info> providers_{};
    active_map active_{};
    bool frozen_{false};
};

} // namespace refrain::kernel
