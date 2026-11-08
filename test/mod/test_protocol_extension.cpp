// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The production protocol provider exposes the `tab_codec` extension through
// the generated query slot. This test binds it through the same host/service
// path runtime uses and checks the encoded tab packet body.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/services/net_protocol.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/net/base.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace mod = refrain::mod;
namespace net_protocol = refrain::services::net_protocol;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

suite protocol_extension_tests = [] {
    "protocol/provider exposes a working tab_codec extension"_test = [] {
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
        const auto provider_info{host.services().active_provider(
            "refrain:net.protocol", "java_777")};
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
        const net_protocol::provider provider{
            *static_cast<const refrain_services_net_protocol_vtable *>(
                api->vtable),
            api->self};
        auto codec{provider.extension<net_protocol::tab_codec>()};
        expect(codec.has_value());
        if (!codec) {
            return;
        }
        provider.create_connection(7);
        provider.notify_state(7, net_protocol::connection_state::play);
        expect(provider.state_of(refrain::core::connection{7}) ==
               net_protocol::connection_state::play);
        provider.close_connection(7);
        expect(provider.state_of(refrain::core::connection{7}) ==
               net_protocol::connection_state::handshake);

        auto packet{net_protocol::make_encoded_packet()};
        const auto encoded{codec->encode_tab("Header", "Footer", packet)};
        expect(encoded.has_value());
        if (!encoded) {
            return;
        }
        const auto bytes{refrain::contracts::as_bytes(packet.data)};
        refrain::net::reader reader{std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()}};
        const auto packet_id{reader.read_varint()};
        expect(packet_id.has_value());
        if (packet_id) {
            expect(*packet_id == 125U); // play/clientbound tab_list
        }
    };
};

} // namespace
