// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:net.session` provider: deterministic offline profiles, the online
// `hasJoined` lookup and the container-menu model. The network server consumes
// the profile calls as an optional authentication seam; the menu calls are the
// session model a mod observes through the published menu events.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/host/kernel.hpp>
#include <refrain/contracts/services/net_session.hpp>
#include <refrain/contracts/services/net_session_abi.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/net/auth.hpp>
#include <refrain/server/inventory_menu.hpp>
#include <refrain/sim/player.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using refrain::contracts::as_string_view;
using refrain::contracts::to_borrowed;

namespace session_service = refrain::services::net_session;

/// The only container the model implements: the player inventory menu, whose
/// slots 9..44 project onto the 36 player inventory slots.
constexpr std::uint32_t player_menu_container{0};

/// One open container menu: the connection that owns it and the modeled slots.
struct menu_state {
    std::uint64_t connection{0};
    std::uint32_t container{player_menu_container};
    std::vector<refrain::sim::item_stack> inventory{};
    /// Stack attached to the cursor, owned by the model like the client does.
    refrain::sim::item_stack carried{};
};

struct session_state {
    std::mutex mutex{};
    std::unique_ptr<refrain::net::http_transport> transport{};
    std::unique_ptr<refrain::net::auth_service> auth{};
    std::string current_id{};
    std::string current_name{};
    std::vector<std::byte> current_properties{};

    /// Menu state has its own lock: an online profile lookup must not stall a
    /// tick that opens or clicks a menu.
    std::mutex menu_mutex{};
    std::map<std::uint64_t, menu_state> menus{};
    std::map<std::uint64_t, std::uint64_t> menu_of_connection{};
    std::uint64_t next_menu{1};
    /// Strings borrowed by the last result/event payload; the sim thread is
    /// the only writer, so a caller reads them before the next menu call.
    std::string current_slot_item{};
    std::string current_carried_item{};

    /// Sealed host kernel provider used to publish the C ABI payload. Empty
    /// when the composition registers no kernel service.
    const refrain_host_kernel_vtable *kernel_vtable{nullptr};
    void *kernel_self{nullptr};
};

/// Hand one generated C payload to the C ABI subscribers when the composition
/// offers the host kernel. The payload is built field by field at each call
/// site, so no C++ layout (a `std::string_view` is not `{ data, len }`) crosses
/// the boundary.
void publish_bytes(session_state &state, std::uint64_t type,
                   const void *payload, std::size_t size) {
    if (state.kernel_vtable == nullptr || state.kernel_self == nullptr) {
        return;
    }
    const refrain::host::kernel::provider kernel{*state.kernel_vtable,
                                                 state.kernel_self};
    kernel.event_publish(type, const_cast<void *>(payload),
                         static_cast<std::uint64_t>(size));
}

auto offline_error(const refrain::error &error)
    -> refrain_services_net_session_session_error {
    if (error.code == refrain::error_code::invalid_argument) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNAVAILABLE;
}

auto online_error(refrain::net::auth_error error)
    -> refrain_services_net_session_session_error {
    using refrain::net::auth_error;
    switch (error) {
    case auth_error::invalid_argument:
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    case auth_error::not_joined:
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_NOT_JOINED;
    case auth_error::malformed_response:
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_MALFORMED_PROFILE;
    default:
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNAVAILABLE;
    }
}

auto fill_profile(session_state &state, const refrain::net::profile &profile,
                  refrain_services_net_session_session_profile *out) -> void {
    state.current_id = profile.id.to_string();
    state.current_name = profile.name;
    state.current_properties = profile.properties;
    out->struct_size = REFRAIN_SERVICES_NET_SESSION_SESSION_PROFILE_SIZE;
    out->id = to_borrowed(state.current_id);
    out->name = to_borrowed(state.current_name);
    out->properties = refrain_bytes{
        reinterpret_cast<const std::uint8_t *>(state.current_properties.data()),
        state.current_properties.size()};
}

auto offline_profile(void *self, refrain_str name,
                     refrain_services_net_session_session_profile *out)
    -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<session_state *>(self)};
    auto profile{refrain::net::offline_profile(as_string_view(name))};
    if (!profile) {
        return offline_error(profile.error());
    }
    const std::lock_guard lock{state.mutex};
    fill_profile(state, *profile, out);
    return REFRAIN_STATUS_OK;
}

