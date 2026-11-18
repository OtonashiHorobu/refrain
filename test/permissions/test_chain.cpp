// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Provider chain: the tri-state walk over wrappers and the base resolver,
// revision composition and reload propagation.

#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/error.hpp>
#include <refrain/permissions/chain.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace permissions = refrain::permissions;
namespace contract = refrain::services::permissions;

using permissions::permission_result;

/// One scripted provider. Every call is recorded under its id so a test can
/// assert the walk order and that a link after a deciding answer was not
/// called.
struct fake_provider {
    std::string id{};
    permission_result answer{permission_result::not_handled};
    permission_result default_answer{permission_result::not_handled};
    std::uint64_t revision{1};
    bool reload_succeeds{true};
    std::uint64_t reload_increment{1};
    std::vector<std::string> *calls{nullptr};
    std::vector<std::string> *reload_calls{nullptr};

    [[nodiscard]] auto record(std::string_view method) const {
        if (calls != nullptr) {
            calls->push_back(id + "." + std::string{method});
        }
    }

    static auto
    abi_query(void *self,
              const refrain_services_permissions_permission_query *query)
        -> refrain_services_permissions_permission_result {
        if (self == nullptr || query == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
        }
        auto *fake{static_cast<fake_provider *>(self)};
        fake->record("query");
        return static_cast<refrain_services_permissions_permission_result>(
            fake->answer);
    }

    static auto abi_default_result(
        void *self, const refrain_services_permissions_permission_query *query)
        -> refrain_services_permissions_permission_result {
        if (self == nullptr || query == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
        }
        auto *fake{static_cast<fake_provider *>(self)};
        fake->record("default_result");
        return static_cast<refrain_services_permissions_permission_result>(
            fake->default_answer);
    }

    static auto
    abi_has(void *self,
            const refrain_services_permissions_permission_query *query)
        -> std::uint8_t {
        return abi_query(self, query) !=
                       REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED
                   ? 1U
                   : 0U;
    }

    static auto abi_revision(void *self) -> std::uint64_t {
        const auto *fake{static_cast<const fake_provider *>(self)};
        return fake->revision;
    }

    static auto abi_reload(void *self, std::uint64_t *out_value)
        -> refrain_status {
        auto *fake{static_cast<fake_provider *>(self)};
        if (fake->reload_calls != nullptr) {
            fake->reload_calls->push_back(fake->id + ".reload");
        }
        if (!fake->reload_succeeds) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_PARSE_ERROR;
        }
        fake->revision += fake->reload_increment;
        if (out_value != nullptr) {
            *out_value = fake->revision;
        }
        return REFRAIN_STATUS_OK;
    }

    [[nodiscard]] static auto table()
        -> const refrain_services_permissions_vtable & {
        static const refrain_services_permissions_vtable value{
            .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
            .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
            .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
            .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
            .query = &fake_provider::abi_query,
            .default_result = &fake_provider::abi_default_result,
            .has = &fake_provider::abi_has,
            .revision = &fake_provider::abi_revision,
            .reload = &fake_provider::abi_reload,
        };
        return value;
    }

    [[nodiscard]] auto link() const -> permissions::provider_link {
        return permissions::make_provider_link(
            id, table(), const_cast<fake_provider *>(this));
    }
};

[[nodiscard]] auto make_query(std::string_view node)
    -> contract::permission_query {
    auto query{contract::make_permission_query()};
    query.subject = contract::make_subject_ref();
    query.subject.kind = static_cast<refrain_services_permissions_subject_kind>(
        contract::subject_kind::player);
    static constexpr std::string_view uuid{
        "069a79f4-44e9-4726-a5be-fca90e38aaf5"};
    query.subject.id = refrain::contracts::to_borrowed(uuid);
    query.context = contract::make_permission_context();
    query.node = refrain::contracts::to_borrowed(node);
    return query;
}

