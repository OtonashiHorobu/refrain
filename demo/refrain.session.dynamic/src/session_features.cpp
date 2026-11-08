// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Example session feature provider: dynamic server-list status and tab list.
// It demonstrates that presentation features are supplied through session
// feature services, not encoded by the network layer.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/session_status_abi.h>
#include <refrain/contracts/services/session_tab_abi.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>

#include <cstdint>
#include <string_view>

namespace {

struct feature_state {
    std::string_view motd{"Refrain dynamic"};
    std::string_view tab_header{"Refrain"};
    std::string_view tab_footer{"Dynamic tab"};
};

auto status(void *self,
            const refrain_services_session_status_status_request *request,
            refrain_services_session_status_status_view *view) -> std::uint8_t {
    if (self == nullptr || request == nullptr || view == nullptr) {
        return 0;
    }
    const auto &state{*static_cast<feature_state *>(self)};
    view->struct_size = REFRAIN_SERVICES_SESSION_STATUS_STATUS_VIEW_SIZE;
    view->description = refrain::contracts::to_borrowed(state.motd);
    view->online_players = request->online_players;
    view->max_players = 20;
    return 1;
}

auto tab(void *self, const refrain_services_session_tab_tab_request *request,
         refrain_services_session_tab_tab_view *view) -> std::uint8_t {
    if (self == nullptr || request == nullptr || view == nullptr) {
        return 0;
    }
    const auto &state{*static_cast<feature_state *>(self)};
    view->struct_size = REFRAIN_SERVICES_SESSION_TAB_TAB_VIEW_SIZE;
    view->header = refrain::contracts::to_borrowed(state.tab_header);
    view->footer = refrain::contracts::to_borrowed(state.tab_footer);
    return 1;
}

auto make_session_features(refrain::mod::host &host) -> refrain::status {
    static feature_state state{};
    static const refrain_services_session_status_vtable status_vtable{
        .struct_size = REFRAIN_SERVICES_SESSION_STATUS_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_SESSION_STATUS_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_SESSION_STATUS_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_SESSION_STATUS_ABI_PATCH,
        .status = &status,
    };
    static const refrain_services_session_tab_vtable tab_vtable{
        .struct_size = REFRAIN_SERVICES_SESSION_TAB_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_SESSION_TAB_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_SESSION_TAB_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_SESSION_TAB_ABI_PATCH,
        .tab = &tab,
    };
    const refrain::mod::provider_api status_api{
        .vtable = &status_vtable,
        .self = &state,
        .abi_major = REFRAIN_SERVICES_SESSION_STATUS_ABI_MAJOR,
    };
    const refrain::mod::provider_api tab_api{
        .vtable = &tab_vtable,
        .self = &state,
        .abi_major = REFRAIN_SERVICES_SESSION_TAB_ABI_MAJOR,
    };
    if (auto registered{host.register_provider("refrain:session.status",
                                               "dynamic", status_api, "1.0.0")};
        !registered) {
        return registered;
    }
    return host.register_provider("refrain:session.tab", "dynamic", tab_api,
                                  "1.0.0");
}

} // namespace

REFR_MOD_STATIC("refrain.session.dynamic", "1.0.0", 1, make_session_features);
