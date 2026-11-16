// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/chunk_streamer.hpp>

#include <refrain/session/chunk_tracker.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace server = refrain::server;
namespace session = refrain::session;
namespace sim = refrain::sim;

/// One chunk coordinate as the fake encoder writes it into the body, so a
/// test can read the order the streamer drained without decoding a packet.
struct coordinate {
    std::int32_t x{0};
    std::int32_t z{0};

    friend auto operator==(const coordinate &, const coordinate &)
        -> bool = default;
};

/// Records the chunk batches the streamer sends and reports the backlog the
/// test sets per connection.
class recording_gateway final : public server::play_gateway {
  public:
    struct batch {
        std::uint64_t connection{0};
        std::int32_t chunk_x{0};
        std::int32_t chunk_z{0};
        std::vector<std::vector<std::byte>> packets{};
        bool update_center{false};
    };

    struct forgotten_chunk {
        std::uint64_t connection{0};
        std::int32_t chunk_x{0};
        std::int32_t chunk_z{0};
    };

    struct raw_packet {
        std::uint64_t connection{0};
        std::vector<std::byte> body{};
    };

    [[nodiscard]] auto available() const -> bool override { return true; }

    void send_raw(std::uint64_t connection,
                  std::span<const std::byte> body) override {
        raw.push_back(raw_packet{
            .connection = connection,
            .body = std::vector<std::byte>{body.begin(), body.end()},
        });
    }

    [[nodiscard]] auto active_connections() const
        -> std::vector<std::uint64_t> override {
        return active;
    }

    [[nodiscard]] auto backlog(std::uint64_t connection) const
        -> std::size_t override {
        const auto found{backlogs.find(connection)};
        return found == backlogs.end() ? 0 : found->second;
    }

    void send_chunk_batch(std::uint64_t connection, std::int32_t chunk_x,
                          std::int32_t chunk_z,
                          std::span<const std::vector<std::byte>> packets,
                          bool update_center) override {
        batches.push_back(batch{
            .connection = connection,
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
            .packets = std::vector<std::vector<std::byte>>{packets.begin(),
                                                           packets.end()},
            .update_center = update_center,
        });
    }

    void send_forget_chunk(std::uint64_t connection, std::int32_t chunk_x,
                           std::int32_t chunk_z) override {
        forgotten.push_back(forgotten_chunk{
            .connection = connection,
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
        });
    }

    [[nodiscard]] auto take_creative_slots()
        -> std::vector<refrain::net::creative_slot_event> override {
        return {};
    }
    [[nodiscard]] auto take_container_closes()
        -> std::vector<refrain::net::container_close_event> override {
        return {};
    }
    [[nodiscard]] auto take_container_clicks()
        -> std::vector<refrain::net::container_click_event> override {
        return {};
    }

    void send_container_content(std::uint64_t, std::int32_t, std::int32_t,
                                std::span<const refrain::net::item_stack>,
                                const refrain::net::item_stack &) override {}
    void send_container_set_slot(std::uint64_t, std::int32_t, std::int32_t,
                                 std::int16_t,
                                 const refrain::net::item_stack &) override {}
    void send_cursor_item(std::uint64_t,
                          const refrain::net::item_stack &) override {}

    std::vector<std::uint64_t> active{};
    std::unordered_map<std::uint64_t, std::size_t> backlogs{};
    std::vector<batch> batches{};
    std::vector<forgotten_chunk> forgotten{};
    std::vector<raw_packet> raw{};
};

/// Coordinates of the chunks in the order the streamer queues one viewport
/// around the origin: ring by ring, then by z and x.
[[nodiscard]] auto viewport_order(std::int32_t radius)
    -> std::vector<coordinate> {
    std::vector<coordinate> out{};
    for (std::int32_t distance{0}; distance <= radius; ++distance) {
        for (std::int32_t dz{-distance}; dz <= distance; ++dz) {
            for (std::int32_t dx{-distance}; dx <= distance; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != distance) {
                    continue;
                }
                out.push_back({dx, dz});
            }
        }
    }
    return out;
}

/// The streamed coordinates of one connection, in wire order.
[[nodiscard]] auto streamed(const recording_gateway &gateway,
                            std::uint64_t connection)
    -> std::vector<coordinate> {
    std::vector<coordinate> out{};
    for (const auto &batch : gateway.batches) {
        if (batch.connection != connection || batch.packets.empty()) {
            continue;
        }
        for (const auto &body : batch.packets) {
            coordinate value{};
            if (body.size() == sizeof(value)) {
                std::memcpy(&value, body.data(), sizeof(value));
            }
            out.push_back(value);
        }
    }
    return out;
}

