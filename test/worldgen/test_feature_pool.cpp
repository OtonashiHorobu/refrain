// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Feature pool elements run a placed feature while the owning piece is placed.
// The tests use synthetic feature documents: a deterministic simple block, a
// weighted provider that draws from the piece random, and two malformed
// references that must be diagnosed.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/structure/feature_placement.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

auto parse(std::string_view text) -> data::json::value {
    auto value = data::json::value::parse(text);
    expect(value.has_value()) << (value ? "" : value.error().message);
    return value.has_value() ? std::move(*value) : data::json::value{};
}

auto make_piece(structure::jigsaw_element_info element, const block_pos &origin)
    -> structure_piece_record {
    structure_piece_record piece{};
    piece.type = "minecraft:jigsaw";
    piece.min_x = origin.x;
    piece.max_x = origin.x;
    piece.min_y = origin.y;
    piece.max_y = origin.y;
    piece.min_z = origin.z;
    piece.max_z = origin.z;
    piece.template_x = origin.x;
    piece.template_y = origin.y;
    piece.template_z = origin.z;
    piece.jigsaw = structure::jigsaw_piece_data{.element = std::move(element)};
    return piece;
}

auto feature_element(std::string id) -> structure::jigsaw_element_info {
    structure::jigsaw_element_info element{};
    element.kind = structure::pool_element_kind::feature;
    element.feature = std::move(id);
    return element;
}

struct fixture {
    block_state_registry states{};
    block_state_id air{};
    block_state_id stone{};
    block_state_id dirt{};
    std::unique_ptr<proto_chunk> chunk{};
    std::unique_ptr<chunk_region> region{};
    structure::structure_place_context context{};

    fixture() {
        air = states.air();
        stone =
            states
                .intern("minecraft:stone", {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true})
                .value();
        dirt = states
                   .intern("minecraft:dirt", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true})
                   .value();
        chunk = std::make_unique<proto_chunk>(states, 0, 0, 0, 32);
        chunk->set_status(chunk_status::terrain);
        region = std::make_unique<chunk_region>(*chunk, 1);
        context = structure::structure_place_context{
            .region = region.get(),
            .chunk = chunk.get(),
            .states = &states,
            .state =
                [this](std::string_view key) {
                    return states.find(key).value_or(no_block_state);
                },
            .registry = &states,
            .seed = 1234,
            .sea_level = 8,
        };
    }
};

} // namespace