auto joined_profile(void *self, refrain_str name, refrain_str server_hash,
                    refrain_str remote_ip,
                    refrain_services_net_session_session_profile *out)
    -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<session_state *>(self)};
    const std::lock_guard lock{state.mutex};
    if (!state.auth) {
        auto transport{refrain::net::make_curl_transport()};
        if (!transport) {
            return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNAVAILABLE;
        }
        state.transport = std::move(*transport);
        state.auth =
            std::make_unique<refrain::net::auth_service>(*state.transport);
    }
    auto profile{state.auth->has_joined(as_string_view(name),
                                        as_string_view(server_hash),
                                        as_string_view(remote_ip))};
    if (!profile) {
        const auto reason{refrain::net::auth_error_of(profile.error())};
        if (reason) {
            return online_error(*reason);
        }
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNAVAILABLE;
    }
    fill_profile(state, *profile, out);
    return REFRAIN_STATUS_OK;
}

/// Menu slot -> inventory index, or `no_inventory_index` when the slot is not
/// part of the modeled player menu.
[[nodiscard]] auto slot_index(std::int32_t slot) -> std::size_t {
    if (slot < std::numeric_limits<std::int16_t>::min() ||
        slot > std::numeric_limits<std::int16_t>::max()) {
        return refrain::server::no_inventory_index;
    }
    return refrain::server::inventory_index_of(static_cast<std::int16_t>(slot));
}

[[nodiscard]] auto find_menu(session_state &state, std::uint64_t raw)
    -> menu_state * {
    const auto found{state.menus.find(raw)};
    return found == state.menus.end() ? nullptr : &found->second;
}

auto menu_open(void *self, std::uint64_t connection, std::uint32_t container,
               refrain_services_net_session_handle_session_menu *out_value)
    -> refrain_status {
    if (self == nullptr || out_value == nullptr) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<session_state *>(self)};
    // TODO: containers other than the player menu need a slot-layout registry;
    // the model rejects them until the container table exists.
    if (container != player_menu_container) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNSUPPORTED;
    }
    std::uint64_t menu{0};
    {
        const std::lock_guard lock{state.menu_mutex};
        if (state.menu_of_connection.contains(connection)) {
            return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_ALREADY_OPEN;
        }
        menu = state.next_menu++;
        state.menus.emplace(
            menu, menu_state{.connection = connection, .container = container});
        state.menu_of_connection.emplace(connection, menu);
    }
    refrain_services_net_session_menu_opened payload{};
    payload.menu = menu;
    payload.connection = connection;
    payload.container = container;
    publish_bytes(state, session_service::menu_opened_type, &payload,
                  sizeof(payload));
    *out_value = menu;
    return REFRAIN_STATUS_OK;
}

auto menu_close(void *self,
                refrain_services_net_session_handle_session_menu menu)
    -> refrain_status {
    if (self == nullptr) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<session_state *>(self)};
    std::uint64_t connection{0};
    {
        const std::lock_guard lock{state.menu_mutex};
        const auto found{state.menus.find(menu)};
        if (found == state.menus.end()) {
            return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_NOT_FOUND;
        }
        connection = found->second.connection;
        state.menu_of_connection.erase(connection);
        state.menus.erase(found);
    }
    refrain_services_net_session_menu_closed payload{};
    payload.menu = menu;
    payload.connection = connection;
    publish_bytes(state, session_service::menu_closed_type, &payload,
                  sizeof(payload));
    return REFRAIN_STATUS_OK;
}

auto menu_set_slot(void *self,
                   refrain_services_net_session_handle_session_menu menu,
                   std::int32_t slot, refrain_str item, std::uint32_t count,
                   std::uint8_t *out_value) -> refrain_status {
    if (self == nullptr || out_value == nullptr) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<session_state *>(self)};
    const auto index{slot_index(slot)};
    if (index == refrain::server::no_inventory_index) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    const auto item_view{as_string_view(item)};
    const auto clears{item_view.empty() || count == 0};
    if (!clears && count > refrain::sim::absolute_stack_size) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    bool changed{false};
    {
        const std::lock_guard lock{state.menu_mutex};
        auto *open{find_menu(state, menu)};
        if (open == nullptr) {
            return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_NOT_FOUND;
        }
        if (open->inventory.size() <= index) {
            open->inventory.resize(index + 1);
        }
        auto &stack{open->inventory[index]};
        const std::uint32_t target_count{clears ? 0U : count};
        changed = stack.item != item_view || stack.count != target_count;
        if (changed) {
            stack.item = clears ? std::string{} : std::string{item_view};
            stack.count = target_count;
        }
        state.current_slot_item = stack.item;
    }
    if (changed) {
        refrain_services_net_session_menu_slot_changed payload{};
        payload.menu = menu;
        payload.slot = slot;
        payload.count = clears ? 0U : count;
        payload.item = to_borrowed(state.current_slot_item);
        publish_bytes(state, session_service::menu_slot_changed_type, &payload,
                      sizeof(payload));
    }
    *out_value = changed ? 1 : 0;
    return REFRAIN_STATUS_OK;
}

