// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Compile-time layout and dispatch checks for the generated
// `refrain:permissions` contract. A fake provider exercises the C ABI vtable
// through the generated C++ wrapper and through the consumer helper, so struct
// sizes, slot order, enum values and the chain composition metadata are all
// verified without a host.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/permissions/client.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

using namespace boost::ut;

namespace service = refrain::services::permissions;
namespace permissions = refrain::permissions;

static_assert(sizeof(service::permission_context_entry) == 40);
static_assert(alignof(service::permission_context_entry) == 8);
static_assert(offsetof(service::permission_context_entry, struct_size) == 0);
static_assert(offsetof(service::permission_context_entry, key) == 8);
static_assert(offsetof(service::permission_context_entry, value) == 24);

static_assert(sizeof(service::permission_context) == 56);
static_assert(alignof(service::permission_context) == 8);
static_assert(offsetof(service::permission_context, struct_size) == 0);
static_assert(offsetof(service::permission_context, world) == 8);
static_assert(offsetof(service::permission_context, gamemode) == 24);
static_assert(offsetof(service::permission_context, extra) == 40);

static_assert(sizeof(service::subject_ref) == 40);
static_assert(alignof(service::subject_ref) == 8);
static_assert(offsetof(service::subject_ref, struct_size) == 0);
static_assert(offsetof(service::subject_ref, kind) == 4);
static_assert(offsetof(service::subject_ref, id) == 8);
static_assert(offsetof(service::subject_ref, profile_name) == 24);

static_assert(sizeof(service::permission_query) == 128);
static_assert(alignof(service::permission_query) == 8);
static_assert(offsetof(service::permission_query, struct_size) == 0);
static_assert(offsetof(service::permission_query, subject) == 8);
static_assert(offsetof(service::permission_query, node) == 48);
static_assert(offsetof(service::permission_query, context) == 64);
static_assert(offsetof(service::permission_query, op_level) == 120);
static_assert(REFRAIN_SERVICES_PERMISSIONS_PERMISSION_QUERY_SIZE == 128);

// The vtable starts with the shared ABI header and then one slot per method, in
// declaration order.
static_assert(offsetof(refrain_services_permissions_vtable, query) == 16);
static_assert(offsetof(refrain_services_permissions_vtable, default_result) ==
              24);
static_assert(offsetof(refrain_services_permissions_vtable, has) == 32);
static_assert(offsetof(refrain_services_permissions_vtable, revision) == 40);
static_assert(offsetof(refrain_services_permissions_vtable, reload) == 48);
static_assert(REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE == 16 + 5 * 8);

static_assert(service::abi_major == 1);
static_assert(service::abi_minor == 0);
static_assert(service::abi_patch == 0);
static_assert(REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR == 1);
static_assert(std::string_view{service::service_id} == "refrain:permissions");
static_assert(service::side == refrain::contracts::side_kind::server);
static_assert(service::composition ==
              refrain::contracts::composition_kind::chain);
static_assert(REFRAIN_SERVICES_PERMISSIONS_COMPOSITION ==
              REFRAIN_COMPOSITION_CHAIN);

// Result and subject enum values are part of the wire contract; the error enum
// has to start at one so a failure never collides with REFRAIN_STATUS_OK.
static_assert(static_cast<std::uint32_t>(service::subject_kind::console) == 0);
static_assert(static_cast<std::uint32_t>(service::subject_kind::player) == 1);
static_assert(static_cast<std::uint32_t>(service::subject_kind::entity) == 2);
static_assert(static_cast<std::uint32_t>(service::subject_kind::plugin) == 3);
static_assert(static_cast<std::uint32_t>(service::permission_result::allowed) ==
              0);
static_assert(static_cast<std::uint32_t>(service::permission_result::denied) ==
              1);
static_assert(
    static_cast<std::uint32_t>(service::permission_result::not_handled) == 2);
static_assert(static_cast<std::uint32_t>(
                  service::permission_error::invalid_argument) == 1);
static_assert(
    static_cast<std::uint32_t>(service::permission_error::parse_error) == 2);

/// Provider state the fake slots answer from.
struct fake_permissions {
    std::uint64_t revision{7};
    std::string last_node{};
};

refrain_services_permissions_permission_result
fake_query(void *self,
           const refrain_services_permissions_permission_query *query) {
    auto &state{*static_cast<fake_permissions *>(self)};
    if (query == nullptr ||
        query->struct_size !=
            REFRAIN_SERVICES_PERMISSIONS_PERMISSION_QUERY_SIZE ||
        query->subject.struct_size !=
            REFRAIN_SERVICES_PERMISSIONS_SUBJECT_REF_SIZE ||
        query->context.struct_size !=
            REFRAIN_SERVICES_PERMISSIONS_PERMISSION_CONTEXT_SIZE) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
    }
    state.last_node = refrain::contracts::as_string_view(query->node);
    if (state.last_node == "fake.allowed") {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_ALLOWED;
    }
    if (query->subject.kind ==
        REFRAIN_SERVICES_PERMISSIONS_SUBJECT_KIND_CONSOLE) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_ALLOWED;
    }
    return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
}

