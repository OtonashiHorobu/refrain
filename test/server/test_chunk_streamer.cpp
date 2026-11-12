// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_streamer.hpp>

#include <refrain/session/chunk_tracker.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
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

    [[nodiscard]] auto available() const -> bool override { return true; }

    void send_raw(std::uint64_t, std::span<const std::byte>) override {}

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
};

} // namespace