auto menu_click(void *self,
                refrain_services_net_session_handle_session_menu menu,
                std::int32_t slot, std::int32_t button,
                refrain_services_net_session_menu_click_result *result)
    -> refrain_status {
    if (self == nullptr || result == nullptr) {
        return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT;
    }
    auto &state{*static_cast<session_state *>(self)};
    const auto index{slot_index(slot)};
    bool changed{false};
    std::uint32_t slot_count{0};
    std::uint32_t carried_count{0};
    {
        const std::lock_guard lock{state.menu_mutex};
        auto *open{find_menu(state, menu)};
        if (open == nullptr) {
            return REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_NOT_FOUND;
        }
        const auto menu_slot{
            slot >= std::numeric_limits<std::int16_t>::min() &&
                    slot <= std::numeric_limits<std::int16_t>::max()
                ? static_cast<std::int16_t>(slot)
                : static_cast<std::int16_t>(-1)};
        if (button >= std::numeric_limits<std::int8_t>::min() &&
            button <= std::numeric_limits<std::int8_t>::max()) {
            // This provider drives its own menu without the item registry
            // data, so the click uses the default stack limit.
            changed = refrain::server::apply_player_click(
                open->inventory, open->carried, menu_slot,
                static_cast<std::int8_t>(button), refrain::sim::item_table{});
        }
        if (index != refrain::server::no_inventory_index &&
            index < open->inventory.size()) {
            slot_count = open->inventory[index].count;
            state.current_slot_item = open->inventory[index].item;
        } else {
            state.current_slot_item.clear();
        }
        carried_count = open->carried.count;
        state.current_carried_item = open->carried.item;
    }
    result->struct_size = REFRAIN_SERVICES_NET_SESSION_MENU_CLICK_RESULT_SIZE;
    result->slot = slot;
    result->button = button;
    result->accepted = changed ? 1 : 0;
    result->slot_count = slot_count;
    result->slot_item = to_borrowed(state.current_slot_item);
    result->carried_count = carried_count;
    result->carried_item = to_borrowed(state.current_carried_item);
    refrain_services_net_session_menu_clicked payload{};
    payload.menu = menu;
    payload.slot = slot;
    payload.button = button;
    payload.accepted = changed ? 1 : 0;
    payload.slot_count = slot_count;
    payload.slot_item = to_borrowed(state.current_slot_item);
    payload.carried_count = carried_count;
    payload.carried_item = to_borrowed(state.current_carried_item);
    publish_bytes(state, session_service::menu_clicked_type, &payload,
                  sizeof(payload));
    return REFRAIN_STATUS_OK;
}

const refrain_services_net_session_vtable &vtable() {
    static const refrain_services_net_session_vtable table{
        .struct_size = REFRAIN_SERVICES_NET_SESSION_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_NET_SESSION_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_NET_SESSION_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_NET_SESSION_ABI_PATCH,
        .offline_profile = &offline_profile,
        .joined_profile = &joined_profile,
        .menu_open = &menu_open,
        .menu_close = &menu_close,
        .menu_set_slot = &menu_set_slot,
        .menu_click = &menu_click,
    };
    return table;
}

auto make_net_session(refrain::mod::host &host) -> refrain::status {
    static session_state state{};
    // The kernel service is sealed and registered before any mod factory runs;
    // when the embedding registers it, menu events also reach C ABI
    // subscribers.
    if (const auto kernel{
            host.services().active_provider("refrain:host.kernel")};
        kernel && kernel->instance != nullptr) {
        const auto *api{
            static_cast<const refrain::mod::provider_api *>(kernel->instance)};
        if (api->vtable != nullptr) {
            const auto *kernel_vtable{
                static_cast<const refrain_host_kernel_vtable *>(api->vtable)};
            if (kernel_vtable->struct_size == REFRAIN_HOST_KERNEL_VTABLE_SIZE) {
                state.kernel_vtable = kernel_vtable;
                state.kernel_self = api->self;
            }
        }
    }
    const refrain::mod::provider_api api{
        .vtable = &vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_NET_SESSION_ABI_MAJOR,
    };
    return host.register_provider("refrain:net.session", api, "1.0.0");
}

} // namespace

REFR_MOD_STATIC("refrain.session.vanilla", "1.0.0", 1, make_net_session);
