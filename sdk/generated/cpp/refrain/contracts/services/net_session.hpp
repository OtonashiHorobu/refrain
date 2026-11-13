// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: e168f1954a0feabb

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
#include <refrain/contracts/event.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::net_session {

enum class session_error : std::uint32_t { // enum 'session_error'
    invalid_argument = 1,
    not_joined = 2,
    unavailable = 3,
    malformed_profile = 4,
    not_found = 5,
    already_open = 6,
    unsupported = 7,
};

namespace detail {
struct session_menu_tag;
} // namespace detail

using session_menu = ::refrain::contracts::handle<detail::session_menu_tag>;

/// A connection opened a menu.
struct menu_opened {
    /// Raw `session_menu` handle.
    std::uint64_t menu;
    std::uint64_t connection;
    /// Container id; 0 is the player inventory.
    std::uint32_t container;
    std::uint8_t reserved_0[4];
};

static_assert(sizeof(menu_opened) == 24);
static_assert(alignof(menu_opened) == 8);
static_assert(offsetof(menu_opened, menu) == 0);
static_assert(offsetof(menu_opened, connection) == 8);
static_assert(offsetof(menu_opened, container) == 16);
inline constexpr std::string_view menu_opened_id =
    "refrain:event/refrain/services/net_session/menu_opened";
inline constexpr std::uint64_t menu_opened_type = REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_OPENED_TYPE;

/// A menu closed.
struct menu_closed {
    std::uint64_t menu;
    std::uint64_t connection;
};

static_assert(sizeof(menu_closed) == 16);
static_assert(alignof(menu_closed) == 8);
static_assert(offsetof(menu_closed, menu) == 0);
static_assert(offsetof(menu_closed, connection) == 8);
inline constexpr std::string_view menu_closed_id =
    "refrain:event/refrain/services/net_session/menu_closed";
inline constexpr std::uint64_t menu_closed_type = REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLOSED_TYPE;

/// One slot of an open menu changed.
struct menu_slot_changed {
    std::uint64_t menu;
    std::int32_t slot;
    std::uint32_t count;
    std::string_view item;
};

static_assert(sizeof(menu_slot_changed) == 32);
static_assert(alignof(menu_slot_changed) == 8);
static_assert(offsetof(menu_slot_changed, menu) == 0);
static_assert(offsetof(menu_slot_changed, slot) == 8);
static_assert(offsetof(menu_slot_changed, count) == 12);
static_assert(offsetof(menu_slot_changed, item) == 16);
inline constexpr std::string_view menu_slot_changed_id =
    "refrain:event/refrain/services/net_session/menu_slot_changed";
inline constexpr std::uint64_t menu_slot_changed_type = REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_SLOT_CHANGED_TYPE;

/// A mouse click was applied to an open menu.
struct menu_clicked {
    std::uint64_t menu;
    std::int32_t slot;
    std::int32_t button;
    bool accepted;
    std::uint8_t reserved_0[3];
    std::uint32_t slot_count;
    std::string_view slot_item;
    std::uint32_t carried_count;
    std::uint8_t reserved_1[4];
    std::string_view carried_item;
};

static_assert(sizeof(menu_clicked) == 64);
static_assert(alignof(menu_clicked) == 8);
static_assert(offsetof(menu_clicked, menu) == 0);
static_assert(offsetof(menu_clicked, slot) == 8);
static_assert(offsetof(menu_clicked, button) == 12);
static_assert(offsetof(menu_clicked, accepted) == 16);
static_assert(offsetof(menu_clicked, slot_count) == 20);
static_assert(offsetof(menu_clicked, slot_item) == 24);
static_assert(offsetof(menu_clicked, carried_count) == 40);
static_assert(offsetof(menu_clicked, carried_item) == 48);
inline constexpr std::string_view menu_clicked_id =
    "refrain:event/refrain/services/net_session/menu_clicked";
inline constexpr std::uint64_t menu_clicked_type = REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLICKED_TYPE;

using session_profile = ::refrain_services_net_session_session_profile;
/// Zero-initialized `session_profile` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_session_profile() noexcept -> session_profile {
    session_profile value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Outcome of one mouse click on an open menu.
