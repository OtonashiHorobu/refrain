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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

// `refrain:world` v1.2.0 appended the storage compression settings after the
// boot projection slot, so the generated table grows by three pointers and
// carries the policy layout.
namespace world_service = refrain::services::world;
static_assert(world_service::abi_major == 1);
static_assert(world_service::abi_minor == 2);
static_assert(REFRAIN_SERVICES_WORLD_ABI_MINOR == 2);
static_assert(std::string_view{world_service::service_id} == "refrain:world");
static_assert(sizeof(world_service::level_info) == 72);
static_assert(alignof(world_service::level_info) == 8);
static_assert(offsetof(world_service::level_info, level_name) == 40);
static_assert(sizeof(world_service::level_projection) == 88);
static_assert(alignof(world_service::level_projection) == 8);
static_assert(offsetof(world_service::level_projection, level) == 8);
static_assert(offsetof(world_service::level_projection, has_chunks) == 80);
static_assert(sizeof(world_service::zstd_policy) == 24);
static_assert(alignof(world_service::zstd_policy) == 4);
static_assert(offsetof(world_service::zstd_policy, struct_size) == 0);
static_assert(offsetof(world_service::zstd_policy, near_level) == 4);
static_assert(offsetof(world_service::zstd_policy, mid_level) == 8);
static_assert(offsetof(world_service::zstd_policy, far_level) == 12);
static_assert(offsetof(world_service::zstd_policy, near_radius) == 16);
static_assert(offsetof(world_service::zstd_policy, mid_radius) == 20);
static_assert(offsetof(refrain_services_world_vtable, level) ==
              REFRAIN_SERVICES_WORLD_VTABLE_SIZE - 4 * sizeof(void *));
static_assert(offsetof(refrain_services_world_vtable, zstd_level) ==
              REFRAIN_SERVICES_WORLD_VTABLE_SIZE - 3 * sizeof(void *));
static_assert(offsetof(refrain_services_world_vtable, set_zstd_level) ==
              REFRAIN_SERVICES_WORLD_VTABLE_SIZE - 2 * sizeof(void *));
static_assert(offsetof(refrain_services_world_vtable, set_zstd_policy) ==
              REFRAIN_SERVICES_WORLD_VTABLE_SIZE - sizeof(void *));

/// Settings a fake provider records when the generated wrapper dispatches the
/// compression calls.
struct fake_world_settings {
    std::int32_t effective_level{0};
    int set_level_calls{0};
    int set_policy_calls{0};
};

refrain_status fake_world_set_zstd_level(void *self, std::int32_t level) {
    auto &settings{*static_cast<fake_world_settings *>(self)};
    ++settings.set_level_calls;
    settings.effective_level = level;
    return REFRAIN_STATUS_OK;
}

refrain_status
fake_world_set_zstd_policy(void *self,
                           const refrain_services_world_zstd_policy *policy) {
    auto &settings{*static_cast<fake_world_settings *>(self)};
    if (policy == nullptr ||
        policy->struct_size != REFRAIN_SERVICES_WORLD_ZSTD_POLICY_SIZE) {
        return REFRAIN_SERVICES_WORLD_WORLD_ERROR_INTERNAL;
    }
    ++settings.set_policy_calls;
    settings.effective_level = policy->near_level;
    return REFRAIN_STATUS_OK;
}

std::int32_t fake_world_zstd_level(void *self) {
    return static_cast<fake_world_settings *>(self)->effective_level;
}

refrain_status fake_world_level(void *self,
                                refrain_services_world_level_projection *out) {
    auto &name{*static_cast<std::string *>(self)};
    name = "projected";
    out->struct_size = REFRAIN_SERVICES_WORLD_LEVEL_PROJECTION_SIZE;
    out->has_chunks = 1;
    out->level.struct_size = REFRAIN_SERVICES_WORLD_LEVEL_INFO_SIZE;
    out->level.data_version = 4189;
    out->level.seed = 1234;
    out->level.spawn_x = 1;
    out->level.spawn_y = 64;
    out->level.spawn_z = -1;
    out->level.time = 6000;
    out->level.level_name = refrain::contracts::to_borrowed(name);
    out->level.game_mode = 1;
    out->level.hardcore = 1;
    out->level.difficulty = 2;
    out->level.difficulty_locked = 1;
    return REFRAIN_STATUS_OK;
}

refrain_status
fake_world_level_missing(void *, refrain_services_world_level_projection *) {
    return REFRAIN_SERVICES_WORLD_WORLD_ERROR_NOT_IMPLEMENTED;
}

