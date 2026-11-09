// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/core.hpp>
#include <refrain/core/error.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace refrain::host {

// Abstract host runtime context behind the sealed `refrain:host.core`
// service. The composition root provides the concrete implementation; the
// ABI adapter in `src/host/core_api.cpp` only depends on this interface, so a
// test can supply a fake without linking app/mod code.
//
// Ownership: returned `string_view`s must stay valid for the documented
// lifetime (config snapshots until the next reload; diagnostics until the
// next host call). `alloc` returns host-owned memory released by `free`.
class context {
  public:
    context() = default;
    context(const context &) = delete;
    auto operator=(const context &) -> context & = delete;
    context(context &&) = delete;
    auto operator=(context &&) -> context & = delete;
    virtual ~context() = default;

    virtual void log(std::uint32_t level,
                     std::string_view message) noexcept = 0;
    [[nodiscard]] virtual auto now_ns() const noexcept -> std::int64_t = 0;
    [[nodiscard]] virtual auto tick_now() const noexcept -> std::int64_t = 0;
    virtual void version(core::host_version &out) const noexcept = 0;

    [[nodiscard]] virtual auto config_i64(std::string_view key)
        -> result<std::int64_t> = 0;
    [[nodiscard]] virtual auto config_bool(std::string_view key)
        -> result<bool> = 0;
    [[nodiscard]] virtual auto config_string(std::string_view key)
        -> result<std::string_view> = 0;

    [[nodiscard]] virtual auto query_service(std::string_view service,
                                             std::string_view instance)
        -> result<core::provider_ref> = 0;
    [[nodiscard]] virtual auto register_provider(std::string_view service,
                                                 std::string_view instance,
                                                 const core::provider_ref &ref)
        -> status = 0;

    [[nodiscard]] virtual auto alloc(std::uint64_t size,
                                     std::uint64_t alignment)
        -> result<std::span<std::uint8_t>> = 0;
    virtual void free(std::span<std::uint8_t> block) noexcept = 0;
};

} // namespace refrain::host