/// State shared with the encoder so a test can change one encode's cost
/// between ticks and count the encodes the streamer asked for.
struct fake_encoder {
    std::chrono::nanoseconds cost{};
    std::size_t calls{0};
};

[[nodiscard]] auto make_encoder(std::shared_ptr<fake_encoder> state)
    -> server::chunk_streamer::chunk_encoder {
    return
        [state](const server::chunk_packet_source &,
                sim::chunk_pos pos) -> refrain::result<std::vector<std::byte>> {
            ++state->calls;
            if (state->cost.count() > 0) {
                std::this_thread::sleep_for(state->cost);
            }
            const coordinate value{pos.x, pos.z};
            std::vector<std::byte> body(sizeof(value));
            std::memcpy(body.data(), &value, sizeof(value));
            return body;
        };
}

namespace play_cb = refrain::protocol::current::play::clientbound;

/// A minimal tracked table set with the static entity-type registry the spawn
/// packets resolve their ids from.
struct entity_type_tables {
    std::vector<std::byte> bytes{};
    std::optional<refrain::data::vanilla_tables> tables{};

    entity_type_tables() {
        refrain::data::vanilla_spec spec{};
        spec.data_version = 5023;
        refrain::data::block_spec air{};
        air.name = "minecraft:air";
        air.states.push_back(refrain::data::state_spec{
            .vanilla_id = 0, .values = {}, .is_default = true});
        spec.blocks = {std::move(air)};
        spec.biome_presets.push_back(refrain::data::biome_preset_spec{
            .name = "minecraft:overworld",
            .entries = {refrain::data::biome_parameter_spec{
                .biome = "minecraft:plains",
                .min = {0, 0, 0, 0, 0, 0},
                .max = {0, 0, 0, 0, 0, 0},
                .offset = 0,
            }},
        });
        spec.registries.push_back(refrain::data::registry_spec{
            .name = "minecraft:entity_type",
            .entries =
                {
                    refrain::data::registry_entry_spec{.name = "minecraft:pig",
                                                       .protocol_id = 101},
                    refrain::data::registry_entry_spec{
                        .name = "minecraft:item_frame", .protocol_id = 74},
                    refrain::data::registry_entry_spec{
                        .name = "minecraft:shulker", .protocol_id = 115},
                },
        });
        auto built{refrain::data::build_vanilla_tables(spec)};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        bytes = std::move(*built);
        auto opened{refrain::data::vanilla_tables::open(bytes)};
        expect(static_cast<bool>(opened));
        if (opened) {
            tables = std::move(*opened);
        }
    }
};

