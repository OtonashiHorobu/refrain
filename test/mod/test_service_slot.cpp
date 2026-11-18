// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <optional>
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

    "mod/chain wrapper slots round-trip their position"_test = [] {
        expect(mod::chain_wrapper_instance(0) == "chain:0");
        expect(mod::chain_wrapper_instance(12) == "chain:12");
        expect(mod::chain_wrapper_position("chain:0") ==
               std::optional<std::size_t>{0});
        expect(mod::chain_wrapper_position("chain:12") ==
               std::optional<std::size_t>{12});
        expect(!mod::chain_wrapper_position("default").has_value());
        expect(!mod::chain_wrapper_position("chain").has_value());
        expect(!mod::chain_wrapper_position("chain:").has_value());
        expect(!mod::chain_wrapper_position("chain:x").has_value());
        expect(!mod::chain_wrapper_position("chain:1x").has_value());
        expect(!mod::chain_wrapper_position("chain:1234567890").has_value());
    };

    "mod/chain wrapper positions share one bound with the slot name"_test = [] {
        // The last valid position is the bound minus one; the bound itself
        // and anything larger cannot decode, so a published slot always has
        // a position the registry reader understands.
        const auto last{mod::max_chain_wrappers - 1U};
        expect(mod::chain_wrapper_position(mod::chain_wrapper_instance(last)) ==
               std::optional<std::size_t>{last});
        expect(!mod::chain_wrapper_position(
                    fmt::format("chain:{}", mod::max_chain_wrappers))
                    .has_value());
        expect(!mod::chain_wrapper_position("chain:9999999").has_value());
        expect(!mod::chain_wrapper_position("chain:12345678").has_value());
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
