// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/gamepack.hpp>

#include <boost/ut.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;

namespace mod = refrain::mod;

[[nodiscard]] auto declares(std::vector<std::string> provides,
                            std::string_view service) -> bool {
    return mod::declares_service(std::span<const std::string>{provides},
                                 service);
}

const suite service_slot_tests = [] {
    "mod/slot matcher accepts the slot itself"_test = [] {
        expect(mod::matches_slot("data", "data"));
        expect(mod::matches_slot("net_session", "net_session"));
        expect(!mod::matches_slot("data", "net_session"));
    };

    "mod/slot matcher accepts refrain:<slot>"_test = [] {
        expect(mod::matches_slot("refrain:data", "data"));
        expect(mod::matches_slot("refrain:worldgen", "worldgen"));
    };

    "mod/slot matcher accepts the default network namespace"_test = [] {
        expect(mod::matches_slot("refrain:net.protocol", "protocol"));
        expect(mod::matches_slot("refrain:net.session", "session"));
        expect(!mod::matches_slot("refrain:net.protocol", "session"));
    };

    "mod/slot matcher accepts a member of the slot namespace"_test = [] {
        expect(mod::matches_slot("refrain:data.packs", "data"));
        expect(mod::matches_slot("refrain:world.migration", "world"));
        expect(!mod::matches_slot("refrain:world.migration", "worldgen"));
    };

    "mod/slot matcher accepts the dotted slot spelling"_test = [] {
        expect(mod::matches_slot("refrain:net.session", "net_session"));
        expect(mod::matches_slot("refrain:world.migration", "world_migration"));
    };

    "mod/slot matcher rejects a longer dotted id"_test = [] {
        expect(!mod::matches_slot("refrain:net.session.extra", "net_session"));
        expect(!mod::matches_slot("refrain:world.migration.legacy",
                                  "world_migration"));
    };

    "mod/slot matcher rejects an unrelated service"_test = [] {
        expect(!mod::matches_slot("refrain:net.session", "net_transport"));
        expect(!mod::matches_slot("refrain:user.identity", "net_session"));
        expect(!mod::matches_slot("refrain:net.session", "world_migration"));
    };

    "mod/slot matcher rejects an empty operand"_test = [] {
        expect(!mod::matches_slot("", "net_session"));
        expect(!mod::matches_slot("refrain:net.session", ""));
        expect(!mod::matches_slot("", ""));
    };

    "mod/declares_service matches one declared contract"_test = [] {
        expect(declares({"refrain:net.session"}, "refrain:net.session"));
        expect(!declares({"refrain:net.session"}, "refrain:net.protocol"));
        expect(!declares({"refrain:net.session"}, "refrain:world.migration"));
    };

    "mod/declares_service applies the short-slot rule to a declaration"_test =
        [] {
            expect(declares({"refrain:data.packs"}, "refrain:data.packs"));
            expect(declares({"data"}, "refrain:data.packs"));
            expect(declares({"net_session"}, "refrain:net.session"));
            expect(!declares({}, "refrain:data.packs"));
        };
};

} // namespace
