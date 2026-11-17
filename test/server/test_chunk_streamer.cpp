// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/error.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_streamer.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <refrain/session/chunk_tracker.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
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

/// The columns of one viewport, as a set of absolute coordinates.
[[nodiscard]] auto viewport_set(std::int32_t center_x, std::int32_t center_z,
                                std::int32_t radius)
    -> std::set<std::pair<std::int32_t, std::int32_t>> {
    std::set<std::pair<std::int32_t, std::int32_t>> out{};
    for (std::int32_t dz{-radius}; dz <= radius; ++dz) {
        for (std::int32_t dx{-radius}; dx <= radius; ++dx) {
            out.insert({center_x + dx, center_z + dz});
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

// --- world-backed cache revalidation fixtures ------------------------------

namespace world = refrain::world;
namespace storage = refrain::storage;
namespace test_support = refrain::test_support;

constexpr std::size_t world_section_entries{4096};
constexpr std::size_t wire_section_count{24};

/// One storage chunk with a single stone section, enough for the packet
/// encoder and for a block edit to change the column revision.
[[nodiscard]] auto flat_chunk() -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = -4;
    storage::section_record section{};
    section.y = -4;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world_section_entries);
    section.palette.push_back(storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world_section_entries),
        .state = 0,
    });
    section.biome_kind = storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    chunk.sections.push_back(std::move(section));
    return chunk;
}

/// Block tables with two distinct vanilla state ids so a decoded packet shows
/// which block the encoder read.
[[nodiscard]] auto two_block_tables()
    -> std::optional<refrain::data::vanilla_tables> {
    refrain::data::vanilla_spec spec{};
    spec.data_version = 5023;
    for (const auto &[name, vanilla_id] :
         std::array<std::pair<std::string_view, std::uint32_t>, 2>{
             {std::pair{"minecraft:stone", 1U},
              std::pair{"minecraft:gold_block", 2U}}}) {
        refrain::data::block_spec block{};
        block.name = std::string{name};
        block.states.push_back(refrain::data::state_spec{
            .vanilla_id = static_cast<std::uint16_t>(vanilla_id),
            .values = {},
            .is_default = true,
        });
        spec.blocks.push_back(std::move(block));
    }
    auto bytes{refrain::data::build_vanilla_tables(spec)};
    if (!bytes) {
        return std::nullopt;
    }
    auto tables{refrain::data::vanilla_tables::open(*bytes, false)};
    if (!tables) {
        return std::nullopt;
    }
    return std::optional{std::move(*tables)};
}

