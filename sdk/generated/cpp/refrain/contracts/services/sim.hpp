// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: a29acb77eba22b5e

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SIM_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SIM_HPP

#include <refrain/contracts/services/sim_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/contracts/event.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::sim {

enum class sim_error : std::uint32_t { // enum 'sim_error'
    invalid_handle = 1,
    already_open = 2,
    not_found = 3,
    io_error = 4,
    internal = 5,
};

namespace detail {
struct sim_world_tag;
} // namespace detail

using sim_world = ::refrain::contracts::handle<detail::sim_world_tag>;

/// Published once per tick, after the player/RNG step.
struct tick_event {
    std::int64_t tick;
};

static_assert(sizeof(tick_event) == 8);
static_assert(alignof(tick_event) == 8);
static_assert(offsetof(tick_event, tick) == 0);

inline constexpr std::string_view tick_event_id =
    "refrain:event/refrain/services/sim/tick_event";

using sim_config = ::refrain_services_sim_sim_config;
/// Zero-initialized `sim_config` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_sim_config() noexcept -> sim_config {
    sim_config value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(sim_config) == 32);
static_assert(alignof(sim_config) == 8);
static_assert(offsetof(sim_config, struct_size) == 0);
static_assert(offsetof(sim_config, world_path) == 8);
static_assert(offsetof(sim_config, seed) == 24);

inline constexpr std::string_view service_id = "refrain:sim";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::sim;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "tick,replay,deterministic-rng";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0,refrain:world>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:sim"};

/// Type-safe wrapper around `refrain_services_sim_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_sim_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_sim_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    [[nodiscard]] auto open_world(const ::refrain::services::sim::sim_config &config) const -> std::expected<::refrain::services::sim::sim_world, ::refrain::services::sim::sim_error> {
        const auto abi_config = ::refrain::contracts::with_struct_size(config);
        ::refrain_services_sim_handle_sim_world value{};
        if (const auto code = table_->open_world(self_, &abi_config, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return ::refrain::services::sim::sim_world{value};
    }

    [[nodiscard]] auto close_world(::refrain::services::sim::sim_world world) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->close_world(self_, world.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

    /// Advance exactly one tick.
    [[nodiscard]] auto tick(::refrain::services::sim::sim_world world) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->tick(self_, world.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto ticks(::refrain::services::sim::sim_world world) const -> std::int64_t {
        return static_cast<std::int64_t>(table_->ticks(self_, world.raw()));
    }

    /// Lowercase hex BLAKE3 of the deterministic replay state.
    [[nodiscard]] auto state_hash(::refrain::services::sim::sim_world world) const -> std::string_view {
        return ::refrain::contracts::as_string_view(table_->state_hash(self_, world.raw()));
    }

  private:
    const ::refrain_services_sim_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::sim

namespace refrain::contracts {

template <> struct event_traits<::refrain::services::sim::tick_event> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::sim::tick_event_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

} // namespace refrain::contracts

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SIM_HPP
