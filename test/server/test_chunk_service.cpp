// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Chunk lifecycle at the service seam: the streamer decides whether a column
// must come from the generator, and the runtime world decides whether a column
// survives a save and reload. A resident column is served from the world and is
// never handed to the generator again, including after its viewport is left
// and re-entered. The store half writes a generated column, reloads it and
// compares the content by state hash and block values.

#include <refrain/data/checksum.hpp>
#include <refrain/server/chunk_service.hpp>
#include <refrain/server/chunk_streamer.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/model.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace server = refrain::server;
namespace session = refrain::session;
namespace sim = refrain::sim;
namespace world = refrain::world;
namespace test_support = refrain::test_support;

/// One chunk coordinate as the fake encoder writes it into the body, so a test
/// can read the order the streamer drained without decoding a packet.
struct coordinate {
    std::int32_t x{0};
    std::int32_t z{0};

    friend auto operator==(const coordinate &, const coordinate &)
        -> bool = default;
};

/// Records the chunk batches and forgets the streamer produces.
class recording_gateway final : public server::play_gateway {
  public:
    struct batch {
        std::uint64_t connection{0};
        std::vector<std::vector<std::byte>> packets{};
    };

    struct forgotten_chunk {
        std::uint64_t connection{0};
        std::int32_t chunk_x{0};
        std::int32_t chunk_z{0};
    };

    [[nodiscard]] auto available() const -> bool override { return true; }

    void send_raw(std::uint64_t, std::span<const std::byte>) override {}

    [[nodiscard]] auto active_connections() const
        -> std::vector<std::uint64_t> override {
        return active;
    }

    [[nodiscard]] auto backlog(std::uint64_t connection) const
        -> std::size_t override {
        const auto found{backlogs.find(connection)};
        return found == backlogs.end() ? 0U : found->second;
    }

    /// Bytes the gateway reports queued per connection. A test raises one to
    /// make the streamer starve that client.
    std::unordered_map<std::uint64_t, std::size_t> backlogs{};

    void send_chunk_batch(std::uint64_t connection, std::int32_t, std::int32_t,
                          std::span<const std::vector<std::byte>> packets,
                          bool) override {
        batches.push_back(batch{
            .connection = connection,
            .packets = std::vector<std::vector<std::byte>>{packets.begin(),
                                                           packets.end()},
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
    std::vector<batch> batches{};
    std::vector<forgotten_chunk> forgotten{};
};

/// Shared counters between a test and the encoder it installs.
struct encode_log {
    std::size_t calls{0};
};

/// A minimal valid column: one stone section at y=0 with a plains biome.
[[nodiscard]] auto make_column() -> sim::storage_chunk {
    sim::storage_chunk column{};
    column.format_version = world::chunk_format_version;
    column.status = world::chunk_status::full;
    column.y_pos = 0;
    world::section_record section{};
    section.y = 0;
    section.kind = world::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world::section_volume);
    section.biome_kind = world::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(world::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world::section_volume),
        .state = 0,
    });
    column.sections.push_back(std::move(section));
    return column;
}

/// The streamed coordinates of one connection, in wire order.
[[nodiscard]] auto streamed(const recording_gateway &gateway,
                            std::uint64_t connection)
    -> std::vector<coordinate> {
    std::vector<coordinate> out{};
    for (const auto &batch : gateway.batches) {
        if (batch.connection != connection) {
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

/// A column source backed by a runtime world: a column the world holds is
/// encoded, an absent one fails with `not_found` exactly like the live source.
[[nodiscard]] auto make_encoder(sim::world &runtime,
                                std::shared_ptr<encode_log> log)
    -> server::chunk_streamer::chunk_encoder {
    return [&runtime, log](
               const server::chunk_packet_source &,
               sim::chunk_pos pos) -> refrain::result<std::vector<std::byte>> {
        ++log->calls;
        if (runtime.find_chunk(pos) == nullptr) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "column is not resident")};
        }
        const coordinate value{pos.x, pos.z};
        std::vector<std::byte> body(sizeof(value));
        std::memcpy(body.data(), &value, sizeof(value));
        return body;
    };
}

/// Rows of one viewport in the order the streamer queues them: ring by ring,
/// then by z and x.
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

