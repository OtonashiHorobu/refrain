// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 927eab4ffcbbd2c4

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_HPP

#include <refrain/contracts/services/net_session_abi.h>
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

namespace refrain::services::net_session {

enum class session_error : std::uint32_t { // enum 'session_error'
    invalid_argument = 1,
    not_joined = 2,
    unavailable = 3,
    malformed_profile = 4,
};

using session_profile = ::refrain_services_net_session_session_profile;
/// Zero-initialized `session_profile` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_session_profile() noexcept -> session_profile {
    session_profile value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(session_profile) == 56);
static_assert(alignof(session_profile) == 8);
static_assert(offsetof(session_profile, struct_size) == 0);
static_assert(offsetof(session_profile, id) == 8);
static_assert(offsetof(session_profile, name) == 24);
static_assert(offsetof(session_profile, properties) == 40);

inline constexpr std::string_view service_id = "refrain:net.session";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "offline-profile,online-profile";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:net.session"};

/// Type-safe wrapper around `refrain_services_net_session_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_net_session_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_net_session_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    [[nodiscard]] auto offline_profile(std::string_view name, ::refrain::services::net_session::session_profile &profile) const -> std::expected<void, ::refrain::services::net_session::session_error> {
        ::refrain::contracts::init_struct(profile);
        if (const auto code = table_->offline_profile(self_, ::refrain::contracts::to_borrowed(name), &profile); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_session::session_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto joined_profile(std::string_view name, std::string_view server_hash, std::string_view remote_ip, ::refrain::services::net_session::session_profile &profile) const -> std::expected<void, ::refrain::services::net_session::session_error> {
        ::refrain::contracts::init_struct(profile);
        if (const auto code = table_->joined_profile(self_, ::refrain::contracts::to_borrowed(name), ::refrain::contracts::to_borrowed(server_hash), ::refrain::contracts::to_borrowed(remote_ip), &profile); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_session::session_error>(code));
        }
        return {};
    }

  private:
    const ::refrain_services_net_session_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::net_session

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_HPP
