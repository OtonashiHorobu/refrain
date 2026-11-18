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
#include <string>
#include <string_view>
#include <vector>

namespace refrain::script::wasm {

// Minimal Wasmtime embedding owned by the first-party script module. It
// provides engine, module, instance and call plumbing plus the sandboxed host
// import surface (`refrain.log`, `refrain.config_get`, `refrain.tick`) and the
// per-call fuel and wall-clock budgets the script host dispatches under.
// Compiled modules live as long as the runtime. There is no WASI, filesystem,
// network or component-model support here.
//
// One runtime is one engine + store + linker triple and therefore one budget
// unit: fuel and linear-memory limits are configured on the store at creation,
// and destroying the runtime releases every allocation the guest made. A
// loader that needs clean unload creates one runtime per guest and drops it
// when the guest is unloaded.

using module_handle = std::uint32_t;
inline constexpr module_handle invalid_module{0xFFFF'FFFFU};

struct engine_config {
    /// Count executed Wasm instructions and trap when `fuel` is exhausted
    /// (mandatory for untrusted scripts later).
    bool consume_fuel{false};
    std::uint64_t fuel{1'000'000};
    bool wasm_threads{false};
    bool memory64{false};
    /// Upper bound in bytes for one guest linear memory in this runtime's
    /// store. Zero keeps the store default (unlimited). A memory that would
    /// grow past the limit fails inside the guest store instead of consuming
    /// host memory.
    std::uint64_t max_memory_bytes{0};
    /// Enable epoch interruption so a call can be stopped by a wall-clock
    /// deadline. The runtime increments the engine epoch from its deadline
    /// thread when a call outlives `call_options::timeout_ms`; without this the
    /// per-call deadline has no effect and only the instruction budget bounds
    /// the guest.
    bool epoch_interruption{false};
};

/// Host imports one guest runtime exposes. A guest sees an import only when its
/// capability is granted and the state it reads was provided; an import outside
/// this set is refused with a typed error before instantiation.
struct host_imports {
    host::grant_set grants{};
    /// Read-only host state; must outlive the runtime. Null leaves the `config`
    /// and `clock` imports unavailable even when their caps are granted.
    const host::environment *environment{nullptr};
};

/// One log line a guest emitted through the `log` host import. The runtime
/// collects at most `host::max_log_messages_per_call` lines per call and hands
/// them to the host after the guest returns; a guest log line never enters the
/// host log directly, and the host prints it with its own component tag.
struct guest_log_entry {
    /// 0 trace, 1 debug, 2 info, 3 warn, 4 error.
    std::uint8_t level{2};
    std::string message{};
};

/// Budgets of one guest call after load. They are provisioned per call and
/// never shared with the load path: every dispatch starts with a fresh fuel
/// grant and a fresh wall-clock deadline.
struct call_options {
    /// Instructions granted to this call. Zero keeps the store's current fuel.
    std::uint64_t fuel{0};
    /// Wall-clock deadline in milliseconds. Zero disables the deadline; the
    /// runtime must have been created with `epoch_interruption` for it to be
    /// enforced.
    std::uint64_t timeout_ms{0};
};

/// Parse WebAssembly text (WAT) into the binary module format without
/// compiling it. Compilation is `runtime::compile_bytes`; the split exists so
/// tooling and tests can build module bytes from readable WAT.
[[nodiscard]] auto assemble_wat(std::string_view wat)
    -> result<std::vector<std::byte>>;

/// One engine + store + linker triple; move-only owner of compiled modules.
class runtime {
  public:
    runtime(runtime &&other) noexcept;
    auto operator=(runtime &&other) noexcept -> runtime &;
    ~runtime();

    runtime(const runtime &) = delete;
    auto operator=(const runtime &) -> runtime & = delete;

    [[nodiscard]] static auto create(engine_config config = {},
                                     host_imports imports = {})
        -> result<runtime>;

    [[nodiscard]] auto compile_wat(std::string_view wat)
        -> result<module_handle>;
    [[nodiscard]] auto compile_bytes(std::span<const std::byte> wasm)
        -> result<module_handle>;

    /// Instantiate `module` in this runtime's store, run its `start` function
    /// when it has one and keep the instance for `call_instance_i32`. A store
    /// holds one guest: the store is the budget unit, so the loader creates one
    /// runtime per mod instance. Fails with `already_exists` when an instance
    /// is already live. A module that imports anything outside the runtime's
    /// host import set is refused here with a typed error naming the import,
    /// the allowed imports and the active grant set.
    [[nodiscard]] auto instantiate(module_handle module) -> status;

    /// True when the instantiated guest exports `export_name`.
    [[nodiscard]] auto has_export(std::string_view export_name) const -> bool;

    /// Instantiate `module` and call an exported `(i32, ...) -> i32` function
    /// in a fresh instance. Stateless convenience; a caller that must keep
    /// guest state uses `instantiate` + `call_instance_i32` instead.
    [[nodiscard]] auto
    call_i32(module_handle module, std::string_view export_name,
             std::span<const std::int32_t> args, call_options options = {})
        -> result<std::int32_t>;

    /// Call an exported `(i32, ...) -> i32` function on the instance created by
    /// `instantiate`, under a per-call instruction budget and, when the runtime
    /// enables epoch interruption, a wall-clock deadline. A call that runs out
    /// of either budget traps and is reported as `timeout` (deadline) or with
    /// the store's fuel at zero (instruction budget); a guest trap is reported
    /// as `mod_error`. The store keeps no state from the previous call's
    /// budgets: the next call provisions its own.
    [[nodiscard]] auto call_instance_i32(std::string_view export_name,
                                         std::span<const std::int32_t> args,
                                         call_options options = {})
        -> result<std::int32_t>;

    /// Write `bytes` into the guest's exported linear memory at `offset`.
    /// Fails with `invalid_argument` when the range is outside the memory;
    /// never grows the memory.
    [[nodiscard]] auto write_guest_memory(std::uint64_t offset,
                                          std::span<const std::byte> bytes)
        -> status;

    /// Take the guest log lines collected since the last call. The host drains
    /// this after every guest invocation so a guest that logs through its host
    /// import is visible to the operator without giving it the host log sink.
    [[nodiscard]] auto take_guest_logs() -> std::vector<guest_log_entry>;

    /// Fuel left in the store after the guest's last instruction. Zero when
    /// fuel accounting is disabled or the guest exhausted its budget.
    [[nodiscard]] auto fuel_remaining() const noexcept -> std::uint64_t;

    /// Current size in bytes of the guest's exported `memory`. Zero when the
    /// module exports no linear memory or no instance is live.
    [[nodiscard]] auto memory_bytes() const noexcept -> std::uint64_t;

    [[nodiscard]] auto module_count() const noexcept -> std::size_t;

  private:
    struct impl;

    explicit runtime(std::unique_ptr<impl> state);

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::script::wasm