suite<"server/chunk-service"> chunk_service_tests = [] {
    "server/chunk stream serves a resident column without generating it"_test =
        [] {
            test_support::scratch_directory scratch;
            const auto path{(scratch.path() / "world").string()};
            expect(static_cast<bool>(world::create_world(path, 7, "probe")));
            auto opened{sim::world::open(path)};
            expect(static_cast<bool>(opened));
            if (!opened) {
                return;
            }
            auto &runtime{*opened};
            for (std::int32_t z{-1}; z <= 1; ++z) {
                for (std::int32_t x{-1}; x <= 1; ++x) {
                    auto inserted{runtime.insert_chunk(sim::chunk_pos{x, z},
                                                       make_column())};
                    expect(static_cast<bool>(inserted))
                        << (inserted ? std::string{}
                                     : inserted.error().message);
                }
            }

            recording_gateway gateway;
            gateway.active = {1};
            auto log{std::make_shared<encode_log>()};
            server::chunk_streamer streamer{gateway,
                                            server::chunk_packet_source{}};
            streamer.set_encoder(make_encoder(runtime, log));
            std::vector<coordinate> requested{};
            streamer.set_chunk_requester(
                [&requested](std::int32_t x, std::int32_t z) {
                    requested.push_back({x, z});
                    return true;
                });
            session::chunk_tracker views;
            static_cast<void>(views.update(1, 0, 0, 1));

            const auto expected{viewport_order(1)};
            for (std::size_t guard{0};
                 streamed(gateway, 1).size() < expected.size() && guard < 8;
                 ++guard) {
                streamer.tick(views);
            }
            expect(streamed(gateway, 1) == expected);
            expect(requested.empty());

            // Walk one viewport away and back. The destination columns are
            // absent and may be generated; the origin columns are resident and
            // must be served again without asking the generator.
            static_cast<void>(views.update(1, 8, 8, 1));
            streamer.tick(views);
            const auto sent_after_leaving{streamed(gateway, 1).size()};
            static_cast<void>(views.update(1, 0, 0, 1));
            for (std::size_t guard{0};
                 streamed(gateway, 1).size() <
                     sent_after_leaving + expected.size() &&
                 guard < 8;
                 ++guard) {
                streamer.tick(views);
            }
            expect(streamed(gateway, 1).size() ==
                   sent_after_leaving + expected.size());
            // The origin bodies were still in the encoded cache, so the return
            // trip is served without a single new encode.
            expect(streamer.cache_hits() == expected.size());
            expect(streamer.cache_misses() == expected.size());
            for (const auto &position : requested) {
                const auto in_origin_view{
                    std::max(std::abs(position.x), std::abs(position.z)) <= 1};
                expect(!in_origin_view);
            }
        };

    "server/chunk stream asks for a missing column once while it is in flight"_test =
        [] {
            test_support::scratch_directory scratch;
            const auto path{(scratch.path() / "world").string()};
            expect(static_cast<bool>(world::create_world(path, 7, "probe")));
            auto opened{sim::world::open(path)};
            expect(static_cast<bool>(opened));
            if (!opened) {
                return;
            }
            auto &runtime{*opened};

            recording_gateway gateway;
            gateway.active = {2};
            auto log{std::make_shared<encode_log>()};
            server::chunk_streamer streamer{gateway,
                                            server::chunk_packet_source{}};
            streamer.set_encoder(make_encoder(runtime, log));
            std::vector<coordinate> requested{};
            streamer.set_chunk_requester(
                [&requested](std::int32_t x, std::int32_t z) {
                    requested.push_back({x, z});
                    return true;
                });
            session::chunk_tracker views;
            static_cast<void>(views.update(2, 20, 20, 0));

            // The column is not resident and the requester never answers: the
            // streamer must not encode it again on every tick.
            for (int tick{0}; tick < 5; ++tick) {
                streamer.tick(views);
            }
            expect(requested == std::vector<coordinate>{{20, 20}});
            expect(log->calls == 1U);
            // A failed lookup for a column that is still generating is not a
            // cache miss; the cache never held a body the world could give.
            expect(streamer.cache_misses() == 0U);
            expect(streamed(gateway, 2).empty());

            // The generator answered and the world adopted the column.
            auto inserted{
                runtime.insert_chunk(sim::chunk_pos{20, 20}, make_column())};
            expect(static_cast<bool>(inserted));
            streamer.chunk_ready(20, 20);
            streamer.tick(views);
            expect(streamed(gateway, 2) == std::vector<coordinate>{{20, 20}});
            expect(requested.size() == 1U);
        };

    "server/chunk stream clears the in-flight marker when a column arrives off screen"_test =
        [] {
            test_support::scratch_directory scratch;
            const auto path{(scratch.path() / "world").string()};
            expect(static_cast<bool>(world::create_world(path, 7, "probe")));
            auto opened{sim::world::open(path)};
            expect(static_cast<bool>(opened));
            if (!opened) {
                return;
            }
            auto &runtime{*opened};

            recording_gateway gateway;
            gateway.active = {3};
            auto log{std::make_shared<encode_log>()};
            server::chunk_streamer streamer{gateway,
                                            server::chunk_packet_source{}};
            streamer.set_encoder(make_encoder(runtime, log));
            std::vector<coordinate> requested{};
            streamer.set_chunk_requester(
                [&requested](std::int32_t x, std::int32_t z) {
                    requested.push_back({x, z});
                    return true;
                });
            session::chunk_tracker views;
            static_cast<void>(views.update(3, 0, 0, 0));
            streamer.tick(views);
            expect(requested == std::vector<coordinate>{{0, 0}});

            // The player is two viewports away when the column arrives.
            static_cast<void>(views.update(3, 40, 40, 0));
            streamer.tick(views);
            auto inserted{
                runtime.insert_chunk(sim::chunk_pos{0, 0}, make_column())};
            expect(static_cast<bool>(inserted));
            streamer.chunk_ready(0, 0);

            static_cast<void>(views.update(3, 0, 0, 0));
            streamer.tick(views);
            expect(streamed(gateway, 3) == std::vector<coordinate>{{0, 0}});
            expect(std::count(requested.begin(), requested.end(),
                              coordinate{0, 0}) == 1);
        };

    "server/chunk store reload returns the same content"_test = [] {
        test_support::scratch_directory scratch;
        const auto path{(scratch.path() / "world").string()};
        expect(static_cast<bool>(world::create_world(path, 7, "probe")));

        refrain::data::digest32 saved{};
        {
            auto opened{sim::world::open(path)};
            expect(static_cast<bool>(opened));
            if (!opened) {
                return;
            }
            auto inserted{
                opened->insert_chunk(sim::chunk_pos{0, 0}, make_column())};
            expect(static_cast<bool>(inserted));
            const auto dirt{opened->registry().intern("minecraft:dirt")};
            expect(static_cast<bool>(opened->set_block(
                sim::block_pos{0, static_cast<std::int16_t>(0), 0}, dirt)));
            expect(static_cast<bool>(opened->set_block(
                sim::block_pos{3, static_cast<std::int16_t>(5), 4}, dirt)));
            saved = opened->state_hash();
            expect(static_cast<bool>(opened->flush()));
        }

        auto reopened{sim::world::open(path)};
        expect(static_cast<bool>(reopened));
        if (!reopened) {
            return;
        }
        auto loaded{reopened->ensure_chunk(sim::chunk_pos{0, 0})};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{} : loaded.error().message);
        expect(loaded && *loaded != nullptr);
        expect(reopened->state_hash() == saved);
        const auto dirt{reopened->registry().find("minecraft:dirt")};
        const auto stone{reopened->registry().find("minecraft:stone")};
        expect(dirt.has_value());
        expect(stone.has_value());
        if (dirt && stone) {
            expect(reopened->block_at(sim::block_pos{
                       0, static_cast<std::int16_t>(0), 0}) == *dirt);
            expect(reopened->block_at(sim::block_pos{
                       3, static_cast<std::int16_t>(5), 4}) == *dirt);
            expect(reopened->block_at(sim::block_pos{
                       5, static_cast<std::int16_t>(5), 5}) == *stone);
        }

        auto absent{reopened->ensure_chunk(sim::chunk_pos{3, 3})};
        expect(!absent);
        if (!absent) {
            expect(absent.error().code == refrain::error_code::not_found);
        }
    };

    "server/chunk stream starves a client over its backlog limit"_test = [] {
        recording_gateway gateway;
        gateway.active = {1, 2};
        // Connection 1 is over the per-connection limit and connection 2 is
        // not, so one viewport must pause while the other drains.
        gateway.backlogs[1] = 8U;
        server::chunk_streamer streamer{gateway, server::chunk_packet_source{},
                                        server::chunk_streamer::backlog_limits{
                                            .max_connection_bytes = 4U,
                                            .max_total_bytes = 1'000U,
                                        }};
        streamer.set_encoder(
            [](const server::chunk_packet_source &,
               sim::chunk_pos pos) -> refrain::result<std::vector<std::byte>> {
                const coordinate value{pos.x, pos.z};
                std::vector<std::byte> body(sizeof(value));
                std::memcpy(body.data(), &value, sizeof(value));
                return body;
            });
        session::chunk_tracker views;
        static_cast<void>(views.update(1, 0, 0, 1));
        static_cast<void>(views.update(2, 0, 0, 1));
        const auto expected{viewport_order(1)};
        for (std::size_t guard{0};
             streamed(gateway, 2).size() < expected.size() && guard < 8;
             ++guard) {
            streamer.tick(views);
        }
        expect(streamed(gateway, 2) == expected);
        // The starved connection keeps its pending viewport instead of
        // dropping it, and receives nothing while it stays behind.
        expect(streamed(gateway, 1).empty());
        // Draining the queue lets it catch up with the same columns, not a
        // regenerated set.
        gateway.backlogs[1] = 0;
        for (std::size_t guard{0};
             streamed(gateway, 1).size() < expected.size() && guard < 8;
             ++guard) {
            streamer.tick(views);
        }
        expect(streamed(gateway, 1) == expected);
    };

    "server/chunk stream limits clamp to one byte"_test = [] {
        const auto limits{
            server::chunk_streamer::backlog_limits::from_config(0, -1)};
        expect(limits.max_connection_bytes == 1U);
        expect(limits.max_total_bytes == 1U);
        const auto clamped{server::chunk_streamer::backlog_limits{
            .max_connection_bytes = 0U, .max_total_bytes = 0U}
                               .clamped()};
        expect(clamped.max_connection_bytes == 1U);
        expect(clamped.max_total_bytes == 1U);
    };
};

} // namespace
