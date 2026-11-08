// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:session` provider surface: the runtime registers a session instance
// and exposes its descriptor/lifecycle entry points through the generated ABI.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/services/session.hpp>
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
namespace session = refrain::services::session;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

suite session_provider_tests = [] {
    "session/provider exposes descriptor and lifecycle entries"_test = [] {
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
            host.services().active_provider("refrain:session", "minecraft")};
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
        const session::provider session_provider{
            *static_cast<const refrain_services_session_vtable *>(api->vtable),
            api->self};
        auto descriptor{session::make_session_descriptor()};
        session_provider.descriptor(descriptor);
        expect(refrain::contracts::as_string_view(descriptor.family) ==
               "minecraft-java");
        expect(refrain::contracts::as_string_view(descriptor.edition) ==
               "java");

        auto player{session::make_session_player()};
        player.connection = 9;
        session_provider.attach(player);
        session_provider.tick(1);
        session_provider.detach(player);
    };
};

} // namespace
