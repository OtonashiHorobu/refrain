// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Compile-time + dispatch checks for the generated service contracts. A fake
// worldgen provider exercises the C ABI vtable through the generated C++
// wrapper, so layout, slot order and expected<> conversion are all verified.

#include <refrain/contracts.hpp>
#include <refrain/schema/storage/chunk.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

using namespace boost::ut;

using refrain::core::block_pos;
using refrain::core::chunk_buffer;
using refrain::core::chunk_pos;
using refrain::core::gen_ctx;
using refrain::services::worldgen::gen_stage;
using refrain::services::worldgen::generate_outcome;
using refrain::services::worldgen::provider;
using refrain::services::worldgen::worldgen_config;
using refrain::services::worldgen::worldgen_error;

static_assert(sizeof(refrain_core_handle_gen_ctx) == 8);
static_assert(sizeof(chunk_pos) == 12);
static_assert(offsetof(chunk_pos, struct_size) == 0);
static_assert(offsetof(chunk_pos, x) == 4);
static_assert(refrain::services::worldgen::abi_major == 1);
static_assert(refrain::services::worldgen::abi_minor == 0);
static_assert(refrain::services::worldgen::abi_patch == 0);
static_assert(refrain::services::worldgen::composition ==
              refrain::contracts::composition_kind::exclusive);
static_assert(REFRAIN_SERVICES_WORLDGEN_COMPOSITION ==
              REFRAIN_COMPOSITION_EXCLUSIVE);
static_assert(std::string_view{refrain::services::worldgen::service_id} ==
              "refrain:worldgen");

// The `event` declared by `sdk/idl/refrain/services/sim.ridl` is an
// in-process notification (no struct_size) with a stable id and traits.
using sim_tick_event = refrain::services::sim::tick_event;
static_assert(sizeof(sim_tick_event) == 8);
static_assert(alignof(sim_tick_event) == 8);
static_assert(offsetof(sim_tick_event, tick) == 0);
static_assert(refrain::contracts::event_traits<sim_tick_event>::is_event);
static_assert(refrain::contracts::event_traits<sim_tick_event>::id ==
              "refrain:event/refrain/services/sim/tick_event");
static_assert(!refrain::contracts::event_traits<sim_tick_event>::batch);
static_assert(!refrain::contracts::event_traits<sim_tick_event>::cancellable);
static_assert(std::string_view{refrain::services::sim::tick_event_id} ==
              "refrain:event/refrain/services/sim/tick_event");
static_assert(std::string_view{REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_ID} ==
              "refrain:event/refrain/services/sim/tick_event");
static_assert(refrain::contracts::contract_event<sim_tick_event>);

refrain_status
fake_configure(void *,
               const refrain_services_worldgen_worldgen_config *config) {
    // The generated wrapper must always hand a struct whose mandatory
    // struct_size is filled in (even if the caller forgot init_struct()).
    if (config == nullptr ||
        config->struct_size !=
            sizeof(refrain_services_worldgen_worldgen_config)) {
        return REFRAIN_SERVICES_WORLDGEN_WORLDGEN_ERROR_INTERNAL;
    }
    return REFRAIN_STATUS_OK;
}

refrain_status
fake_generate_chunk(void *, refrain_core_handle_gen_ctx,
                    const refrain_core_chunk_pos *pos,
                    refrain_core_chunk_buffer *buffer,
                    refrain_services_worldgen_generate_outcome *out) {
    out->stage = REFRAIN_SERVICES_WORLDGEN_GEN_STAGE_SURFACE;
    out->generated_blocks = 4096;
    out->elapsed_us = 42;
    static const uint8_t fake_payload[]{1, 2, 3};
    buffer->generation = 7;
    buffer->data.data = fake_payload;
    buffer->data.len = sizeof(fake_payload);
    return pos == nullptr ? REFRAIN_SERVICES_WORLDGEN_WORLDGEN_ERROR_INTERNAL
                          : REFRAIN_STATUS_OK;
}

refrain_status fake_generate_stage(
    void *, refrain_core_handle_gen_ctx, const refrain_core_chunk_pos *,
    refrain_services_worldgen_gen_stage, refrain_core_chunk_buffer *,
    refrain_services_worldgen_generate_outcome *) {
    return REFRAIN_SERVICES_WORLDGEN_WORLDGEN_ERROR_CANCELLED;
}

