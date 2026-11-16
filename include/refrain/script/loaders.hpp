// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <memory>

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
// The wasm guest ABI of this slice is deliberately narrow: the module has no
// host imports at all, must export `on_load: () -> i32` and may export
// `on_unload: () -> i32`. A return value of zero reports success. Guest state
// can only grow through the guest's own linear memory; there is no path from a
// guest to the provider table, the service registry or any host service.

/// The generated `refrain:mod.loader` vtable plus the provider state pointer it
/// receives as `self`, and the contract ABI major the table was built against.
/// The host copies all three at registration, so a value has to stay valid for
/// the process lifetime (or at least for the host's lifetime).
struct mod_loader_provider {
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
    /// Instruction budget still available to live instances.
    std::uint64_t live_fuel{0};
    std::uint64_t total_loaded{0};
    std::uint64_t total_unloaded{0};
};

/// One wasm loader provider with its own budgets and accounting. The provider
/// address is stable for the lifetime of the object; the host keeps the
/// pointer, so a loader registered with a host must outlive that host.
class loader {
  public:
    explicit loader(loader_budgets budgets = {});
    loader(loader &&other) noexcept;
    auto operator=(loader &&other) noexcept -> loader &;
    ~loader();

    loader(const loader &) = delete;
    auto operator=(const loader &) -> loader & = delete;

    /// Provider triple to register with `mod::host::register_mod_loader()` as
    /// `wasm`. The returned pointers are owned by this object.
    [[nodiscard]] auto provider() const noexcept -> mod_loader_provider;

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

} // namespace wasm

namespace lua {

/// Fail-closed `lua` provider: no Lua runtime is part of the repository, so
/// this provider declares the loader's sandboxed/clean contract and refuses
/// every package that names it with `unsupported`, naming the missing runtime.
[[nodiscard]] auto default_mod_loader_provider() noexcept
    -> mod_loader_provider;

} // namespace lua

} // namespace refrain::script
