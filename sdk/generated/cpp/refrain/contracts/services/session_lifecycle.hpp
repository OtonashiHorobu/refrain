// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 927eab4ffcbbd2c4

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_LIFECYCLE_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_LIFECYCLE_HPP

#include <refrain/contracts/services/session_lifecycle_abi.h>
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

namespace refrain::services::session_lifecycle {

using player_info = ::refrain_services_session_lifecycle_player_info;
/// Zero-initialized `player_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_player_info() noexcept -> player_info {
    player_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(player_info) == 48);
static_assert(alignof(player_info) == 8);
static_assert(offsetof(player_info, struct_size) == 0);
static_assert(offsetof(player_info, connection) == 8);
static_assert(offsetof(player_info, name) == 16);
static_assert(offsetof(player_info, world) == 32);

inline constexpr std::string_view service_id = "refrain:session.lifecycle";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::sim;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::instanced;
inline constexpr std::string_view capabilities = "player-lifecycle";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:session.lifecycle"};

/// Type-safe wrapper around `refrain_services_session_lifecycle_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_session_lifecycle_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_session_lifecycle_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    auto player_join(const ::refrain::services::session_lifecycle::player_info &player) const -> void {
        const auto abi_player = ::refrain::contracts::with_struct_size(player);
        table_->player_join(self_, &abi_player);
    }

    auto player_leave(const ::refrain::services::session_lifecycle::player_info &player) const -> void {
        const auto abi_player = ::refrain::contracts::with_struct_size(player);
        table_->player_leave(self_, &abi_player);
    }

    /// Called once per simulation tick after the world advanced.
    auto tick(std::uint64_t tick) const -> void {
        table_->tick(self_, tick);
    }

  private:
    const ::refrain_services_session_lifecycle_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::session_lifecycle

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SESSION_LIFECYCLE_HPP