[[nodiscard]] auto entity_components(std::optional<std::int8_t> facing)
    -> std::vector<std::byte> {
    refrain::data::nbt::compound tag{};
    if (facing.has_value()) {
        tag["Facing"] = refrain::data::nbt::value{*facing};
    }
    auto bytes{
        refrain::data::nbt::write(refrain::data::nbt::value{std::move(tag)}, "",
                                  refrain::data::nbt::compression::none)};
    expect(static_cast<bool>(bytes));
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

[[nodiscard]] auto
make_chunk_entity(refrain::sim::entity_id id, std::string type, double x,
                  double y, double z, float yaw, std::vector<std::byte> data)
    -> refrain::sim::chunk_entity {
    refrain::world::entity_record record{};
    record.id = std::move(type);
    record.x = x;
    record.y = y;
    record.z = z;
    record.yaw = yaw;
    record.pitch = 0.0F;
    record.pos = refrain::world::block_pos{
        .x = static_cast<std::int32_t>(x),
        .y = static_cast<std::int16_t>(y),
        .z = static_cast<std::int32_t>(z),
    };
    record.data = std::move(data);
    return refrain::sim::chunk_entity{.id = id, .record = std::move(record)};
}

/// Packet id of one raw body, or nullopt when the body is empty.
[[nodiscard]] auto raw_packet_id(std::span<const std::byte> body)
    -> std::optional<std::uint32_t> {
    refrain::net::reader in{body};
    const auto id{in.read_varint()};
    if (!id) {
        return std::nullopt;
    }
    return *id;
}

suite<"server/chunk-streamer"> chunk_streamer_tests = [] {
    "server/chunk stream stops at the tick budget and resumes in order"_test =
        [] {
            recording_gateway gateway;
            gateway.active = {7};
            auto state{std::make_shared<fake_encoder>()};
            // One encode spends the whole budget, so every pass sends exactly
            // one chunk and leaves the rest at the front of the queue.
            state->cost = std::chrono::nanoseconds{
                server::chunk_streamer::tick_budget_ns};
            server::chunk_streamer streamer{gateway,
                                            server::chunk_packet_source{}};
            streamer.set_encoder(make_encoder(state));
            session::chunk_tracker views;
            static_cast<void>(views.update(7, 0, 0, 2));
            const auto expected{viewport_order(2)};

            for (std::size_t tick{1}; tick <= 4; ++tick) {
                streamer.tick(views);
                expect(state->calls == tick);
                const std::vector<coordinate> sent{streamed(gateway, 7)};
                expect(sent.size() == tick);
                expect(std::equal(sent.begin(), sent.end(), expected.begin()));
            }

            // A free encode is not a tick of work either: the rest drains in
            // one or two passes, not one chunk per tick.
            state->cost = std::chrono::nanoseconds{0};
            streamer.tick(views);
            streamer.tick(views);
            expect(streamed(gateway, 7) == expected);
            expect(state->calls == expected.size());
        };

    "server/chunk cache keeps one full viewport resident"_test = [] {
        recording_gateway gateway;
        gateway.active = {1, 2};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_streamer streamer{gateway, server::chunk_packet_source{}};
        streamer.set_encoder(make_encoder(state));
        session::chunk_tracker views;
        static_cast<void>(views.update(1, 0, 0, 10));
        const auto expected{viewport_order(10)};

        // The first connection fills its viewport. A slow host can split the
        // drain across ticks; that must not change the order.
        std::size_t guard{0};
        while (streamed(gateway, 1).size() < expected.size() && guard < 64) {
            streamer.tick(views);
            ++guard;
        }
        expect(streamed(gateway, 1) == expected);
        expect(state->calls == expected.size());
        expect(streamer.cache_misses() == expected.size());
        expect(streamer.cache_hits() == 0U);

        // A second connection joins the same viewport after the first one is
        // done: every body is still resident. A cache smaller than the
        // viewport (the old fixed 256 against these 441 chunks) would have
        // evicted the first rings and encoded them again.
        static_cast<void>(views.update(2, 0, 0, 10));
        guard = 0;
        while (streamed(gateway, 2).size() < expected.size() && guard < 64) {
            streamer.tick(views);
            ++guard;
        }
        expect(streamed(gateway, 2) == expected);
        expect(state->calls == expected.size());
        // Every lookup of the second viewport was a hit.
        expect(streamer.cache_hits() == expected.size());
        expect(streamer.cache_misses() == expected.size());
    };

    "server/chunk stream leaves a slow client pending and resumes later"_test =
        [] {
            recording_gateway gateway;
            gateway.active = {4};
            auto state{std::make_shared<fake_encoder>()};
            server::chunk_streamer streamer{gateway,
                                            server::chunk_packet_source{}};
            streamer.set_encoder(make_encoder(state));
            session::chunk_tracker views;
            static_cast<void>(views.update(4, 0, 0, 0));
            // Above the per-connection bound: the streamer must not add to a
            // queue the socket is not draining.
            gateway.backlogs[4] = 2U * 1024U * 1024U;
            streamer.tick(views);
            expect(state->calls == 0U);
            expect(streamed(gateway, 4).empty());

            // The socket caught up: the chunk left pending arrives next tick.
            gateway.backlogs[4] = 0;
            streamer.tick(views);
            expect(state->calls == 1U);
            expect(streamed(gateway, 4) == std::vector<coordinate>{{0, 0}});
        };

    "generated entities follow their chunk and leave with it"_test = [] {
        entity_type_tables tables{};
        expect(tables.tables.has_value());
        if (!tables.tables) {
            return;
        }
        recording_gateway gateway;
        gateway.active = {7};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_packet_source source{};
        source.tables = &*tables.tables;
        server::chunk_streamer streamer{gateway, source};
        streamer.set_encoder(make_encoder(state));
        streamer.set_entity_source(
            [](sim::chunk_pos pos) -> std::vector<sim::chunk_entity> {
                if (pos.x != 0 || pos.z != 0) {
                    return {};
                }
                return {make_chunk_entity(42, "minecraft:shulker", 0.5, 65.0,
                                          0.5, 90.0F,
                                          entity_components(std::nullopt))};
            });
        session::chunk_tracker views;
        static_cast<void>(views.update(7, 0, 0, 0));
        streamer.tick(views);

        // The chunk batch comes first, then the entity spawn.
        expect(gateway.batches.size() == 1U);
        expect(gateway.batches.front().packets.size() == 1U);
        std::vector<const recording_gateway::raw_packet *> spawns{};
        for (const auto &body : gateway.raw) {
            if (raw_packet_id(body.body) ==
                std::optional<std::uint32_t>{play_cb::add_entity::packet_id}) {
                spawns.push_back(&body);
            }
        }
        expect(spawns.size() == 1U);
        if (spawns.size() != 1U) {
            return;
        }
        refrain::net::reader in{spawns.front()->body};
        const auto id{in.read_varint()};
        expect(id.has_value());
        play_cb::add_entity packet{};
        expect(static_cast<bool>(play_cb::decode_add_entity(in, packet)));
        expect(packet.id == 42);
        expect(packet.type_ == 115U);
        expect(packet.x == 0.5);
        expect(packet.y == 65.0);
        expect(packet.z == 0.5);
        // 90 degrees packs to 64 in the reference rotation encoding.
        expect(static_cast<int>(packet.y_rot) == 64);
        expect(static_cast<int>(packet.y_head_rot) == 64);

        // A second pass over the same viewport does not announce the entity
        // again.
        streamer.tick(views);
        expect(gateway.raw.size() == spawns.size());

        // Walking away removes the entity before the client forgets the chunk.
        static_cast<void>(views.update(7, 20, 0, 0));
        streamer.tick(views);
        expect(gateway.forgotten.size() == 1U);
        std::vector<const recording_gateway::raw_packet *> removals{};
        for (const auto &body : gateway.raw) {
            if (raw_packet_id(body.body) ==
                std::optional<std::uint32_t>{
                    play_cb::remove_entities::packet_id}) {
                removals.push_back(&body);
            }
        }
        expect(removals.size() == 1U);
        if (removals.size() == 1U) {
            refrain::net::reader removal_reader{removals.front()->body};
            const auto remove_id{removal_reader.read_varint()};
            expect(remove_id.has_value());
            play_cb::remove_entities removal{};
            expect(static_cast<bool>(
                play_cb::decode_remove_entities(removal_reader, removal)));
            expect(removal.entity_ids == std::vector<std::uint32_t>{42U});
        }

        // Returning to the column announces the entity again: the scrollback
        // client lost it with the chunk.
        static_cast<void>(views.update(7, 0, 0, 0));
        streamer.tick(views);
        std::size_t spawn_total{0};
        for (const auto &body : gateway.raw) {
            if (raw_packet_id(body.body) ==
                std::optional<std::uint32_t>{play_cb::add_entity::packet_id}) {
                ++spawn_total;
            }
        }
        expect(spawn_total == 2U);
    };

    "an item frame spawn carries its hanging direction"_test = [] {
        entity_type_tables tables{};
        expect(tables.tables.has_value());
        if (!tables.tables) {
            return;
        }
        recording_gateway gateway;
        gateway.active = {11};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_packet_source source{};
        source.tables = &*tables.tables;
        server::chunk_streamer streamer{gateway, source};
        streamer.set_encoder(make_encoder(state));
        streamer.set_entity_source(
            [](sim::chunk_pos pos) -> std::vector<sim::chunk_entity> {
                if (pos.x != 0 || pos.z != 0) {
                    return {};
                }
                return {make_chunk_entity(77, "minecraft:item_frame", 1.5, 66.0,
                                          1.5, 180.0F, entity_components(3))};
            });
        session::chunk_tracker views;
        static_cast<void>(views.update(11, 0, 0, 0));
        streamer.tick(views);
        expect(gateway.raw.size() == 1U);
        if (gateway.raw.empty()) {
            return;
        }
        refrain::net::reader in{gateway.raw.front().body};
        const auto id{in.read_varint()};
        expect(id.has_value());
        play_cb::add_entity packet{};
        expect(static_cast<bool>(play_cb::decode_add_entity(in, packet)));
        expect(packet.type_ == 74U);
        expect(packet.data == 3);
    };

    "an entity type the static registry does not know is skipped"_test = [] {
        entity_type_tables tables{};
        expect(tables.tables.has_value());
        if (!tables.tables) {
            return;
        }
        recording_gateway gateway;
        gateway.active = {12};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_packet_source source{};
        source.tables = &*tables.tables;
        server::chunk_streamer streamer{gateway, source};
        streamer.set_encoder(make_encoder(state));
        streamer.set_entity_source(
            [](sim::chunk_pos pos) -> std::vector<sim::chunk_entity> {
                if (pos.x != 0 || pos.z != 0) {
                    return {};
                }
                return {make_chunk_entity(78, "minecraft:not_a_type", 0.5, 65.0,
                                          0.5, 0.0F,
                                          entity_components(std::nullopt))};
            });
        session::chunk_tracker views;
        static_cast<void>(views.update(12, 0, 0, 0));
        streamer.tick(views);
        expect(gateway.batches.size() == 1U);
        expect(gateway.raw.empty());
    };
};

} // namespace
