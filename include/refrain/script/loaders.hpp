// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/script/host_api.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace refrain::mod {
class host;
} // namespace refrain::mod

namespace refrain::script {

// First-party mod loader providers for the script runtimes.
//
// A loader is host bootstrap code, not a mod: the composition root registers
// one provider instance per loader id on the host's instanced
// `refrain:mod.loader` service before the composition is activated, and the
// host dispatches every manifest naming that id through it.
//
// `wasm` executes a sandboxed WebAssembly module out of an installed mod
// package. `lua` is the reserved counterpart; until a Lua runtime is part of
// the repository it is registered as an explicit fail-closed provider so a
// manifest that names it is refused with the missing runtime named instead of
// the loader silently not existing.
//
// The wasm guest ABI is a core module with a small, versioned, capability-
// gated import surface: `refrain.log`, `refrain.config_get` and
// `refrain.tick`, described in `host_api.hpp`. A guest that imports anything
// else is refused before it runs. The module must export
// `on_load: () -> i32`, may export `on_unload: () -> i32` and may export
// callbacks with the `(i32, i32) -> i32` signature the host invokes after
// load. Guest state can only grow through the guest's own linear memory; there
// is no path from a guest to the provider table, the service registry, the
// filesystem or the network.

/// The generated `refrain:mod.loader` vtable plus the provider state pointer it
/// receives as `self`, and the contract ABI major the table was built against.
/// The host copies all three at registration, so a value has to stay valid for
/// the process lifetime (or at least for the host's lifetime).
struct mod_loader_provider {
    const void *vtable{nullptr};
    void *self{nullptr};
    std::uint32_t abi_major{0};
};

/// Provider triple for the sealed `refrain:script.host` service.
struct script_host_provider {
    const void *vtable{nullptr};
    void *self{nullptr};
    std::uint32_t abi_major{0};
};

namespace wasm {

namespace detail {
struct loader_state;
} // namespace detail

/// Guest ABI major a wasm mod is built against. It follows the host runtime
/// ABI major because the guest ABI only changes with the host; the manifest
/// `[mod].abi` is validated against it before any guest code runs.
inline constexpr std::uint32_t guest_abi_major{1};

/// Resource budgets one wasm guest may hold. A provider can be constructed with
/// tighter budgets (tests, resource-constrained deployments); the defaults are
/// what the embedded server uses.
struct loader_budgets {
    /// Largest accepted module member. A bigger package fails closed before
    /// compilation.
    std::uint64_t max_module_bytes{8ULL * 1024ULL * 1024ULL};
    /// Linear-memory ceiling enforced by the guest store: a guest memory that
    /// would grow past it fails inside the store instead of consuming host
    /// memory.
    std::uint64_t max_memory_bytes{64ULL * 1024ULL * 1024ULL};
    /// Instruction budget for the load path (instantiation, start function and
    /// the guest `on_load` hook). Exhaustion traps the guest and fails the
    /// load.
    std::uint64_t load_fuel{5'000'000};
    /// Wall-clock deadline for the whole load path. Guest instructions are
    /// bounded by `load_fuel` and compilation work by `max_module_bytes`, so
    /// the deadline is checked between load phases instead of interrupting a
    /// running compiler.
    std::uint64_t load_timeout_ms{5'000};
    /// Instruction budget for one dispatched guest call. It is provisioned
    /// from scratch before every call: the load path remainder and the previous
    /// call's remainder are not reused.
    std::uint64_t call_fuel{2'000'000};
    /// Wall-clock deadline for one dispatched guest call, enforced by the
    /// guest store's epoch counter.
    std::uint64_t call_timeout_ms{100};
};

/// Accounting of one wasm loader provider. The host has no way to observe a
/// guest's remaining budget, so the provider exposes it: a clean unload must
/// bring every live counter back to zero.
struct loader_stats {
    std::uint64_t live_instances{0};
    /// Compiled module bytes held by live instances.
    std::uint64_t live_module_bytes{0};
    /// Linear-memory budgets granted to live instances.
    std::uint64_t live_memory_bytes{0};
    /// Load-path instruction budget still available to live instances. Call
    /// budgets are provisioned per dispatch and are not part of this counter.
    std::uint64_t live_fuel{0};
    std::uint64_t total_loaded{0};
    std::uint64_t total_unloaded{0};
    /// Call dispatches attempted on live instances.
    std::uint64_t total_calls{0};
    /// Dispatches that returned an error, including budget and trap failures.
    std::uint64_t failed_calls{0};
    /// Live instances quarantined after a failed call. A quarantined guest
    /// runs no further callbacks and is released by unload.
    std::uint64_t quarantined_instances{0};
    /// Fuel left in the store after the most recent call dispatch. Zero when
    /// fuel accounting is disabled or the call exhausted its budget. Every
    /// dispatch provisions its own budget, so this value is not carried into
    /// the next call.
    std::uint64_t last_call_fuel_remaining{0};
};

/// Read-only host state and the grant set one loader's guests receive.
///
/// The default provider grants only `log`: the least a guest needs to report
/// anything, and the only import that touches no host state. A loader created
/// with a sealed environment can additionally grant `config` and `clock` to
/// its guests; an embedder does that when it owns the state the guest reads.
struct loader_host_api {
    host::grant_set grants{host::grant_set{} | host::capability::log};
    host::environment environment{};
};

/// One wasm loader provider with its own budgets and accounting. The provider
/// address is stable for the lifetime of the object; the host keeps the
/// pointer, so a loader registered with a host must outlive that host.
class loader {
  public:
    explicit loader(loader_budgets budgets = {}, loader_host_api host_api = {});
    loader(loader &&other) noexcept;
    auto operator=(loader &&other) noexcept -> loader &;
    ~loader();

