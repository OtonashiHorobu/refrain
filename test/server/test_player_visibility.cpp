// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/player_visibility.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

using namespace boost::ut;
using namespace refrain::server;

namespace {

[[nodiscard]] auto viewer(std::string_view world, double x, double z,
                          std::int32_t view_distance = 10) -> visibility_query {
    return visibility_query{
        .world = world, .x = x, .z = z, .view_distance = view_distance};
}

constexpr std::array<std::string_view, 3> test_worlds{"overworld", "nether",
                                                      "end"};

[[nodiscard]] auto chunk_of(double value) -> std::int64_t {
    return static_cast<std::int64_t>(std::floor(value / 16.0));
}

/// Deterministic random players over three worlds, with a clustered group and
/// a quarter of the positions sitting on a chunk boundary.
[[nodiscard]] auto random_entities(std::mt19937 &engine)
    -> std::vector<visibility_entity> {
    std::uniform_int_distribution<std::size_t> world_index{
        0, test_worlds.size() - 1};
    std::uniform_int_distribution<std::int32_t> view_distance{0, 12};
    std::uniform_real_distribution<double> position{-1600.0, 1600.0};
    std::uniform_int_distribution<int> nudge{-2, 2};
    std::vector<visibility_entity> entities;
    entities.reserve(240);
    for (std::uint32_t entity_id{1}; entity_id <= 240; ++entity_id) {
        auto x{position(engine)};
        auto z{position(engine)};
        if (entity_id % 4 == 0) {
            x = static_cast<double>(chunk_of(x) * 16 + nudge(engine));
            z = static_cast<double>(chunk_of(z) * 16 + nudge(engine));
        }
        // Keep a cluster in the first world so the candidate sets are not all
        // trivially empty or single-player.
        if (entity_id <= 12) {
            entities.push_back(visibility_entity{
                .entity_id = entity_id,
                .world = test_worlds[0],
                .x = static_cast<double>(entity_id) * 4.0,
                .z = static_cast<double>(entity_id % 3) * 4.0,
                .view_distance = 4,
            });
            continue;
        }
        entities.push_back(visibility_entity{
            .entity_id = entity_id,
            .world = test_worlds[world_index(engine)],
            .x = x,
            .z = z,
            .view_distance = view_distance(engine),
        });
    }
    return entities;
}

/// The viewport square of one viewer, computed by scanning every entity.
[[nodiscard]] auto
brute_force_candidates(std::span<const visibility_entity> entities,
                       std::size_t viewer, std::int32_t tracking_chunks)
    -> std::vector<std::size_t> {
    const auto &source{entities[viewer]};
    const auto chunks{
        effective_tracking_chunks(tracking_chunks, source.view_distance)};
    std::vector<std::size_t> out;
    for (std::size_t position{0}; position < entities.size(); ++position) {
        const auto &other{entities[position]};
        if (other.world != source.world) {
            continue;
        }
        if (std::llabs(chunk_of(other.x) - chunk_of(source.x)) > chunks ||
            std::llabs(chunk_of(other.z) - chunk_of(source.z)) > chunks) {
            continue;
        }
        out.push_back(position);
    }
    return out;
}

} // namespace