suite<"worldgen/feature-pool"> feature_pool_tests = [] {
    "a feature element places the resolved placed feature at the piece "
    "position"_test = [] {
        fixture state;
        feature::feature_data_set data;
        data.features.emplace(
            "oracle:marker",
            feature::configured_feature_document{
                .id = "oracle:marker",
                .document = parse(
                    R"({"type":"minecraft:simple_block","to_place":{"id":"minecraft:stone"}})")});
        data.placed.emplace("oracle:marker", feature::placed_feature_document{
                                                 .id = "oracle:marker",
                                                 .feature = "oracle:marker",
                                                 .placement = parse("[]")});

        structure::structure_feature_placer placer{state.context, data};
        const std::array marker_ids{std::string{"oracle:marker"}};
        const auto loaded = placer.load(marker_ids);
        expect(loaded.has_value()) << (loaded ? "" : loaded.error().message);
        if (!loaded) {
            return;
        }
        state.context.place_feature = [&placer](std::string_view id,
                                                worldgen_random &random,
                                                const block_pos &position) {
            return placer.place(id, random, position);
        };
        const auto piece = make_piece(feature_element("oracle:marker"),
                                      block_pos{.x = 5, .y = 10, .z = 5});
        worldgen_random random{
            std::make_unique<xoroshiro_random_source>(424242)};
        structure::place_structure_piece(state.context, piece, random);
        expect(state.chunk->state_at(5, 10, 5) == state.stone);
    };

    "an unknown placed feature is diagnosed by load and place"_test = [] {
        fixture state;
        feature::feature_data_set data;
        data.features.emplace(
            "oracle:marker",
            feature::configured_feature_document{
                .id = "oracle:marker",
                .document = parse(
                    R"({"type":"minecraft:simple_block","to_place":{"id":"minecraft:stone"}})")});
        data.placed.emplace("oracle:marker", feature::placed_feature_document{
                                                 .id = "oracle:marker",
                                                 .feature = "oracle:marker",
                                                 .placement = parse("[]")});
        data.placed.emplace("oracle:broken", feature::placed_feature_document{
                                                 .id = "oracle:broken",
                                                 .feature = "oracle:missing",
                                                 .placement = parse("[]")});

        structure::structure_feature_placer placer{state.context, data};
        const std::array absent_ids{std::string{"oracle:absent"}};
        const auto unknown = placer.load(absent_ids);
        expect(!unknown.has_value());
        if (unknown) {
            return;
        }
        expect(unknown.error().code == error_code::not_found);
        expect(unknown.error().message.find("oracle:absent") !=
               std::string::npos);

        const std::array broken_ids{std::string{"oracle:broken"}};
        const auto broken = placer.load(broken_ids);
        expect(!broken.has_value());
        if (broken) {
            return;
        }
        expect(broken.error().code == error_code::not_found);
        expect(broken.error().message.find("oracle:missing") !=
               std::string::npos);

        worldgen_random random{
            std::make_unique<xoroshiro_random_source>(424242)};
        expect(!placer.place("oracle:absent", random,
                             block_pos{.x = 5, .y = 10, .z = 5}));
        expect(state.chunk->state_at(5, 10, 5) == state.air);
    };

    "feature elements consume the piece random in element order"_test = [] {
        fixture state;
        feature::feature_data_set data;
        data.features.emplace(
            "oracle:weighted",
            feature::configured_feature_document{
                .id = "oracle:weighted",
                .document = parse(
                    R"({"type":"minecraft:simple_block","to_place":{"type":"minecraft:weighted","entries":[{"data":"minecraft:stone","weight":1},{"data":"minecraft:dirt","weight":1}]}})")});
        data.placed.emplace("oracle:weighted", feature::placed_feature_document{
                                                   .id = "oracle:weighted",
                                                   .feature = "oracle:weighted",
                                                   .placement = parse("[]")});

        structure::structure_feature_placer placer{state.context, data};
        const std::array weighted_ids{std::string{"oracle:weighted"}};
        const auto loaded = placer.load(weighted_ids);
        expect(loaded.has_value()) << (loaded ? "" : loaded.error().message);
        if (!loaded) {
            return;
        }
        state.context.place_feature = [&placer](std::string_view id,
                                                worldgen_random &random,
                                                const block_pos &position) {
            return placer.place(id, random, position);
        };

        constexpr jlong seed = 987654321;
        const auto run_case =
            [&](std::vector<structure::jigsaw_element_info> items,
                const block_pos &origin) {
                structure::jigsaw_element_info element{};
                if (items.size() == 1) {
                    element = std::move(items.front());
                } else {
                    element.kind = structure::pool_element_kind::list;
                    element.elements = std::move(items);
                }
                auto random = worldgen_random{
                    std::make_unique<xoroshiro_random_source>(seed)};
                structure::place_structure_piece(
                    state.context, make_piece(std::move(element), origin),
                    random);
                return random;
            };

        auto expected =
            worldgen_random{std::make_unique<xoroshiro_random_source>(seed)};
        const auto first = expected.next_int(2);
        auto one = run_case({feature_element("oracle:weighted")},
                            block_pos{.x = 6, .y = 12, .z = 6});
        expect(state.chunk->state_at(6, 12, 6) ==
               (first == 0 ? state.stone : state.dirt));
        expect(one.next_int(1000) == expected.next_int(1000));

        expected =
            worldgen_random{std::make_unique<xoroshiro_random_source>(seed)};
        const auto first_again = expected.next_int(2);
        const auto second = expected.next_int(2);
        auto two = run_case({feature_element("oracle:weighted"),
                             feature_element("oracle:weighted")},
                            block_pos{.x = 7, .y = 12, .z = 7});
        expect(first_again == first);
        expect(state.chunk->state_at(7, 12, 7) ==
               (second == 0 ? state.stone : state.dirt));
        expect(two.next_int(1000) == expected.next_int(1000));
    };
};