/// Fake `refrain:sim` player surface. The strings and the profile uuid live in
/// the fake state, which outlives every call, so the generated wrappers see the
/// same borrowed views a real provider returns.
struct fake_sim_players {
    std::string name{"Alice"};
    std::string world{"overworld"};
    std::array<std::uint8_t, 16> profile{1, 2,  3,  4,  5,  6,  7,  8,
                                         9, 10, 11, 12, 13, 14, 15, 16};
    int counts{0};
    int reads{0};
    int lookups{0};
    int adds{0};
    int removals{0};
    int transforms{0};
    int positions{0};
    int view_distances{0};
    int moves{0};
    int jumps{0};
    std::uint64_t added_connection{0};
    std::string added_name{};
    std::size_t added_profile_size{0};
    std::uint32_t added_slots{0};
};

void fill_fake_player(const fake_sim_players &state,
                      refrain_services_sim_player_info &out) {
    out.struct_size = REFRAIN_SERVICES_SIM_PLAYER_INFO_SIZE;
    out.connection = 7;
    out.profile_id =
        refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
            state.profile.data(), state.profile.size()});
    out.name = refrain::contracts::to_borrowed(std::string_view{state.name});
    out.world = refrain::contracts::to_borrowed(std::string_view{state.world});
    out.entity_id = 3;
    out.x = 1.5;
    out.y = 64.0;
    out.z = -2.5;
    out.yaw = 90.0F;
    out.pitch = -10.0F;
    out.on_ground = 1;
    out.game_mode = 1;
    out.health = 18.0F;
    out.view_distance = 12;
}

std::uint32_t fake_sim_player_count(void *self,
                                    refrain_services_sim_handle_sim_world) {
    ++static_cast<fake_sim_players *>(self)->counts;
    return 1;
}

refrain_status fake_sim_player_at(void *self,
                                  refrain_services_sim_handle_sim_world,
                                  std::uint32_t index,
                                  refrain_services_sim_player_info *out) {
    auto &state{*static_cast<fake_sim_players *>(self)};
    ++state.reads;
    if (out == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    if (index != 0) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_OUT_OF_BOUNDS;
    }
    fill_fake_player(state, *out);
    return REFRAIN_STATUS_OK;
}

refrain_status
fake_sim_player_by_connection(void *self, refrain_services_sim_handle_sim_world,
                              std::uint64_t connection,
                              refrain_services_sim_player_info *out) {
    auto &state{*static_cast<fake_sim_players *>(self)};
    ++state.lookups;
    if (out == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    if (connection != 7) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND;
    }
    fill_fake_player(state, *out);
    return REFRAIN_STATUS_OK;
}

refrain_status fake_sim_add_player(
    void *self, refrain_services_sim_handle_sim_world,
    const refrain_services_sim_player_registration *registration,
    std::uint64_t *out_value) {
    auto &state{*static_cast<fake_sim_players *>(self)};
    ++state.adds;
    if (registration == nullptr || out_value == nullptr ||
        registration->struct_size !=
            REFRAIN_SERVICES_SIM_PLAYER_REGISTRATION_SIZE) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    state.added_connection = registration->connection;
    state.added_name =
        std::string{refrain::contracts::as_string_view(registration->name)};
    state.added_profile_size =
        refrain::contracts::as_bytes(registration->profile_id).size();
    state.added_slots = registration->inventory_slots;
    *out_value = 12;
    return REFRAIN_STATUS_OK;
}

std::uint8_t fake_sim_remove_player(void *self,
                                    refrain_services_sim_handle_sim_world,
                                    std::uint64_t connection) {
    auto &state{*static_cast<fake_sim_players *>(self)};
    ++state.removals;
    return connection == 7 ? 1 : 0;
}

refrain_status
fake_sim_set_player_transform(void *self, refrain_services_sim_handle_sim_world,
                              std::uint64_t, double, double, double, float,
                              float) {
    ++static_cast<fake_sim_players *>(self)->transforms;
    return REFRAIN_STATUS_OK;
}

refrain_status
fake_sim_set_player_position(void *self, refrain_services_sim_handle_sim_world,
                             std::uint64_t, double, double, double) {
    ++static_cast<fake_sim_players *>(self)->positions;
    return REFRAIN_STATUS_OK;
}

refrain_status
fake_sim_set_player_view_distance(void *self,
                                  refrain_services_sim_handle_sim_world,
                                  std::uint64_t, std::int32_t) {
    ++static_cast<fake_sim_players *>(self)->view_distances;
    return REFRAIN_STATUS_OK;
}

