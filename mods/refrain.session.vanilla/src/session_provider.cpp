// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:net.session` provider: deterministic offline profiles and the
// online `hasJoined` lookup. The network server consumes this service as an
// optional authentication seam.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/net_session_abi.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/net/auth.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace {

using refrain::contracts::as_string_view;
using refrain::contracts::to_borrowed;

struct session_state {
    std::mutex mutex{};
    std::unique_ptr<refrain::net::http_transport> transport{};
    std::unique_ptr<refrain::net::auth_service> auth{};
    std::string current_id{};
    std::string current_name{};
    std::vector<std::byte> current_properties{};
};

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

const refrain_services_net_session_vtable &vtable() {
    static const refrain_services_net_session_vtable table{
        .struct_size = REFRAIN_SERVICES_NET_SESSION_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_NET_SESSION_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_NET_SESSION_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_NET_SESSION_ABI_PATCH,
        .offline_profile = &offline_profile,
        .joined_profile = &joined_profile,
    };
    return table;
}

auto make_net_session(refrain::mod::host &host) -> refrain::status {
    static session_state state{};
    const refrain::mod::provider_api api{
        .vtable = &vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_NET_SESSION_ABI_MAJOR,
    };
    return host.register_provider("refrain:net.session", api, "1.0.0");
}

} // namespace

REFR_MOD_STATIC("refrain.session.vanilla", "1.0.0", 1, make_net_session);
