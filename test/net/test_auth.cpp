// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/auth.hpp>
#include <refrain/net/crypto.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <string>
#include <utility>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;

struct fake_transport : net::http_transport {
    net::http_request last{};
    net::http_response response{};

    auto send(const net::http_request &request)
        -> refrain::result<net::http_response> override {
        last = request;
        return response;
    }
};

[[nodiscard]] auto service(fake_transport &transport) -> net::auth_service {
    return net::auth_service{
        transport,
        net::auth_options{.session_server = "https://example.test",
                          .timeout = std::chrono::milliseconds{1000}}};
}

suite auth_tests = [] {
    "auth/has_joined parses a profile and encodes the query"_test = [] {
        fake_transport transport{};
        transport.response = {
            .status = 200,
            .body = "{\"id\":\"069a79f4-44e9-4726-a5be-fca90e38aaf5\","
                    "\"name\":\"Player\","
                    "\"properties\":[{\"name\":\"textures\","
                    "\"value\":\"abc\",\"signature\":\"sig\"}]}",
        };
        auto auth{service(transport)};
        auto profile{auth.has_joined("Player", "-abc123")};
        expect(static_cast<bool>(profile));
        if (profile) {
            const auto expected{
                net::uuid::parse("069a79f4-44e9-4726-a5be-fca90e38aaf5")};
            expect(static_cast<bool>(expected));
            if (expected) {
                expect(profile->id == *expected);
            }
            expect(profile->name == "Player");
            const std::string properties{
                reinterpret_cast<const char *>(profile->properties.data()),
                profile->properties.size()};
            expect(properties.find("\"textures\"") != std::string::npos);
        }
        expect(transport.last.url.starts_with(
            "https://example.test/session/minecraft/hasJoined?"
            "username=Player&serverId=-abc123"));
    };

    "auth/has_joined treats 204 as not joined"_test = [] {
        fake_transport transport{};
        transport.response = {.status = 204, .body = {}};
        auto auth{service(transport)};
        auto profile{auth.has_joined("Player", "abcdef")};
        expect(!profile);
        if (!profile) {
            expect(net::auth_error_of(profile.error()) ==
                   net::auth_error::not_joined);
        }
    };

    "auth/has_joined reports a server error"_test = [] {
        fake_transport transport{};
        transport.response = {.status = 503, .body = {}};
        auto auth{service(transport)};
        auto profile{auth.has_joined("Player", "abcdef")};
        expect(!profile);
        if (!profile) {
            expect(net::auth_error_of(profile.error()) ==
                   net::auth_error::remote_error);
        }
    };

    "auth/has_joined rejects a malformed profile"_test = [] {
        fake_transport transport{};
        transport.response = {.status = 200, .body = "{}"};
        auto auth{service(transport)};
        auto profile{auth.has_joined("Player", "abcdef")};
        expect(!profile);
        if (!profile) {
            expect(net::auth_error_of(profile.error()) ==
                   net::auth_error::malformed_response);
        }
    };

    "auth/has_joined rejects an invalid name or hash before sending"_test = [] {
        fake_transport transport{};
        auto auth{service(transport)};
        expect(!auth.has_joined("", "abcdef"));
        expect(!auth.has_joined("Player", "not-hex!"));
        expect(transport.last.url.empty());
    };

    "auth/offline profile is deterministic and has no properties"_test = [] {
        auto first{net::offline_profile("Player")};
        auto second{net::offline_profile("Player")};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (first && second) {
            expect(first->id == second->id);
            const std::string properties{
                reinterpret_cast<const char *>(first->properties.data()),
                first->properties.size()};
            expect(properties == "[]");
        }
    };
};

} // namespace
