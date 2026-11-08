// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 5d21cfe923511284

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_RUNTIME_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_RUNTIME_HPP

#include <refrain/contracts/services/runtime_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::runtime {

enum class runtime_error : std::uint32_t { // enum 'runtime_error'
    invalid_config = 1,
    io_error = 2,
    internal = 3,
};

/// Process-wide runtime configuration assembled by the host from the parsed
/// CLI. Strings are borrowed for the duration of `run`; the provider copies
/// any value it needs to keep.
using runtime_config = ::refrain_services_runtime_runtime_config;
/// Zero-initialized `runtime_config` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_runtime_config() noexcept -> runtime_config {
    runtime_config value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(runtime_config) == 184);
static_assert(alignof(runtime_config) == 8);
static_assert(offsetof(runtime_config, struct_size) == 0);
static_assert(offsetof(runtime_config, world_path) == 8);
static_assert(offsetof(runtime_config, ticks) == 24);
static_assert(offsetof(runtime_config, command_file) == 32);
static_assert(offsetof(runtime_config, script_file) == 48);
static_assert(offsetof(runtime_config, hash_every) == 64);
static_assert(offsetof(runtime_config, world_zstd_level) == 72);
static_assert(offsetof(runtime_config, zstd_near_level) == 76);
static_assert(offsetof(runtime_config, zstd_mid_level) == 80);
static_assert(offsetof(runtime_config, zstd_far_level) == 84);
static_assert(offsetof(runtime_config, zstd_near_radius) == 88);
static_assert(offsetof(runtime_config, zstd_mid_radius) == 92);
static_assert(offsetof(runtime_config, watchdog_timeout_ms) == 96);
static_assert(offsetof(runtime_config, autosave_interval_ticks) == 104);
static_assert(offsetof(runtime_config, compact_interval_ticks) == 112);
static_assert(offsetof(runtime_config, compact_min_bytes) == 120);
static_assert(offsetof(runtime_config, compact_garbage_percent) == 128);
static_assert(offsetof(runtime_config, store_compact_interval_ticks) == 136);
static_assert(offsetof(runtime_config, content_store) == 144);
static_assert(offsetof(runtime_config, content_gc_apply) == 160);
static_assert(offsetof(runtime_config, content_gc_keep_days) == 164);
static_assert(offsetof(runtime_config, content_gc_budget_ms) == 168);
static_assert(offsetof(runtime_config, content_gc_interval_ticks) == 176);

inline constexpr std::string_view service_id = "refrain:runtime";
inline constexpr std::uint32_t abi_major = 2;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::main;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "tick,console";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:runtime"};

/// Type-safe wrapper around `refrain_services_runtime_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_runtime_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_runtime_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Start the headless server runtime and block until it stops (console
    /// `stop`, the tick limit, EOF, or a fatal runtime error). Returns the
    /// process exit code: 0 for a clean stop, non-zero for a failed startup or
    /// an unclean shutdown. `runtime_error` is reserved for calls the provider
    /// rejects before the tick loop starts (for example an invalid config);
    /// once the loop owns the process, the exit code is the authoritative
    /// result.
    [[nodiscard]] auto run(const ::refrain::services::runtime::runtime_config &config) const -> std::expected<std::int32_t, ::refrain::services::runtime::runtime_error> {
        const auto abi_config = ::refrain::contracts::with_struct_size(config);
        ::int32_t value{};
        if (const auto code = table_->run(self_, &abi_config, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::runtime::runtime_error>(code));
        }
        return static_cast<std::int32_t>(value);
    }

  private:
    const ::refrain_services_runtime_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::runtime

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_RUNTIME_HPP
