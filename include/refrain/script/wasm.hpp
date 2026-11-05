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

namespace refrain::script::wasm {

// Minimal Wasmtime embedding owned by the first-party script module. It
// provides engine, module, instance and call plumbing; the host API (WASI,
// capability sandbox, generated imports) builds on it. Compiled modules live as
// long as the runtime.

using module_handle = std::uint32_t;
inline constexpr module_handle invalid_module{0xFFFF'FFFFU};

struct engine_config {
    /// Count executed Wasm instructions and trap when `fuel` is exhausted
    /// (mandatory for untrusted scripts later).
    bool consume_fuel{false};
    std::uint64_t fuel{1'000'000};
    bool wasm_threads{false};
    bool memory64{false};
};

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

    /// Instantiate `module` and call an exported `(i32, ...) -> i32` function.
    [[nodiscard]] auto call_i32(module_handle module,
                                std::string_view export_name,
                                std::span<const std::int32_t> args)
        -> result<std::int32_t>;

    [[nodiscard]] auto module_count() const noexcept -> std::size_t;

  private:
    struct impl;

    explicit runtime(std::unique_ptr<impl> state);

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::script::wasm