refrain_services_permissions_permission_result fake_default_result(
    void *self, const refrain_services_permissions_permission_query *query) {
    return fake_query(self, query);
}

std::uint8_t
fake_has(void *self,
         const refrain_services_permissions_permission_query *query) {
    return fake_query(self, query) !=
                   REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED
               ? 1U
               : 0U;
}

std::uint64_t fake_revision(void *self) {
    return static_cast<fake_permissions *>(self)->revision;
}

refrain_status fake_reload(void *self, std::uint64_t *out_value) {
    auto &state{*static_cast<fake_permissions *>(self)};
    if (out_value == nullptr) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_INVALID_ARGUMENT;
    }
    ++state.revision;
    *out_value = state.revision;
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto make_table() -> refrain_services_permissions_vtable {
    return refrain_services_permissions_vtable{
        .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
        .query = &fake_query,
        .default_result = &fake_default_result,
        .has = &fake_has,
        .revision = &fake_revision,
        .reload = &fake_reload,
    };
}

/// Build one query the way a hand-written C++ consumer would: every struct
/// size is initialized and every string is borrowed for the call.
[[nodiscard]] auto make_query(const permissions::subject &who,
                              std::string_view node)
    -> service::permission_query {
    auto query{service::make_permission_query()};
    query.subject = service::make_subject_ref();
    query.subject.kind =
        static_cast<refrain_services_permissions_subject_kind>(who.kind);
    query.subject.id = refrain::contracts::to_borrowed(who.id);
    query.subject.profile_name =
        refrain::contracts::to_borrowed(who.profile_name);
    query.context = service::make_permission_context();
    query.node = refrain::contracts::to_borrowed(node);
    query.op_level = who.op_level;
    return query;
}

const suite permissions_contract_tests = [] {
    "permissions/contract dispatches every slot through the wrapper"_test = [] {
        fake_permissions state{};
        const auto table{make_table()};
        const service::provider provider{table, &state};

        const auto player{permissions::subject::player(
            "069a79f4-44e9-4726-a5be-fca90e38aaf5", "Notch")};
        expect(provider.query(make_query(player, "fake.allowed")) ==
               service::permission_result::allowed);
        expect(provider.query(make_query(player, "fake.denied")) ==
               service::permission_result::denied);
        expect(provider.query(
                   make_query(permissions::subject::console(), "a.b")) ==
               service::permission_result::allowed);
        expect(provider.default_result(make_query(player, "fake.denied")) ==
               service::permission_result::denied);
        expect(provider.has(make_query(player, "fake.allowed")));
        expect(!provider.has(make_query(player, "fake.denied")));
        expect(provider.revision() == 7U);
        if (const auto reloaded{provider.reload()}; reloaded) {
            expect(*reloaded == 8U);
        } else {
            expect(false) << "reload slot must succeed";
        }
        expect(provider.revision() == 8U);
        expect(state.last_node == "fake.denied");
    };

    "permissions/client builds a query the vtable accepts"_test = [] {
        fake_permissions state{};
        const auto table{make_table()};
        const permissions::client client{table, &state};

        const auto player{permissions::subject::player(
            "069a79f4-44e9-4726-a5be-fca90e38aaf5", "Notch", 4)};
        expect(client.query(player, "fake.allowed") ==
               service::permission_result::allowed);
        expect(client.query(player, "fake.denied") ==
               service::permission_result::denied);
        expect(client.query(permissions::subject::console(), "a.b") ==
               service::permission_result::allowed);
        expect(client.has(player, "fake.allowed"));
        expect(!client.has(player, "fake.denied"));
        expect(client.revision() == 7U);
        const auto reloaded{client.reload()};
        expect(static_cast<bool>(reloaded));
        if (reloaded) {
            expect(*reloaded == 8U);
        }

        const permissions::context_entry extra[] = {
            {.key = "dimension_type", .value = "overworld"}};
        expect(client.query(
                   player, "fake.allowed",
                   permissions::context{
                       .world = "minecraft:overworld",
                       .gamemode = "creative",
                       .extra = std::span<const permissions::context_entry>{
                           extra}}) == service::permission_result::allowed);
    };

    "permissions/client binds over an opaque provider pair"_test = [] {
        fake_permissions state{};
        const auto table{make_table()};
        auto client{permissions::client::bind(&table, &state)};
        expect(client.query(permissions::subject::console(), "a.b") ==
               service::permission_result::allowed);
    };
};

} // namespace
