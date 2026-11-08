// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:net.session` provider: offline profiles are deterministic and
// available without any network access.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/services/net_session.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace mod = refrain::mod;
namespace net_session = refrain::services::net_session;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

suite net_session_provider_tests = [] {
    "session/provider resolves an offline profile"_test = [] {
        auto spec{mod::load_gamepack_spec(std::string{REFRAIN_SOURCE_DIR} +
                                              "/gamepack/gamepack.toml",
                                          "server")};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        auto plan{mod::preview_composition(*spec)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{};
        auto loaded{host.load(*plan, builtin_bytes())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        const auto provider_info{
            host.services().active_provider("refrain:net.session")};
        expect(provider_info.has_value());
        if (!provider_info || provider_info->instance == nullptr) {
            return;
        }
        const auto *api{
            static_cast<const mod::provider_api *>(provider_info->instance)};
        expect(api->vtable != nullptr);
        if (api->vtable == nullptr) {
            return;
        }
        const net_session::provider session{
            *static_cast<const refrain_services_net_session_vtable *>(
                api->vtable),
            api->self};
        auto profile{net_session::make_session_profile()};
        const auto offline{session.offline_profile("Alice", profile)};
        expect(offline.has_value());
        if (!offline) {
            return;
        }
        expect(refrain::contracts::as_string_view(profile.id) ==
               "10920508-d5d8-3eed-93d2-92f193afe7d7");
        expect(refrain::contracts::as_string_view(profile.name) == "Alice");
    };
};

} // namespace