    loader(const loader &) = delete;
    auto operator=(const loader &) -> loader & = delete;

    /// Provider triple to register with `mod::host::register_mod_loader()` as
    /// `wasm`. The returned pointers are owned by this object.
    [[nodiscard]] auto provider() const noexcept -> mod_loader_provider;

    /// Provider triple of the `refrain:script.host` counterpart of this
    /// loader: the process script host that dispatches callbacks into this
    /// loader's guests. Register it with `register_script_host()` while the
    /// host is still bootstrapping.
    [[nodiscard]] auto script_host_provider() const noexcept
        -> script_host_provider;

    /// Update the tick index `refrain.tick` reports to this loader's guests.
    /// The value is a plain snapshot read by guest imports, so the caller must
    /// marshal a live simulation tick onto the loader contract's main thread
    /// and call it between dispatches; the setter is not synchronised.
    void set_tick_index(std::int64_t tick_index) noexcept;

    [[nodiscard]] auto stats() const noexcept -> const loader_stats &;

  private:
    std::unique_ptr<detail::loader_state> state_;
};

/// The process-wide `wasm` provider the server composition registers. Its
/// state lives for the process lifetime; the provider is valid even when
/// Wasmtime is not compiled in, in which case every load fails closed with
/// `unsupported` and the build is named in the diagnostic.
[[nodiscard]] auto default_mod_loader_provider() noexcept
    -> mod_loader_provider;

/// The `refrain:script.host` counterpart of `default_mod_loader_provider()`.
/// Both resolve to the same provider state, so instance handles returned by the
/// default loader are dispatchable through the default script host.
[[nodiscard]] auto default_script_host_provider() noexcept
    -> script_host_provider;

} // namespace wasm

/// Register `provider` as the process `refrain:script.host` service on `host`.
/// The script host is sealed bootstrap code, so this is only legal before
/// `load()`; it is idempotent and keeps an already registered script host.
[[nodiscard]] auto register_script_host(mod::host &host,
                                        const script_host_provider &provider)
    -> status;

/// Outcome of one guest callback dispatched by the host.
struct guest_call_result {
    std::int32_t value{0};
};

/// Dispatch one guest callback through the sealed `refrain:script.host`
/// provider of `host`.
///
/// `instance` is the handle the mod loader returned for the guest, `callback`
/// is its exported function name and `arguments` is copied into the guest's
/// reserved argument prefix. The call runs under the loader's per-call
/// instruction budget and wall-clock deadline; a guest that exhausts either is
/// quarantined and must be unloaded. The returned error carries the loader's
/// diagnostic detail when the failure happened inside the guest.
[[nodiscard]] auto dispatch_guest_call(const mod::host &host,
                                       std::uint64_t instance,
                                       std::string_view callback,
                                       std::span<const std::byte> arguments)
    -> result<guest_call_result>;

namespace lua {

/// Fail-closed `lua` provider: no Lua runtime is part of the repository, so
/// this provider declares the loader's sandboxed/clean contract and refuses
/// every package that names it with `unsupported`, naming the missing runtime.
[[nodiscard]] auto default_mod_loader_provider() noexcept
    -> mod_loader_provider;

} // namespace lua

} // namespace refrain::script