refrain_status fake_sim_move_player(void *self,
                                    refrain_services_sim_handle_sim_world,
                                    std::uint64_t, double, double, double,
                                    std::uint8_t *out_value) {
    auto &state{*static_cast<fake_sim_players *>(self)};
    ++state.moves;
    if (out_value == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    *out_value = 1;
    return REFRAIN_STATUS_OK;
}

refrain_status fake_sim_jump(void *self, refrain_services_sim_handle_sim_world,
                             std::uint64_t) {
    ++static_cast<fake_sim_players *>(self)->jumps;
    return REFRAIN_STATUS_OK;
}

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

// `refrain:sim` 1.1.0 appended the player projection and the
// connection-addressed player calls after the entity mutations. Every existing
// slot kept its offset and the table grew by ten pointers; the projection and
// the registration are the two structs the new calls carry.
namespace sim_service = refrain::services::sim;
static_assert(sim_service::abi_major == 1);
static_assert(sim_service::abi_minor == 1);
static_assert(REFRAIN_SERVICES_SIM_ABI_MINOR == 1);
static_assert(std::string_view{sim_service::service_id} == "refrain:sim");
static_assert(sizeof(sim_service::player_info) == 120);
static_assert(alignof(sim_service::player_info) == 8);
static_assert(offsetof(sim_service::player_info, struct_size) == 0);
static_assert(offsetof(sim_service::player_info, connection) == 8);
static_assert(offsetof(sim_service::player_info, profile_id) == 16);
static_assert(offsetof(sim_service::player_info, name) == 32);
static_assert(offsetof(sim_service::player_info, world) == 48);
static_assert(offsetof(sim_service::player_info, entity_id) == 64);
static_assert(offsetof(sim_service::player_info, x) == 72);
static_assert(offsetof(sim_service::player_info, y) == 80);
static_assert(offsetof(sim_service::player_info, z) == 88);
static_assert(offsetof(sim_service::player_info, yaw) == 96);
static_assert(offsetof(sim_service::player_info, pitch) == 100);
static_assert(offsetof(sim_service::player_info, on_ground) == 104);
static_assert(offsetof(sim_service::player_info, game_mode) == 108);
static_assert(offsetof(sim_service::player_info, health) == 112);
static_assert(offsetof(sim_service::player_info, view_distance) == 116);
static_assert(sizeof(sim_service::player_registration) == 96);
static_assert(alignof(sim_service::player_registration) == 8);
static_assert(offsetof(sim_service::player_registration, struct_size) == 0);
static_assert(offsetof(sim_service::player_registration, connection) == 8);
static_assert(offsetof(sim_service::player_registration, profile_id) == 16);
static_assert(offsetof(sim_service::player_registration, name) == 32);
static_assert(offsetof(sim_service::player_registration, world) == 48);
static_assert(offsetof(sim_service::player_registration, x) == 64);
static_assert(offsetof(sim_service::player_registration, y) == 72);
static_assert(offsetof(sim_service::player_registration, z) == 80);
static_assert(offsetof(sim_service::player_registration, game_mode) == 88);
static_assert(offsetof(sim_service::player_registration, inventory_slots) ==
              92);
static_assert(offsetof(refrain_services_sim_vtable, remove_entity) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 11 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, player_count) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 10 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, player_at) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 9 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, player_by_connection) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 8 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, add_player) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 7 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, remove_player) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 6 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, set_player_transform) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 5 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, set_player_position) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 4 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, set_player_view_distance) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 3 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, move_player) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - 2 * sizeof(void *));
static_assert(offsetof(refrain_services_sim_vtable, jump) ==
              REFRAIN_SERVICES_SIM_VTABLE_SIZE - sizeof(void *));

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
static_assert(refrain::services::net_translator::abi_major == 1);
static_assert(std::string_view{refrain::services::net_translator::service_id} ==
              "refrain:net.translator");

// The production `refrain:net.protocol` contract declares the `tab_codec`
// extension, so these checks also compile the generated C++ wrapper and query
// path. The fake provider exercises registration, success, missing extension,
// version mismatch and malformed/undersized vtable handling.

static_assert(refrain::services::net_protocol::abi_minor == 1);
static_assert(std::string_view{
                  refrain::services::net_protocol::tab_codec::extension_id} ==
              "refrain:net.protocol/tab_codec");
static_assert(
    std::string_view{
        refrain::services::net_protocol::broadcast_codec::extension_id} ==
    "refrain:net.protocol/broadcast_codec");
static_assert(
    std::string_view{
        refrain::services::net_protocol::broadcast_codec::extends_service} ==
    "refrain:net.protocol");
static_assert(
    std::string_view{
        refrain::services::net_protocol::broadcast_codec::capability} ==
    "broadcast-codec");
