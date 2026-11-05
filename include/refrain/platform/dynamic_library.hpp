// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace refrain::platform {

/// Native shared library (`dlopen`/`dlsym` on POSIX, `LoadLibrary` on Windows).
///
/// This is the **only** place in the engine that talks to the native loader.
/// Kernel/mod code depends on this abstraction so that a future WASM-only or
/// sandboxed loader can be swapped in, and so that tests can inject a fake.
class dynamic_library {
  public:
    [[nodiscard]] static auto load(std::string_view path)
        -> result<dynamic_library>;

    dynamic_library(dynamic_library &&other) noexcept;
    auto operator=(dynamic_library &&other) noexcept -> dynamic_library &;

    dynamic_library(const dynamic_library &) = delete;
    auto operator=(const dynamic_library &) = delete;

    ~dynamic_library();

    /// Resolve an exported symbol; `not_found` when the library does not export
    /// `name`, `mod_error` when the native loader reports a failure.
    [[nodiscard]] auto symbol(std::string_view name) const -> result<void *>;

    /// Typed variant: `symbol_as<int (*)(void)>("fn")`.
    template <typename T>
    [[nodiscard]] auto symbol_as(std::string_view name) const -> result<T> {
        auto raw = symbol(name);
        if (!raw) {
            return std::unexpected(std::move(raw.error()));
        }
        return reinterpret_cast<T>(*raw); // NOLINT
    }

    [[nodiscard]] auto path() const noexcept -> std::string_view;

    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Idempotent: closes the handle if it is still open.
    void unload() noexcept;

  private:
    dynamic_library(std::string path, void *handle) noexcept;

    std::string path_{};
    void *handle_{nullptr};
};

} // namespace refrain::platform