/// 
/// The model owns the carried stack, like the client does: a click either
/// picks a stack up, puts it down, merges it or swaps it. `accepted` is false
/// when the click changed nothing, in which case the reported slot and carried
/// contents are the unchanged state.
using menu_click_result = ::refrain_services_net_session_menu_click_result;
/// Zero-initialized `menu_click_result` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_menu_click_result() noexcept -> menu_click_result {
    menu_click_result value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(session_profile) == 56);
static_assert(alignof(session_profile) == 8);
static_assert(offsetof(session_profile, struct_size) == 0);
static_assert(offsetof(session_profile, id) == 8);
static_assert(offsetof(session_profile, name) == 24);
static_assert(offsetof(session_profile, properties) == 40);
static_assert(sizeof(menu_click_result) == 64);
static_assert(alignof(menu_click_result) == 8);
static_assert(offsetof(menu_click_result, struct_size) == 0);
static_assert(offsetof(menu_click_result, slot) == 4);
static_assert(offsetof(menu_click_result, button) == 8);
static_assert(offsetof(menu_click_result, accepted) == 12);
static_assert(offsetof(menu_click_result, slot_count) == 16);
static_assert(offsetof(menu_click_result, slot_item) == 24);
static_assert(offsetof(menu_click_result, carried_count) == 40);
static_assert(offsetof(menu_click_result, carried_item) == 48);

inline constexpr std::string_view service_id = "refrain:net.session";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "offline-profile,online-profile,container-menus";
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

    /// Open a container menu for a connection and publish `menu_opened`.
    /// Fails with `already_open` when the connection already has a menu and
    /// with `unsupported` for a container the model does not implement.
    [[nodiscard]] auto menu_open(std::uint64_t connection, std::uint32_t container) const -> std::expected<::refrain::services::net_session::session_menu, ::refrain::services::net_session::session_error> {
        ::refrain_services_net_session_handle_session_menu value{};
        if (const auto code = table_->menu_open(self_, connection, container, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_session::session_error>(code));
        }
        return ::refrain::services::net_session::session_menu{value};
    }

    /// Close a menu and publish `menu_closed`. Slots are discarded.
    [[nodiscard]] auto menu_close(::refrain::services::net_session::session_menu menu) const -> std::expected<void, ::refrain::services::net_session::session_error> {
        if (const auto code = table_->menu_close(self_, menu.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_session::session_error>(code));
        }
        return {};
    }

    /// Replace one slot and publish `menu_slot_changed` when the item or the
    /// count differs. An empty `item` or a zero `count` clears the slot.
    [[nodiscard]] auto menu_set_slot(::refrain::services::net_session::session_menu menu, std::int32_t slot, std::string_view item, std::uint32_t count) const -> std::expected<bool, ::refrain::services::net_session::session_error> {
        ::uint8_t value{};
        if (const auto code = table_->menu_set_slot(self_, menu.raw(), slot, ::refrain::contracts::to_borrowed(item), count, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_session::session_error>(code));
        }
        return (value != 0);
    }

    /// Apply one mouse click, publish `menu_clicked` and fill `result` with
    /// the transmitted slot and carried state.
    [[nodiscard]] auto menu_click(::refrain::services::net_session::session_menu menu, std::int32_t slot, std::int32_t button, ::refrain::services::net_session::menu_click_result &result) const -> std::expected<void, ::refrain::services::net_session::session_error> {
        ::refrain::contracts::init_struct(result);
        if (const auto code = table_->menu_click(self_, menu.raw(), slot, button, &result); code != REFRAIN_STATUS_OK) {
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

namespace refrain::contracts {

template <> struct event_traits<::refrain::services::net_session::menu_opened> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::net_session::menu_opened_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

template <> struct event_traits<::refrain::services::net_session::menu_closed> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::net_session::menu_closed_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

template <> struct event_traits<::refrain::services::net_session::menu_slot_changed> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::net_session::menu_slot_changed_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

template <> struct event_traits<::refrain::services::net_session::menu_clicked> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::net_session::menu_clicked_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

} // namespace refrain::contracts

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_HPP
