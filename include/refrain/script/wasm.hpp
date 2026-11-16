// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace refrain::script::wasm {

// Minimal Wasmtime embedding owned by the first-party script module. It
// provides engine, module, instance and call plumbing; the host API (WASI,
// capability sandbox, generated imports) builds on it. Compiled modules live as
// long as the runtime.
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

    [[nodiscard]] static auto create(engine_config config = {})
        -> result<runtime>;

    [[nodiscard]] auto compile_wat(std::string_view wat)
        -> result<module_handle>;
    [[nodiscard]] auto compile_bytes(std::span<const std::byte> wasm)
        -> result<module_handle>;

    /// Instantiate `module` in this runtime's store, run its `start` function
    /// when it has one and keep the instance for `call_instance_i32`. A store
    /// holds one guest: the store is the budget unit, so the loader creates one
    /// runtime per mod instance. Fails with `already_exists` when an instance
    /// is already live.
    [[nodiscard]] auto instantiate(module_handle module) -> status;

    /// True when the instantiated guest exports `export_name`.
    [[nodiscard]] auto has_export(std::string_view export_name) const -> bool;

    /// Instantiate `module` and call an exported `(i32, ...) -> i32` function
    /// in a fresh instance. Stateless convenience; a caller that must keep
    /// guest state uses `instantiate` + `call_instance_i32` instead.
    [[nodiscard]] auto call_i32(module_handle module,
                                std::string_view export_name,
                                std::span<const std::int32_t> args)
        -> result<std::int32_t>;

    /// Call an exported `(i32, ...) -> i32` function on the instance created by
    /// `instantiate`.
    [[nodiscard]] auto call_instance_i32(std::string_view export_name,
                                         std::span<const std::int32_t> args)
        -> result<std::int32_t>;

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