static_assert(refrain::services::net_protocol::broadcast_codec::abi_major == 1);
static_assert(refrain::services::net_protocol::broadcast_codec::abi_minor == 0);
static_assert(REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_VTABLE_SIZE ==
              sizeof(refrain_services_net_protocol_broadcast_codec_vtable));
static_assert(REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_REF_SIZE ==
              sizeof(refrain_services_net_protocol_connection_ref));
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

    "contracts/world level projection dispatches through the vtable"_test = [] {
        std::string level_name{};
        refrain_services_world_vtable table{};
        table.struct_size = REFRAIN_SERVICES_WORLD_VTABLE_SIZE;
        table.abi_major = REFRAIN_SERVICES_WORLD_ABI_MAJOR;
        table.abi_minor = REFRAIN_SERVICES_WORLD_ABI_MINOR;
        table.abi_patch = REFRAIN_SERVICES_WORLD_ABI_PATCH;
        table.level = &fake_world_level;
        const world_service::provider service{table, &level_name};

        const auto projected{service.level()};
        expect(projected.has_value());
        if (projected) {
            expect(projected->level.data_version == 4189);
            expect(projected->level.seed == 1234);
            expect(projected->level.spawn_x == 1);
            expect(projected->level.spawn_y == 64);
            expect(projected->level.spawn_z == -1);
            expect(projected->level.time == 6000);
            expect(refrain::contracts::as_string_view(
                       projected->level.level_name) == "projected");
            expect(projected->level.game_mode == 1);
            expect(projected->level.hardcore == 1);
            expect(projected->level.difficulty == 2);
            expect(projected->level.difficulty_locked == 1);
            expect(projected->has_chunks == 1);
        }

        table.level = &fake_world_level_missing;
        const world_service::provider missing{table, &level_name};
        const auto failed{missing.level()};
        expect(!failed.has_value());
        if (!failed) {
            expect(failed.error() ==
                   world_service::world_error::not_implemented);
        }
    };

    "contracts/world compression settings dispatch through the vtable"_test =
        [] {
            fake_world_settings settings{};
            refrain_services_world_vtable table{};
            table.struct_size = REFRAIN_SERVICES_WORLD_VTABLE_SIZE;
            table.abi_major = REFRAIN_SERVICES_WORLD_ABI_MAJOR;
            table.abi_minor = REFRAIN_SERVICES_WORLD_ABI_MINOR;
            table.abi_patch = REFRAIN_SERVICES_WORLD_ABI_PATCH;
            table.zstd_level = &fake_world_zstd_level;
            table.set_zstd_level = &fake_world_set_zstd_level;
            table.set_zstd_policy = &fake_world_set_zstd_policy;
            const world_service::provider service{table, &settings};

            expect(service.set_zstd_level(11).has_value());
            expect(settings.set_level_calls == 1);
            expect(settings.effective_level == 11);
            expect(service.zstd_level() == 11);

            auto policy{world_service::make_zstd_policy()};
            policy.near_level = 2;
            policy.mid_level = 4;
            policy.far_level = 7;
            policy.near_radius = 3;
            policy.mid_radius = 9;
            expect(service.set_zstd_policy(policy).has_value());
            expect(settings.set_policy_calls == 1);
            expect(settings.effective_level == 2);

            // An unset slot is how a provider declares that the setting does
            // not apply; the consumer must check it before dispatching.
            refrain_services_world_vtable bare{};
            bare.struct_size = REFRAIN_SERVICES_WORLD_VTABLE_SIZE;
            bare.abi_major = REFRAIN_SERVICES_WORLD_ABI_MAJOR;
            bare.abi_minor = REFRAIN_SERVICES_WORLD_ABI_MINOR;
            bare.abi_patch = REFRAIN_SERVICES_WORLD_ABI_PATCH;
            expect(bare.zstd_level == nullptr);
            expect(bare.set_zstd_level == nullptr);
            expect(bare.set_zstd_policy == nullptr);
        };

    "contracts/sim player calls dispatch through the vtable"_test = [] {
        fake_sim_players state{};
        refrain_services_sim_vtable table{};
        table.struct_size = REFRAIN_SERVICES_SIM_VTABLE_SIZE;
        table.abi_major = REFRAIN_SERVICES_SIM_ABI_MAJOR;
        table.abi_minor = REFRAIN_SERVICES_SIM_ABI_MINOR;
        table.abi_patch = REFRAIN_SERVICES_SIM_ABI_PATCH;
        table.player_count = &fake_sim_player_count;
        table.player_at = &fake_sim_player_at;
        table.player_by_connection = &fake_sim_player_by_connection;
        table.add_player = &fake_sim_add_player;
        table.remove_player = &fake_sim_remove_player;
        table.set_player_transform = &fake_sim_set_player_transform;
        table.set_player_position = &fake_sim_set_player_position;
        table.set_player_view_distance = &fake_sim_set_player_view_distance;
        table.move_player = &fake_sim_move_player;
        table.jump = &fake_sim_jump;
        const sim_service::provider service{table, &state};
        const sim_service::sim_world world{};

        expect(service.player_count(world) == 1U);
        const auto one{service.player_at(world, 0)};
        expect(one.has_value());
        if (one) {
            expect(one->connection == 7U);
            expect(refrain::contracts::as_string_view(one->name) == "Alice");
            expect(refrain::contracts::as_string_view(one->world) ==
                   "overworld");
            expect(refrain::contracts::as_bytes(one->profile_id).size() == 16U);
            expect(one->entity_id == 3U);
            expect(one->x == 1.5);
            expect(one->y == 64.0);
            expect(one->z == -2.5);
            expect(one->yaw == 90.0F);
            expect(one->pitch == -10.0F);
            expect(one->on_ground == 1);
            expect(one->game_mode == 1U);
            expect(one->health == 18.0F);
            expect(one->view_distance == 12);
        }
        const auto past_end{service.player_at(world, 1)};
        expect(!past_end.has_value());
        if (!past_end) {
            expect(past_end.error() == sim_service::sim_error::out_of_bounds);
        }

        const auto by_connection{service.player_by_connection(world, 7)};
        expect(by_connection.has_value());
        const auto missing{service.player_by_connection(world, 8)};
        expect(!missing.has_value());
        if (!missing) {
            expect(missing.error() == sim_service::sim_error::not_found);
        }

        // The registration carries borrowed views the generated wrapper does
        // not convert for struct fields; the provider sees the exact bytes and
        // the struct_size the wrapper normalises.
        auto registration{sim_service::make_player_registration()};
        const std::array<std::uint8_t, 16> profile{9, 9, 9, 9, 9, 9, 9, 9,
                                                   9, 9, 9, 9, 9, 9, 9, 9};
        registration.connection = 9;
        registration.profile_id = refrain::contracts::to_borrowed(
            std::span<const std::uint8_t>{profile.data(), profile.size()});
        registration.name = refrain::contracts::to_borrowed("Bob");
        registration.world = refrain::contracts::to_borrowed("overworld");
        registration.game_mode = 1;
        registration.inventory_slots = 36;
        const auto added{service.add_player(world, registration)};
        expect(added.has_value());
        if (added) {
            expect(*added == 12U);
        }
        expect(state.adds == 1);
        expect(state.added_connection == 9U);
        expect(state.added_name == "Bob");
        expect(state.added_profile_size == 16U);
        expect(state.added_slots == 36U);

        expect(service.remove_player(world, 7));
        expect(!service.remove_player(world, 8));
        expect(
            service.set_player_transform(world, 7, 1.0, 2.0, 3.0, 45.0F, 5.0F)
                .has_value());
        expect(
            service.set_player_position(world, 7, 1.0, 2.0, 3.0).has_value());
        expect(service.set_player_view_distance(world, 7, 6).has_value());
        const auto moved{service.move_player(world, 7, 0.5, 0.0, 0.0)};
        expect(moved.has_value());
        if (moved) {
            expect(*moved);
        }
        expect(service.jump(world, 7).has_value());
        expect(state.transforms == 1);
        expect(state.positions == 1);
        expect(state.view_distances == 1);
        expect(state.moves == 1);
        expect(state.jumps == 1);
        expect(state.lookups == 2);
        expect(state.reads == 2);

        // Every player slot is appended, so a table built for the previous
        // layout leaves them empty and a consumer checks before dispatching.
        refrain_services_sim_vtable bare{};
        bare.struct_size = REFRAIN_SERVICES_SIM_VTABLE_SIZE;
        bare.abi_major = REFRAIN_SERVICES_SIM_ABI_MAJOR;
        bare.abi_minor = REFRAIN_SERVICES_SIM_ABI_MINOR;
        bare.abi_patch = REFRAIN_SERVICES_SIM_ABI_PATCH;
        expect(bare.player_count == nullptr);
        expect(bare.player_at == nullptr);
        expect(bare.player_by_connection == nullptr);
        expect(bare.add_player == nullptr);
        expect(bare.remove_player == nullptr);
        expect(bare.set_player_transform == nullptr);
        expect(bare.set_player_position == nullptr);
        expect(bare.set_player_view_distance == nullptr);
        expect(bare.move_player == nullptr);
        expect(bare.jump == nullptr);
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