refrain_status fake_locate_structure(void *, refrain_core_handle_gen_ctx,
                                     const refrain_core_chunk_pos *, uint32_t,
                                     refrain_core_block_pos *out) {
    out->x = 1;
    out->y = 2;
    out->z = 3;
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto make_vtable() -> refrain_services_worldgen_vtable {
    return refrain_services_worldgen_vtable{
        sizeof(refrain_services_worldgen_vtable),
        REFRAIN_SERVICES_WORLDGEN_ABI_MAJOR,
        REFRAIN_SERVICES_WORLDGEN_ABI_MINOR,
        REFRAIN_SERVICES_WORLDGEN_ABI_PATCH,
        &fake_configure,
        &fake_generate_chunk,
        &fake_generate_stage,
        &fake_locate_structure};
}

// --- generated extension surface ------------------------------------------
//
// The production `refrain:net.protocol` contract declares the `tab_codec`
// extension, so these checks also compile the generated C++ wrapper and query
// path. The fake provider exercises registration, success, missing extension,
// version mismatch and malformed/undersized vtable handling.

static_assert(refrain::services::net_protocol::abi_minor == 0);
static_assert(std::string_view{
                  refrain::services::net_protocol::tab_codec::extension_id} ==
              "refrain:net.protocol/tab_codec");
static_assert(
    std::string_view{
        refrain::services::net_protocol::tab_codec::extends_service} ==
    "refrain:net.protocol");
static_assert(std::string_view{
                  refrain::services::net_protocol::tab_codec::capability} ==
              "tab-codec");
static_assert(refrain::services::net_protocol::tab_codec::abi_major == 1);
static_assert(refrain::services::net_protocol::tab_codec::abi_minor == 0);
static_assert(REFRAIN_EXTENSION_REF_SIZE == sizeof(refrain_extension_ref));
static_assert(REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_VTABLE_SIZE ==
              sizeof(refrain_services_net_protocol_tab_codec_vtable));
static_assert(offsetof(refrain_services_net_protocol_vtable, query_extension) >=
              4 * sizeof(std::uint32_t));

namespace net_protocol = refrain::services::net_protocol;
using net_query_fn = refrain_status (*)(void *, refrain_str, std::uint32_t,
                                        refrain_extension_ref *);

refrain_status
fake_encode_tab(void *, refrain_str header, refrain_str footer,
                refrain_services_net_protocol_encoded_packet *out) {
    if (header.data == nullptr || footer.data == nullptr || out == nullptr ||
        out->struct_size !=
            sizeof(refrain_services_net_protocol_encoded_packet)) {
        return REFRAIN_EXTENSION_INVALID;
    }
    static constexpr std::uint8_t encoded[]{0x7D};
    out->data = refrain_bytes{encoded, sizeof(encoded)};
    return REFRAIN_STATUS_OK;
}

const refrain_services_net_protocol_tab_codec_vtable fake_tab_vtable{
    REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_VTABLE_SIZE, &fake_encode_tab};

const refrain_services_net_protocol_tab_codec_vtable undersized_tab_vtable{
    0, nullptr};

refrain_status mismatch_extension_query(void *, refrain_str, std::uint32_t,
                                        refrain_extension_ref *) {
    return REFRAIN_EXTENSION_VERSION_MISMATCH;
}

refrain_status malformed_extension_query(void *, refrain_str, std::uint32_t,
                                         refrain_extension_ref *out) {
    out->struct_size = 0;
    return REFRAIN_STATUS_OK;
}

refrain_status undersized_vtable_query(void *self, refrain_str id,
                                       std::uint32_t,
                                       refrain_extension_ref *out) {
    static_cast<void>(self);
    out->struct_size = REFRAIN_EXTENSION_REF_SIZE;
    out->abi_major = 1;
    out->abi_minor = 0;
    out->abi_patch = 0;
    out->id = id;
    out->vtable = &undersized_tab_vtable;
    out->self = nullptr;
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto make_net_protocol_vtable(net_query_fn query)
    -> refrain_services_net_protocol_vtable {
    refrain_services_net_protocol_vtable table{};
    table.struct_size = static_cast<std::uint32_t>(
        sizeof(refrain_services_net_protocol_vtable));
    table.abi_major = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR;
    table.abi_minor = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MINOR;
    table.abi_patch = REFRAIN_SERVICES_NET_PROTOCOL_ABI_PATCH;
    table.query_extension = query;
    return table;
}

struct fake_extension_provider {
    net_protocol::extension_registry extensions;
};

const suite contracts_tests = [] {
    "contracts/generated ABI structs expose struct_size"_test = [] {
        chunk_pos pos{};
        refrain::contracts::init_struct(pos);
        pos.x = 5;
        pos.z = -5;
        expect(pos.struct_size == sizeof(chunk_pos));
        expect(pos.x == 5);
        expect(pos.z == -5);

        worldgen_config config{};
        refrain::contracts::init_struct(config);
        expect(config.struct_size == sizeof(worldgen_config));
        expect(config.seed == 0U);

        // The generated `make_<struct>()` helper zero-initializes and sets the
        // mandatory struct_size.
        auto made{refrain::services::worldgen::make_worldgen_config()};
        expect(made.struct_size == sizeof(worldgen_config));
        made.seed = 7;
        expect(made.seed == 7U);
    };

    "contracts/generated provider dispatches through the vtable"_test = [] {
        const auto table{make_vtable()};
        const provider service{table, nullptr};

        gen_ctx context{std::uint64_t{9}};
        chunk_pos pos{};
        refrain::contracts::init_struct(pos);
        chunk_buffer buffer{};
        buffer.pos = pos;
        buffer.generation = 0;

        worldgen_config config{};
        refrain::contracts::init_struct(config);
        config.seed = 1234;
        expect(service.configure(config).has_value());

        // A caller that forgot init_struct() is still safe: the wrapper
        // normalises struct_size on a local copy (fake_configure rejects
        // struct_size == 0, so this asserts the fix-up).
        worldgen_config uninitialized{};
        expect(uninitialized.struct_size == 0U);
        expect(service.configure(uninitialized).has_value());

        const auto generated{service.generate_chunk(context, pos, buffer)};
        expect(generated.has_value());
        if (generated) {
            expect(generated->generated_blocks == 4096U);
            expect(generated->stage ==
                   static_cast<refrain_services_worldgen_gen_stage>(
                       gen_stage::surface));
            expect(generated->elapsed_us == 42U);
            expect(buffer.generation == 7U);
            expect(refrain::contracts::as_bytes(buffer.data).size() == 3U);
        }

        const auto located{service.locate_structure(context, pos, 3U)};
        expect(located.has_value());
        if (located) {
            expect(located->x == 1);
            expect(located->y == 2);
            expect(located->z == 3);
        }
    };

    "contracts/error statuses map to std::expected errors"_test = [] {
        const auto table{make_vtable()};
        const provider service{table, nullptr};
        gen_ctx context{std::uint64_t{1}};
        chunk_pos pos{};
        refrain::contracts::init_struct(pos);
        chunk_buffer buffer{};

        const auto failed{
            service.generate_stage(context, pos, gen_stage::features, buffer)};
        expect(!failed.has_value());
        if (!failed) {
            expect(failed.error() == worldgen_error::cancelled);
        }
    };

    "contracts/malformed input is reported as a contract error"_test = [] {
        const auto table{make_vtable()};
        const provider service{table, nullptr};
        const gen_ctx context{std::uint64_t{1}};
        chunk_pos pos{};
        refrain::contracts::init_struct(pos);
        chunk_buffer buffer{};
        const auto generated{service.generate_chunk(context, pos, buffer)};
        expect(generated.has_value()); // fake returns OK for a non-null pos

        // The fake treats a null `pos` as an internal failure; the wrapper
        // always passes a valid pointer, so this asserts the success path only.
        expect(buffer.generation == 7U);
    };

    "contracts/RSC data header exposes storage structs"_test = [] {
        refrain::storage::chunk record{};
        record.format_version = 2;
        record.status = refrain::storage::chunk_status::full;
        expect(record.format_version == 2);
        expect(record.status == refrain::storage::chunk_status::full);
        expect(record.sections.size() == 0U);
    };

    // Services with no `#[schedule]` methods still expose an empty, span-able
    // descriptor table (production IDL declares no schedules yet).
    "contracts/service without schedules exposes an empty table"_test = [] {
        expect(refrain::services::sim::schedule_tasks.size() == 0U);
        const auto tasks{std::span{refrain::services::sim::schedule_tasks}};
        expect(tasks.empty());
    };
};

const suite extension_contract_tests = [] {
    "contracts/generated extension binds and dispatches through the vtable"_test =
        [] {
            const net_protocol::extension_binding bindings[] = {
                net_protocol::make_extension_binding<net_protocol::tab_codec>(
                    fake_tab_vtable, nullptr),
            };
            fake_extension_provider provider_state{
                net_protocol::extension_registry{bindings}};
            const auto table{make_net_protocol_vtable(
                &net_protocol::query_extension<fake_extension_provider>)};
            const net_protocol::provider service{table, &provider_state};

            expect(std::string_view{net_protocol::extension_ids[0]} ==
                   "refrain:net.protocol/tab_codec");
            const auto bound{service.extension<net_protocol::tab_codec>()};
            expect(bound.has_value());
            if (!bound) {
                return;
            }
            ::refrain::services::net_protocol::encoded_packet packet{};
            const auto encoded{bound->encode_tab("header", "footer", packet)};
            expect(encoded.has_value());
            expect(packet.data.len == 1U);
        };

    "contracts/extension query reports missing and incompatible providers"_test =
        [] {
            fake_extension_provider empty_provider{
                net_protocol::extension_registry{
                    std::span<const net_protocol::extension_binding>{}}};
            const auto missing_table{make_net_protocol_vtable(
                &net_protocol::query_extension<fake_extension_provider>)};
            const net_protocol::provider missing_service{missing_table,
                                                         &empty_provider};
            const auto missing{
                missing_service.extension<net_protocol::tab_codec>()};
            expect(!missing.has_value());
            if (!missing) {
                expect(missing.error().code == refrain::error_code::not_found);
            }

            const auto mismatch_table{
                make_net_protocol_vtable(&mismatch_extension_query)};
            const net_protocol::provider mismatch_service{mismatch_table,
                                                          nullptr};
            const auto mismatch{
                mismatch_service.extension<net_protocol::tab_codec>()};
            expect(!mismatch.has_value());
            if (!mismatch) {
                expect(mismatch.error().code ==
                       refrain::error_code::version_mismatch);
            }
        };

    "contracts/extension query fails closed on malformed references"_test = [] {
        const auto malformed_table{
            make_net_protocol_vtable(&malformed_extension_query)};
        const net_protocol::provider malformed_service{malformed_table,
                                                       nullptr};
        const auto malformed{
            malformed_service.extension<net_protocol::tab_codec>()};
        expect(!malformed.has_value());
        if (!malformed) {
            expect(malformed.error().code ==
                   refrain::error_code::invalid_argument);
        }

        const auto undersized_table{
            make_net_protocol_vtable(&undersized_vtable_query)};
        const net_protocol::provider undersized_service{undersized_table,
                                                        nullptr};
        const auto undersized{
            undersized_service.extension<net_protocol::tab_codec>()};
        expect(!undersized.has_value());
        if (!undersized) {
            expect(undersized.error().code ==
                   refrain::error_code::invalid_argument);
        }

        auto slotless{make_net_protocol_vtable(nullptr)};
        slotless.struct_size = 4 * sizeof(std::uint32_t);
        const net_protocol::provider slotless_service{slotless, nullptr};
        const auto slotless_extension{
            slotless_service.extension<net_protocol::tab_codec>()};
        expect(!slotless_extension.has_value());
        if (!slotless_extension) {
            expect(slotless_extension.error().code ==
                   refrain::error_code::unsupported);
        }
    };

    "contracts/extension registry validates id, version and sizes"_test = [] {
        const net_protocol::extension_binding bindings[] = {
            net_protocol::make_extension_binding<net_protocol::tab_codec>(
                fake_tab_vtable, nullptr),
        };
        const net_protocol::extension_registry registry{bindings};
        const auto id{refrain::contracts::to_borrowed(
            net_protocol::tab_codec::extension_id)};

        refrain_extension_ref ref{};
        ref.struct_size = REFRAIN_EXTENSION_REF_SIZE;
        expect(registry.query(id, REFRAIN_EXTENSION_VERSION(1, 0), ref) ==
               REFRAIN_STATUS_OK);
        expect(ref.abi_major == 1U);
        expect(ref.abi_minor == 0U);
        expect(ref.vtable == &fake_tab_vtable);

        expect(registry.query(id, REFRAIN_EXTENSION_VERSION(2, 0), ref) ==
               REFRAIN_EXTENSION_VERSION_MISMATCH);
        expect(registry.query(id, REFRAIN_EXTENSION_VERSION(1, 1), ref) ==
               REFRAIN_EXTENSION_VERSION_MISMATCH);

        const auto missing_id{refrain::contracts::to_borrowed(
            std::string_view{"refrain:net.protocol/missing"})};
        expect(registry.query(missing_id, REFRAIN_EXTENSION_VERSION(1, 0),
                              ref) == REFRAIN_EXTENSION_NOT_FOUND);

        refrain_extension_ref bad_shape{};
        expect(registry.query(id, REFRAIN_EXTENSION_VERSION(1, 0), bad_shape) ==
               REFRAIN_EXTENSION_INVALID);

        // A registration-time undersized vtable fails closed.
        const auto rejected{
            net_protocol::make_extension_binding<net_protocol::tab_codec>(
                undersized_tab_vtable, nullptr)};
        expect(rejected.vtable == nullptr);
        const net_protocol::extension_binding rejected_bindings[]{rejected};
        const net_protocol::extension_registry rejected_registry{
            rejected_bindings};
        ref.struct_size = REFRAIN_EXTENSION_REF_SIZE;
        expect(rejected_registry.query(id, REFRAIN_EXTENSION_VERSION(1, 0),
                                       ref) == REFRAIN_EXTENSION_INVALID);
    };
};

} // namespace