const suite chain_tests = [] {
    "chain/the first wrapper that answers decides and later links are not called"_test =
        [] {
            std::vector<std::string> calls{};
            fake_provider denied{.id = "deny",
                                 .answer = permission_result::denied,
                                 .calls = &calls};
            fake_provider allowed{.id = "allow",
                                  .answer = permission_result::allowed,
                                  .calls = &calls};
            fake_provider base{.id = "base",
                               .answer = permission_result::denied,
                               .calls = &calls};
            auto chain{permissions::provider_chain::create(
                {denied.link(), allowed.link()}, base.link())};
            expect(static_cast<bool>(chain));
            if (!chain) {
                return;
            }
            const auto query{make_query("refrain.command.teleport")};
            expect(chain->query(query) == permission_result::denied);
            expect(calls == std::vector<std::string>{"deny.query"});
        };

    "chain/a not_handled wrapper forwards to the next provider"_test = [] {
        std::vector<std::string> calls{};
        fake_provider forwarding{.id = "forward", .calls = &calls};
        fake_provider handling{.id = "handling",
                               .answer = permission_result::allowed,
                               .calls = &calls};
        fake_provider base{
            .id = "base", .answer = permission_result::denied, .calls = &calls};
        auto chain{permissions::provider_chain::create(
            {forwarding.link(), handling.link()}, base.link())};
        expect(static_cast<bool>(chain));
        if (!chain) {
            return;
        }
        const auto query{make_query("com.example.mymod.home")};
        expect(chain->query(query) == permission_result::allowed);
        expect(calls ==
               std::vector<std::string>{"forward.query", "handling.query"});
    };

    "chain/every wrapper forwarding falls back to the base resolver"_test = [] {
        std::vector<std::string> calls{};
        fake_provider first{.id = "first", .calls = &calls};
        fake_provider second{.id = "second", .calls = &calls};
        fake_provider base{
            .id = "base", .answer = permission_result::denied, .calls = &calls};
        auto chain{permissions::provider_chain::create(
            {first.link(), second.link()}, base.link())};
        expect(static_cast<bool>(chain));
        if (!chain) {
            return;
        }
        const auto query{make_query("refrain.command.stop")};
        expect(chain->query(query) == permission_result::denied);
        expect(calls == std::vector<std::string>{"first.query", "second.query",
                                                 "base.query"});
    };

    "chain/default_result answers from the base alone"_test = [] {
        std::vector<std::string> calls{};
        fake_provider wrapper{.id = "wrapper",
                              .answer = permission_result::allowed,
                              .calls = &calls};
        fake_provider base{.id = "base",
                           .answer = permission_result::allowed,
                           .default_answer = permission_result::denied,
                           .calls = &calls};
        auto chain{
            permissions::provider_chain::create({wrapper.link()}, base.link())};
        expect(static_cast<bool>(chain));
        if (!chain) {
            return;
        }
        const auto query{make_query("refrain.command.teleport")};
        expect(chain->default_result(query) == permission_result::denied);
        expect(calls == std::vector<std::string>{"base.default_result"});
    };

    "chain/has is true unless the composed answer is denied"_test = [] {
        fake_provider undecided{.id = "undecided"};
        fake_provider base{.id = "base", .answer = permission_result::allowed};
        auto chain{permissions::provider_chain::create({undecided.link()},
                                                       base.link())};
        expect(static_cast<bool>(chain));
        if (!chain) {
            return;
        }
        const auto query{make_query("refrain.command.teleport")};
        expect(chain->has(query));
        base.answer = permission_result::denied;
        expect(!chain->has(query));
    };

    "chain/the composed revision is the base revision plus the wrapper revisions"_test =
        [] {
            fake_provider wrapper_a{.id = "a", .revision = 2};
            fake_provider wrapper_b{.id = "b", .revision = 3};
            fake_provider base{.id = "base", .revision = 5};
            auto chain{permissions::provider_chain::create(
                {wrapper_a.link(), wrapper_b.link()}, base.link())};
            expect(static_cast<bool>(chain));
            if (!chain) {
                return;
            }
            expect(chain->revision() == 10U);
            wrapper_a.revision = 4;
            expect(chain->revision() == 12U);
            base.revision = 6;
            expect(chain->revision() == 13U);
        };

    "chain/reload walks the base first and then the wrappers"_test = [] {
        std::vector<std::string> calls{};
        fake_provider outer{
            .id = "outer", .revision = 1, .reload_calls = &calls};
        fake_provider inner{
            .id = "inner", .revision = 1, .reload_calls = &calls};
        fake_provider base{.id = "base", .revision = 1, .reload_calls = &calls};
        auto chain{permissions::provider_chain::create(
            {outer.link(), inner.link()}, base.link())};
        expect(static_cast<bool>(chain));
        if (!chain) {
            return;
        }
        const auto outcome{chain->reload()};
        expect(static_cast<bool>(outcome));
        if (!outcome) {
            return;
        }
        expect(outcome->changed);
        expect(outcome->previous_revision == 3U);
        expect(outcome->revision == 6U);
        expect(calls == std::vector<std::string>{"base.reload", "outer.reload",
                                                 "inner.reload"});
    };

    "chain/a reloaded wrapper that did not change keeps the revision"_test =
        [] {
            fake_provider wrapper{.id = "wrapper", .reload_increment = 0};
            fake_provider base{.id = "base", .reload_increment = 0};
            auto chain{permissions::provider_chain::create({wrapper.link()},
                                                           base.link())};
            expect(static_cast<bool>(chain));
            if (!chain) {
                return;
            }
            const auto outcome{chain->reload()};
            expect(static_cast<bool>(outcome));
            if (outcome) {
                expect(!outcome->changed);
                expect(outcome->revision == 2U);
            }
        };

    "chain/a failing wrapper reload stops the walk and names the wrapper"_test =
        [] {
            std::vector<std::string> calls{};
            fake_provider outer{.id = "outer", .reload_calls = &calls};
            fake_provider failing{.id = "broken",
                                  .reload_succeeds = false,
                                  .reload_calls = &calls};
            fake_provider inner{.id = "inner", .reload_calls = &calls};
            fake_provider base{.id = "base", .reload_calls = &calls};
            auto chain{permissions::provider_chain::create(
                {outer.link(), failing.link(), inner.link()}, base.link())};
            expect(static_cast<bool>(chain));
            if (!chain) {
                return;
            }
            const auto outcome{chain->reload()};
            expect(!outcome);
            if (!outcome) {
                expect(outcome.error().code ==
                       refrain::error_code::parse_error);
                expect(outcome.error().message.find("broken") !=
                       std::string::npos);
            }
            expect(calls == std::vector<std::string>{"base.reload",
                                                     "outer.reload",
                                                     "broken.reload"});
        };

    "chain/a malformed query is denied before any link is called"_test = [] {
        std::vector<std::string> calls{};
        fake_provider wrapper{.id = "wrapper",
                              .answer = permission_result::allowed,
                              .calls = &calls};
        fake_provider base{.id = "base",
                           .answer = permission_result::allowed,
                           .calls = &calls};
        auto chain{
            permissions::provider_chain::create({wrapper.link()}, base.link())};
        expect(static_cast<bool>(chain));
        if (!chain) {
            return;
        }
        auto query{make_query("refrain.command.teleport")};
        query.struct_size = 0;
        expect(chain->query(query) == permission_result::denied);
        expect(chain->has(query) == false);
        expect(calls.empty());
    };

    "chain/create rejects an incomplete link"_test = [] {
        fake_provider base{.id = "base"};
        permissions::provider_link broken{.id = "broken", .self = &base};
        auto chain{permissions::provider_chain::create({}, broken)};
        expect(!chain);
        if (!chain) {
            expect(chain.error().code == refrain::error_code::invalid_argument);
        }
        auto missing_id{permissions::provider_chain::create(
            {}, permissions::make_provider_link("", fake_provider::table(),
                                                &base))};
        expect(!missing_id);
    };

    "chain/the provider surface answers through the chain"_test = [] {
        std::vector<std::string> calls{};
        fake_provider wrapper{.id = "wrapper",
                              .answer = permission_result::allowed,
                              .calls = &calls};
        fake_provider base{
            .id = "base", .answer = permission_result::denied, .calls = &calls};
        auto chain{
            permissions::provider_chain::create({wrapper.link()}, base.link())};
        expect(static_cast<bool>(chain));
        if (!chain) {
            return;
        }
        const auto query{make_query("com.example.mymod.home")};
        const auto &table{chain->vtable()};
        expect(table.query(chain->self(), &query) ==
               REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_ALLOWED);
        auto revision{std::uint64_t{0}};
        expect(table.revision(chain->self()) == 2U);
        expect(table.reload(chain->self(), &revision) == REFRAIN_STATUS_OK);
        expect(revision == 4U);
        expect(table.has(chain->self(), &query) != 0);
    };
};

} // namespace