suite<"server/player-visibility"> player_visibility_tests = [] {
    "the view distance clamps the tracking range"_test = [] {
        // Ten chunks of view distance is 160 blocks; the vanilla player range
        // is 32 chunks, so the view distance wins.
        expect(player_visible(viewer("world", 0.0, 0.0), "world", 160.0, 0.0,
                              player_tracking_chunks));
        expect(!player_visible(viewer("world", 0.0, 0.0), "world", 161.0, 0.0,
                               player_tracking_chunks));
    };

    "the distance is horizontal only"_test = [] {
        expect(player_visible(viewer("world", 0.0, 0.0), "world", 0.0, 150.0,
                              player_tracking_chunks));
    };

    "a shorter tracking range still applies"_test = [] {
        expect(
            player_visible(viewer("world", 0.0, 0.0), "world", 32.0, 0.0, 2));
        expect(
            !player_visible(viewer("world", 0.0, 0.0), "world", 33.0, 0.0, 2));
    };

    "another world is never visible"_test = [] {
        expect(!player_visible(viewer("world", 0.0, 0.0), "nether", 1.0, 1.0,
                               player_tracking_chunks));
    };

    "the entity chunk must stay inside the viewport"_test = [] {
        // 150 blocks away is inside the distance for a ten chunk view, and the
        // chunk is the tenth one from the viewer.
        expect(player_visible(viewer("world", 8.0, 8.0), "world", 150.0, 8.0,
                              player_tracking_chunks));
    };

    "a chunk boundary does not round the wrong way"_test = [] {
        // 15.9 and 16.0 are adjacent chunks, while 15.9 and 31.9 are one chunk
        // apart only when the viewer is at 15.9.
        expect(
            player_visible(viewer("world", 15.9, 0.0), "world", 16.0, 0.0, 1));
        expect(
            !player_visible(viewer("world", 15.9, 0.0), "world", 32.1, 0.0, 1));
    };

    "the chunk index matches a brute-force viewport scan"_test = [] {
        constexpr std::int32_t tracking_chunks{6};
        std::mt19937 engine{0x5EED5EEDU};
        const auto entities{random_entities(engine)};
        player_visibility_index index{tracking_chunks};
        index.rebuild(entities);

        std::vector<std::size_t> candidates;
        std::size_t pairs{0};
        for (std::size_t viewer_position{0}; viewer_position < entities.size();
             ++viewer_position) {
            const auto &source{entities[viewer_position]};
            const visibility_query query{
                .world = source.world,
                .x = source.x,
                .z = source.z,
                .view_distance = source.view_distance,
            };
            index.candidates(query, candidates);
            expect(std::is_sorted(candidates.begin(), candidates.end()));
            expect(candidates == brute_force_candidates(entities,
                                                        viewer_position,
                                                        tracking_chunks))
                << "viewer position " << viewer_position;
            pairs += candidates.size();
        }
        // The cluster guarantees that the sample exercises non-empty windows.
        expect(pairs > entities.size());
    };

    "the chunk index returns every viewer whose window holds a point"_test =
        [] {
            constexpr std::int32_t tracking_chunks{6};
            std::mt19937 engine{0x5EED5EEDU};
            const auto entities{random_entities(engine)};
            player_visibility_index index{tracking_chunks};
            index.rebuild(entities);

            std::int32_t max_chunks{0};
            for (const auto &entity : entities) {
                max_chunks = std::max(
                    max_chunks, effective_tracking_chunks(
                                    tracking_chunks, entity.view_distance));
            }
            std::vector<std::size_t> viewers;
            for (const auto &probe : entities) {
                index.viewers(probe.world, probe.x, probe.z, viewers);
                expect(std::is_sorted(viewers.begin(), viewers.end()));
                std::vector<std::size_t> expected;
                for (std::size_t position{0}; position < entities.size();
                     ++position) {
                    const auto &candidate{entities[position]};
                    if (candidate.world != probe.world) {
                        continue;
                    }
                    if (std::llabs(chunk_of(candidate.x) - chunk_of(probe.x)) >
                            max_chunks ||
                        std::llabs(chunk_of(candidate.z) - chunk_of(probe.z)) >
                            max_chunks) {
                        continue;
                    }
                    expected.push_back(position);
                }
                expect(viewers == expected) << "probe " << probe.entity_id;
            }
        };

    "another world has no index buckets"_test = [] {
        std::vector<visibility_entity> entities{
            visibility_entity{
                .entity_id = 1,
                .world = "overworld",
                .x = 0.0,
                .z = 0.0,
                .view_distance = 8,
            },
        };
        player_visibility_index index{8};
        index.rebuild(entities);
        std::vector<std::size_t> out{7U};
        index.candidates(
            visibility_query{
                .world = "nether", .x = 0.0, .z = 0.0, .view_distance = 8},
            out);
        expect(out.empty());
        index.viewers("nether", 0.0, 0.0, out);
        expect(out.empty());
    };
};