/// Open a synthetic world holding `flat_chunk()` at (0, 0).
[[nodiscard]] auto open_flat_world(const std::filesystem::path &directory)
    -> std::optional<sim::world> {
    auto store{world::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    if (!store->write_chunk(world::chunk_pos{0, 0}, flat_chunk()) ||
        !store->commit()) {
        return std::nullopt;
    }
    auto opened{sim::world::open(directory.string())};
    if (!opened) {
        return std::nullopt;
    }
    return std::optional{std::move(*opened)};
}

/// Column coordinates one chunk packet body was encoded for.
[[nodiscard]] auto packet_position(std::span<const std::byte> body)
    -> std::optional<coordinate> {
    refrain::net::reader in{body};
    const auto id{in.read_varint()};
    if (!id || *id != 46U) {
        return std::nullopt;
    }
    const auto x{in.read_i32()};
    const auto z{in.read_i32()};
    if (!x || !z) {
        return std::nullopt;
    }
    return coordinate{*x, *z};
}

/// The most recent chunk packet body one connection received for a column.
[[nodiscard]] auto latest_column_body(const recording_gateway &gateway,
                                      std::uint64_t connection,
                                      coordinate target)
    -> std::optional<std::vector<std::byte>> {
    for (auto batch = gateway.batches.rbegin(); batch != gateway.batches.rend();
         ++batch) {
        if (batch->connection != connection) {
            continue;
        }
        for (const auto &body : batch->packets) {
            if (packet_position(body) == target) {
                return body;
            }
        }
    }
    return std::nullopt;
}

/// One decoded section of a `level_chunk_with_light` body.
struct decoded_section {
    std::uint8_t bits{0};
    std::vector<std::uint32_t> palette{};
    std::vector<std::uint64_t> words{};
};

/// Decode section `section_index` of a chunk packet body. The test only needs
/// the block palette and the packed indices of one section.
[[nodiscard]] auto decode_column_section(std::span<const std::byte> body,
                                         std::size_t section_index)
    -> std::optional<decoded_section> {
    refrain::net::reader in{body};
    const auto packet_id{in.read_varint()};
    if (!packet_id || *packet_id != 46U || !in.read_i32() || !in.read_i32()) {
        return std::nullopt;
    }
    const auto heightmaps{in.read_varint()};
    if (!heightmaps) {
        return std::nullopt;
    }
    for (std::uint32_t entry{0}; entry < *heightmaps; ++entry) {
        const auto type{in.read_varint()};
        const auto words{in.read_varint()};
        if (!type || !words) {
            return std::nullopt;
        }
        for (std::uint32_t word{0}; word < *words; ++word) {
            if (!in.read_i64()) {
                return std::nullopt;
            }
        }
    }
    const auto section_bytes{in.read_varint()};
    if (!section_bytes) {
        return std::nullopt;
    }
    const auto payload{in.read_bytes(*section_bytes)};
    if (!payload) {
        return std::nullopt;
    }
    refrain::net::reader sections{*payload};
    for (std::size_t index{0}; index < wire_section_count; ++index) {
        const auto non_empty{sections.read_u16()};
        const auto fluids{sections.read_u16()};
        const auto bits{sections.read_u8()};
        if (!non_empty || !fluids || !bits) {
            return std::nullopt;
        }
        decoded_section decoded{};
        decoded.bits = *bits;
        if (*bits == 0) {
            const auto entry{sections.read_varint()};
            if (!entry) {
                return std::nullopt;
            }
            decoded.palette.push_back(*entry);
        } else if (*bits <= 8) {
            const auto count{sections.read_varint()};
            if (!count) {
                return std::nullopt;
            }
            for (std::uint32_t entry{0}; entry < *count; ++entry) {
                const auto id{sections.read_varint()};
                if (!id) {
                    return std::nullopt;
                }
                decoded.palette.push_back(*id);
            }
        }
        if (*bits > 0) {
            const auto per_word{64U / *bits};
            const auto words{(world_section_entries + per_word - 1) / per_word};
            decoded.words.reserve(words);
            for (std::uint32_t word{0}; word < words; ++word) {
                const auto value{sections.read_i64()};
                if (!value) {
                    return std::nullopt;
                }
                decoded.words.push_back(static_cast<std::uint64_t>(*value));
            }
        }
        // Biome palette: single value when bits is zero, else a packed list.
        const auto biome_bits{sections.read_u8()};
        if (!biome_bits) {
            return std::nullopt;
        }
        if (*biome_bits == 0) {
            if (!sections.read_varint()) {
                return std::nullopt;
            }
        } else {
            if (*biome_bits <= 3) {
                const auto count{sections.read_varint()};
                if (!count) {
                    return std::nullopt;
                }
                for (std::uint32_t entry{0}; entry < *count; ++entry) {
                    if (!sections.read_varint()) {
                        return std::nullopt;
                    }
                }
            }
            constexpr std::size_t biome_entries{64};
            const auto per_word{64U / *biome_bits};
            const auto words{(biome_entries + per_word - 1) / per_word};
            for (std::uint32_t word{0}; word < words; ++word) {
                if (!sections.read_i64()) {
                    return std::nullopt;
                }
            }
        }
        if (index == section_index) {
            return decoded;
        }
    }
    return std::nullopt;
}

/// Block state id the decoded section holds at a local block index.
[[nodiscard]] auto section_state(const decoded_section &section,
                                 std::size_t index)
    -> std::optional<std::uint32_t> {
    if (section.bits == 0) {
        if (section.palette.empty()) {
            return std::nullopt;
        }
        return section.palette.front();
    }
    const auto per_word{64U / section.bits};
    if (section.words.size() * per_word <= index) {
        return std::nullopt;
    }
    const auto value{(section.words[index / per_word] >>
                      ((index % per_word) * section.bits)) &
                     ((std::uint64_t{1} << section.bits) - 1U)};
    if (section.bits <= 8) {
        if (value >= section.palette.size()) {
            return std::nullopt;
        }
        return section.palette[static_cast<std::size_t>(value)];
    }
    return static_cast<std::uint32_t>(value);
}

/// Wire section index of absolute section Y in the default range.
[[nodiscard]] constexpr auto wire_section(std::int32_t section_y)
    -> std::size_t {
    return static_cast<std::size_t>(section_y + 4);
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

    "server/chunk cache is released after the last stream ends"_test = [] {
        recording_gateway gateway;
        gateway.active = {3};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_streamer streamer{gateway, server::chunk_packet_source{}};
        streamer.set_encoder(make_encoder(state));
        session::chunk_tracker views;
        static_cast<void>(views.update(3, 0, 0, 2));
        const auto expected{viewport_order(2)};
        std::size_t guard{0};
        while (streamed(gateway, 3).size() < expected.size() && guard < 32) {
            streamer.tick(views);
            ++guard;
        }
        expect(streamed(gateway, 3) == expected);
        expect(streamer.cache_entries() == expected.size());

        // The client disconnects. No stream can serve a body again, so the
        // encoded cache is released instead of staying resident until the
        // process exits.
        gateway.active.clear();
        streamer.tick(views);
        expect(streamer.cache_entries() == 0U);
    };

    "server/chunk cache stays bounded along a moving viewport"_test = [] {
        recording_gateway gateway;
        gateway.active = {4};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_streamer streamer{gateway, server::chunk_packet_source{}};
        streamer.set_encoder(make_encoder(state));
        session::chunk_tracker views;
        // A radius-zero viewport reuses one body at most: walking far past it
        // must not accumulate the columns already left.
        constexpr std::size_t radius_zero_capacity{1};
        for (std::int32_t center{0}; center < 24; ++center) {
            static_cast<void>(views.update(4, center, 0, 0));
            streamer.tick(views);
            expect(streamer.cache_entries() <= radius_zero_capacity);
        }
        // A second connection at the same viewport still hits the body the
        // first one encoded.
        gateway.active = {4, 5};
        static_cast<void>(views.update(5, 23, 0, 0));
        const auto misses_before{streamer.cache_misses()};
        streamer.tick(views);
        expect(streamer.cache_misses() == misses_before);
        expect(streamer.cache_entries() <= radius_zero_capacity);
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

    "chunk entities are announced in ascending entity id order"_test = [] {
        entity_type_tables tables{};
        expect(tables.tables.has_value());
        if (!tables.tables) {
            return;
        }
        recording_gateway gateway;
        gateway.active = {13};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_packet_source source{};
        source.tables = &*tables.tables;
        server::chunk_streamer streamer{gateway, source};
        streamer.set_encoder(make_encoder(state));
        // The simulation hands the column's entities over in ascending logical
        // id order; the streamer must keep that order on the wire.
        streamer.set_entity_source(
            [](sim::chunk_pos pos) -> std::vector<sim::chunk_entity> {
                if (pos.x != 0 || pos.z != 0) {
                    return {};
                }
                return {
                    make_chunk_entity(5, "minecraft:shulker", 0.5, 65.0, 0.5,
                                      0.0F, entity_components(std::nullopt)),
                    make_chunk_entity(7, "minecraft:item_frame", 1.5, 66.0, 1.5,
                                      90.0F, entity_components(std::int8_t{3})),
                    make_chunk_entity(9, "minecraft:shulker", 2.5, 67.0, 2.5,
                                      180.0F, entity_components(std::nullopt)),
                };
            });
        session::chunk_tracker views;
        static_cast<void>(views.update(13, 0, 0, 0));
        streamer.tick(views);

        std::vector<std::int32_t> announced{};
        for (const auto &body : gateway.raw) {
            if (raw_packet_id(body.body) !=
                std::optional<std::uint32_t>{play_cb::add_entity::packet_id}) {
                continue;
            }
            refrain::net::reader in{body.body};
            expect(in.read_varint().has_value());
            play_cb::add_entity packet{};
            expect(static_cast<bool>(play_cb::decode_add_entity(in, packet)));
            announced.push_back(packet.id);
        }
        expect(announced == std::vector<std::int32_t>{5, 7, 9});
    };
};

suite<"server/chunk-streamer-prefetch"> chunk_streamer_prefetch_tests = [] {
    "server/chunk stream pre-generates ahead and sends only the accepted viewport"_test =
        [] {
            recording_gateway gateway;
            gateway.active = {7};
            auto state{std::make_shared<fake_encoder>()};
            server::chunk_streamer streamer{gateway,
                                            server::chunk_packet_source{}};
            streamer.set_encoder(make_encoder(state));
            std::vector<coordinate> requested{};
            streamer.set_chunk_requester(
                [&requested](std::int32_t x, std::int32_t z) {
                    requested.push_back({x, z});
                    return true;
                });
            session::chunk_tracker views;
            static_cast<void>(views.update(7, 0, 0, 1));
            streamer.tick(views);
            expect(streamed(gateway, 7).size() == 9U);
            expect(requested.empty());

            // Two accepted centre changes in the same direction. The tracker
            // timestamps them from the same clock the runtime uses, so the
            // spacing has to be real time like a walking player crossing the
            // border every so often.
            const auto step{[&views, &streamer](std::int32_t center_x) {
                std::this_thread::sleep_for(std::chrono::milliseconds{120});
                static_cast<void>(views.update(7, center_x, 0, 1));
                streamer.tick(views);
            }};
            step(1);
            expect(requested.empty());
            step(2);

            // The look-ahead centre is two chunks ahead (the estimate clamps
            // to the tile ahead), so the band is x = 4 and 5 across the view
            // height: pre-generation, never packets.
            expect(requested.size() == 6U);
            for (const auto &position : requested) {
                expect(position.x == 4 || position.x == 5);
                expect(position.z >= -1 && position.z <= 1);
            }
            expect(!streamer.prefetch_columns().empty());
            expect(streamer.prefetch_columns().size() == 6U);
            for (const auto &position : streamer.prefetch_columns()) {
                const auto dx{position.x - 2};
                const auto dz{position.z - 0};
                expect(std::max(std::abs(dx), std::abs(dz)) > 1)
                    << "a prefetched column is inside the accepted viewport";
            }

            // Nothing outside the accepted viewport reached the client: every
            // batch before the last move belongs to an accepted viewport, and
            // the last batch is exactly the viewport around (2, 0).
            const auto batches_before{gateway.batches.size()};
            std::this_thread::sleep_for(std::chrono::milliseconds{120});
            static_cast<void>(views.update(7, 3, 0, 1));
            streamer.tick(views);
            std::set<std::pair<std::int32_t, std::int32_t>> last_batch{};
            for (std::size_t index{batches_before};
                 index < gateway.batches.size(); ++index) {
                for (const auto &body : gateway.batches[index].packets) {
                    coordinate value{};
                    if (body.size() == sizeof(value)) {
                        std::memcpy(&value, body.data(), sizeof(value));
                    }
                    last_batch.insert({value.x, value.z});
                }
            }
            // The viewport around (2, 0) was already sent; the move to (3, 0)
            // adds only its new column, and no prefetched column is in it.
            const std::set<std::pair<std::int32_t, std::int32_t>> added{
                {4, -1}, {4, 0}, {4, 1}};
            expect(last_batch == added);
            for (const auto &position : requested) {
                expect(position.x > 3)
                    << "a look-ahead request was made inside the viewport";
            }
            // The prefetched columns are outside the accepted viewport.
            for (const auto &position : streamer.prefetch_columns()) {
                expect(viewport_set(3, 0, 1).contains(
                           {position.x, position.z}) == false);
            }
        };

    "server/chunk stream stops prefetching when the player reverses"_test = [] {
        recording_gateway gateway;
        gateway.active = {8};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_streamer streamer{gateway, server::chunk_packet_source{}};
        streamer.set_encoder(make_encoder(state));
        std::vector<coordinate> requested{};
        streamer.set_chunk_requester(
            [&requested](std::int32_t x, std::int32_t z) {
                requested.push_back({x, z});
                return true;
            });
        session::chunk_tracker views;
        static_cast<void>(views.update(8, 0, 0, 1));
        streamer.tick(views);
        const auto step{[&views, &streamer](std::int32_t center_x) {
            std::this_thread::sleep_for(std::chrono::milliseconds{120});
            static_cast<void>(views.update(8, center_x, 0, 1));
            streamer.tick(views);
        }};
        step(1);
        step(2);
        const auto forward_requests{requested.size()};
        expect(!streamer.prefetch_columns().empty());

        // One accepted change against the heading collapses the band on that
        // tick: the old direction is not requested again.
        step(1);
        expect(streamer.prefetch_columns().empty());
        const auto after_reverse{requested.size()};
        streamer.tick(views);
        streamer.tick(views);
        expect(requested.size() == after_reverse);
        expect(forward_requests > 0U);
    };

    "server/chunk stream keeps a jittering centre from prefetching"_test = [] {
        recording_gateway gateway;
        gateway.active = {9};
        auto state{std::make_shared<fake_encoder>()};
        server::chunk_streamer streamer{gateway, server::chunk_packet_source{}};
        streamer.set_encoder(make_encoder(state));
        std::vector<coordinate> requested{};
        streamer.set_chunk_requester(
            [&requested](std::int32_t x, std::int32_t z) {
                requested.push_back({x, z});
                return true;
            });
        session::chunk_tracker views;
        static_cast<void>(views.update(9, 0, 0, 1));
        streamer.tick(views);
        const auto step{[&views, &streamer](std::int32_t center_x) {
            std::this_thread::sleep_for(std::chrono::milliseconds{120});
            static_cast<void>(views.update(9, center_x, 0, 1));
            streamer.tick(views);
        }};
        // A player crossing one border back and forth: every change reverses
        // the previous one, so the heading never reaches two steps.
        step(1);
        step(0);
        step(1);
        expect(requested.empty());
        expect(streamer.prefetch_columns().empty());
    };
};

suite<"server/chunk-streamer-cache-revision"> chunk_cache_revision_tests = [] {
    "an edited column is re-encoded when the player returns"_test = [] {
        const test_support::scratch_directory directory{};
        auto runtime{open_flat_world(directory.path())};
        expect(runtime.has_value());
        auto tables{two_block_tables()};
        expect(tables.has_value());
        if (!runtime || !tables) {
            return;
        }
        auto port{server::make_borrowed_world_port(*runtime)};
        const server::chunk_packet_source source{
            .world = port.get(),
            .tables = &*tables,
            .registries = nullptr,
        };
        recording_gateway gateway;
        gateway.active = {1};
        server::chunk_streamer streamer{gateway, source};
        session::chunk_tracker views;
        static_cast<void>(views.update(1, 0, 0, 0));
        streamer.tick(views);
        const auto first{latest_column_body(gateway, 1, {0, 0})};
        expect(first.has_value());
        if (first) {
            const auto before{decode_column_section(*first, wire_section(0))};
            expect(before.has_value());
            if (before) {
                expect(section_state(*before, 0) ==
                       std::optional<std::uint32_t>{0});
            }
        }
        expect(streamer.cache_misses() == 1U);
        expect(streamer.cache_hits() == 0U);

        const auto gold{runtime->registry().intern("minecraft:gold_block")};
        const auto edited{
            runtime->set_block(sim::block_pos{.x = 0, .y = 0, .z = 0}, gold)};
        expect(static_cast<bool>(edited));

        // Walking away forgets the column; coming back asks for it again.
        static_cast<void>(views.update(1, 20, 20, 0));
        streamer.tick(views);
        static_cast<void>(views.update(1, 0, 0, 0));
        streamer.tick(views);

        const auto returned{latest_column_body(gateway, 1, {0, 0})};
        expect(returned.has_value());
        if (returned) {
            const auto after{decode_column_section(*returned, wire_section(0))};
            expect(after.has_value());
            if (after) {
                // The pre-edit body would still hold air here; the edit has to
                // be visible, not a cached copy of the old column.
                expect(section_state(*after, 0) ==
                       std::optional<std::uint32_t>{2});
            }
        }
        // The stale entry is a miss, never a hit.
        expect(streamer.cache_misses() == 2U);
        expect(streamer.cache_hits() == 0U);
    };

    "an edited column is re-encoded for a joining connection"_test = [] {
        const test_support::scratch_directory directory{};
        auto runtime{open_flat_world(directory.path())};
        expect(runtime.has_value());
        auto tables{two_block_tables()};
        expect(tables.has_value());
        if (!runtime || !tables) {
            return;
        }
        auto port{server::make_borrowed_world_port(*runtime)};
        const server::chunk_packet_source source{
            .world = port.get(),
            .tables = &*tables,
            .registries = nullptr,
        };
        recording_gateway gateway;
        gateway.active = {1};
        server::chunk_streamer streamer{gateway, source};
        session::chunk_tracker views;
        static_cast<void>(views.update(1, 0, 0, 0));
        streamer.tick(views);
        expect(latest_column_body(gateway, 1, {0, 0}).has_value());

        const auto gold{runtime->registry().intern("minecraft:gold_block")};
        expect(static_cast<bool>(
            runtime->set_block(sim::block_pos{.x = 0, .y = 0, .z = 0}, gold)));

        // A second player joins after the edit: the cached body belongs to the
        // pre-edit world and must not be reused.
        gateway.active = {1, 2};
        static_cast<void>(views.update(2, 0, 0, 0));
        streamer.tick(views);

        const auto joined{latest_column_body(gateway, 2, {0, 0})};
        expect(joined.has_value());
        if (joined) {
            const auto section{decode_column_section(*joined, wire_section(0))};
            expect(section.has_value());
            if (section) {
                expect(section_state(*section, 0) ==
                       std::optional<std::uint32_t>{2});
            }
        }
    };
};

} // namespace
