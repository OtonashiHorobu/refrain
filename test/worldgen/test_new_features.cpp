// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Synthetic coverage of the configured feature, placement and predicate types
// that the standard biomes name: one fixture per family, no game data tables.

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/feature/feature.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/feature/tree.hpp>
#include <refrain/worldgen/provider.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::feature;

auto parse(std::string_view text) -> data::json::value {
    auto parsed = data::json::value::parse(text);
    expect(parsed.has_value()) << parsed.error().message;
    return parsed ? std::move(*parsed) : data::json::value{};
}

auto make_random(std::uint64_t seed) -> worldgen_random {
    return worldgen_random{std::make_unique<xoroshiro_random_source>(seed)};
}

/// A small generation fixture: a 16x16 chunk with a configurable height and a
/// feature context that resolves and interns arbitrary block state keys.
struct fixture {
    block_state_registry registry{};
    proto_chunk chunk;
    std::unique_ptr<chunk_region> region;
    feature_context context;

    explicit fixture(jint height = 32, jint write_radius = 2)
        : chunk{registry, 0, 0, 0, height} {
        chunk.set_status(chunk_status::features);
        region = std::make_unique<chunk_region>(chunk, write_radius);
        context.placement = placement_context{
            .region = region.get(),
            .chunk = &chunk,
            .states = &registry,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = height,
                                                       .sea_level = 8},
        };
        context.placement.resolve_state =
            [this](std::string_view key) -> block_state_id {
            return resolve(key);
        };
        context.placement.height = [this](heightmap_kind kind, jint x,
                                          jint z) -> jint {
            return region->height_at(kind, x, z);
        };
    }

    auto resolve(std::string_view key) -> block_state_id {
        if (const auto found = registry.find(key); found.has_value()) {
            return *found;
        }
        const auto interned = registry.intern(key, registry.infer_traits(key));
        return interned ? *interned : no_block_state;
    }

    void fill_layer(jint y, block_state_id state) {
        for (jint x = 0; x < 16; ++x) {
            for (jint z = 0; z < 16; ++z) {
                (void)chunk.set_state(x, y, z, state);
            }
        }
    }

    [[nodiscard]] auto name_at(jint x, jint y, jint z) const
        -> std::string_view {
        return registry.name(region->state_at(x, y, z));
    }

    [[nodiscard]] auto count_blocks(std::string_view block) const -> jint {
        jint count = 0;
        for (jint y = 0; y < chunk.height(); ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (state_block_name(name_at(x, y, z)) == block) {
                        ++count;
                    }
                }
            }
        }
        return count;
    }
};

/// Builds a configured feature plus an empty placed wrapper in a registry, so
/// the tests exercise the same decode path the data set uses.
struct decoded_feature {
    feature_registry registry;
    std::string placed_id{"oracle:test"};

    explicit decoded_feature(block_state_registry &states) : registry{states} {}

    auto load(std::string_view document) -> bool {
        feature_data_set data;
        configured_feature_document configured;
        configured.id = "oracle:test_feature";
        configured.document = parse(document);
        data.features.emplace(configured.id, std::move(configured));
        placed_feature_document placed;
        placed.id = placed_id;
        placed.feature = "oracle:test_feature";
        placed.placement = parse("[]");
        data.placed.emplace(placed.id, std::move(placed));
        const auto loaded = registry.load(data);
        expect(loaded.has_value()) << loaded.error().message;
        return loaded.has_value();
    }

    auto place(feature_context &context, worldgen_random &random,
               const block_pos &origin) -> bool {
        const auto found = registry.placed().find(placed_id);
        expect(found != registry.placed().end());
        if (found == registry.placed().end()) {
            return false;
        }
        return place_placed(context, found->second, random, origin);
    }
};

} // namespace

suite<"worldgen/feature-new"> new_feature_tests = [] {
    "no_op performs no write and succeeds"_test = [] {
        fixture environment;
        decoded_feature feature{environment.registry};
        if (!feature.load(R"({"type":"minecraft:no_op"})")) {
            return;
        }
        auto random = make_random(1);
        const jint before = environment.count_blocks("minecraft:air");
        expect(feature.place(environment.context, random,
                             block_pos{.x = 3, .y = 4, .z = 5}));
        expect(environment.count_blocks("minecraft:air") == before);
    };

    "fill_layer writes only air at the layer height"_test = [] {
        fixture environment;
        const auto stone = environment.resolve("minecraft:stone");
        (void)environment.chunk.set_state(1, 5, 1, stone);
        decoded_feature feature{environment.registry};
        if (!feature.load(R"({"type":"minecraft:fill_layer","height":5,
                              "state":"minecraft:dirt"})")) {
            return;
        }
        auto random = make_random(2);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 0, .y = 0, .z = 0}));
        expect(environment.name_at(0, 5, 0) == "minecraft:dirt");
        expect(environment.name_at(15, 5, 15) == "minecraft:dirt");
        expect(environment.name_at(1, 5, 1) == "minecraft:stone");
        expect(environment.name_at(0, 6, 0) == "minecraft:air");
    };

    "single_block_pillar climbs the predicate and runs the cap"_test = [] {
        fixture environment;
        const auto stone = environment.resolve("minecraft:stone");
        environment.fill_layer(5, stone);
        (void)environment.chunk.set_state(2, 6, 2, stone);
        decoded_feature feature{environment.registry};
        if (!feature.load(R"({"type":"minecraft:single_block_pillar",
                              "block":"minecraft:dirt",
                              "can_replace":{"type":"minecraft:matching_blocks",
                                             "blocks":"minecraft:stone"},
                              "cap_feature":{"feature":{"type":"minecraft:simple_block",
                                                        "to_place":{"id":"minecraft:air"}},
                                             "placement":[]},
                              "direction":"up"})")) {
            return;
        }
        auto random = make_random(3);
        std::vector<block_pos> caps;
        environment.context.place_by_id =
            [&](std::string_view, worldgen_random &, const block_pos &pos) {
                caps.push_back(pos);
                return true;
            };
        expect(feature.place(environment.context, random,
                             block_pos{.x = 2, .y = 5, .z = 2}));
        expect(environment.name_at(2, 5, 2) == "minecraft:dirt");
        expect(environment.name_at(2, 6, 2) == "minecraft:dirt");
        expect(environment.name_at(2, 7, 2) == "minecraft:air");
        expect(caps.size() == 1U);
        if (!caps.empty()) {
            expect(caps.front().y == 6);
        }
    };

    "speleothem grows a pointed column from a base block"_test = [] {
        fixture environment;
        const auto dripstone = environment.resolve("minecraft:dripstone_block");
        environment.fill_layer(8, dripstone);
        // A non-base block above forces the column upwards; the base block
        // below is the substrate the column grows from.
        (void)environment.chunk.set_state(
            6, 10, 6, environment.resolve("minecraft:stone"));
        const auto replaceable = environment.resolve("minecraft:air");
        const std::array<block_state_id, 1> replaceable_states{replaceable};
        environment.registry.add_tag("oracle:speleothem_replaceable",
                                     replaceable_states);
        decoded_feature feature{environment.registry};
        if (!feature.load(R"({"type":"minecraft:speleothem",
                              "base_block":"minecraft:dripstone_block",
                              "pointed_block":"minecraft:pointed_dripstone[thickness=tip,tip_direction=up,waterlogged=false]",
                              "replaceable_blocks":"#oracle:speleothem_replaceable",
                              "chance_of_taller_generation":0.0,
                              "chance_of_directional_spread":0.0,
                              "chance_of_spread_radius2":0.0,
                              "chance_of_spread_radius3":0.0})")) {
            return;
        }
        auto random = make_random(4);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 6, .y = 9, .z = 6}));
        const auto pointed = std::string{environment.name_at(6, 9, 6)};
        expect(pointed.starts_with("minecraft:pointed_dripstone[") ||
               pointed == "minecraft:pointed_dripstone")
            << pointed;
        expect(pointed.find("thickness=tip") != std::string::npos) << pointed;
        expect(pointed.find("tip_direction=up") != std::string::npos)
            << pointed;
    };

    "coral tree and claw repeat a nested placed feature"_test = [] {
        fixture environment;
        decoded_feature tree{environment.registry};
        if (!tree.load(R"({"type":"minecraft:coral_tree",
                           "feature":{"feature":{"type":"minecraft:simple_block",
                                                 "to_place":{"id":"minecraft:air"}},
                                      "placement":[]}})")) {
            return;
        }
        auto random = make_random(5);
        std::vector<block_pos> positions;
        environment.context.place_by_id =
            [&](std::string_view, worldgen_random &, const block_pos &pos) {
                positions.push_back(pos);
                return true;
            };
        expect(tree.place(environment.context, random,
                          block_pos{.x = 5, .y = 5, .z = 5}));
        expect(positions.size() >= 3U);
        if (!positions.empty()) {
            expect(positions.front().x == 5 && positions.front().z == 5);
            expect(positions.front().y == 5);
        }
        jint trunk_height = 0;
        for (const auto &pos : positions) {
            if (pos.x == 5 && pos.z == 5) {
                ++trunk_height;
            }
        }
        expect(trunk_height >= 1 && trunk_height <= 3) << trunk_height;

        decoded_feature claw{environment.registry};
        if (!claw.load(R"({"type":"minecraft:coral_claw",
                           "feature":{"feature":{"type":"minecraft:simple_block",
                                                 "to_place":{"id":"minecraft:air"}},
                                      "placement":[]}})")) {
            return;
        }
        positions.clear();
        auto random2 = make_random(6);
        expect(claw.place(environment.context, random2,
                          block_pos{.x = 8, .y = 5, .z = 8}));
        expect(positions.size() >= 2U);
        if (!positions.empty()) {
            expect(positions.front().x == 8 && positions.front().y == 5 &&
                   positions.front().z == 8);
        }
    };

    "projected square drops a state through the projection predicate"_test =
        [] {
            fixture environment;
            const auto stone = environment.resolve("minecraft:stone");
            environment.fill_layer(4, stone);
            decoded_feature feature{environment.registry};
            if (!feature.load(
                    R"({"type":"minecraft:projected_random_patchy_square",
                                  "block":"minecraft:dirt",
                                  "project_through":{"type":"minecraft:matching_blocks",
                                                     "blocks":"minecraft:air"},
                                  "size":1,
                                  "max_projection_height":20})")) {
                return;
            }
            auto random = make_random(7);
            expect(feature.place(environment.context, random,
                                 block_pos{.x = 8, .y = 10, .z = 8}));
            expect(environment.count_blocks("minecraft:dirt") > 0);
            for (jint y = 5; y < 10; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        if (environment.name_at(x, y, z) == "minecraft:dirt") {
                            expect(y == 5);
                        }
                    }
                }
            }
        };

    "stepped column cluster grows columns above the surface"_test = [] {
        fixture environment;
        const auto stone = environment.resolve("minecraft:stone");
        environment.fill_layer(4, stone);
        decoded_feature feature{environment.registry};
        if (!feature.load(R"({"type":"minecraft:stepped_column_cluster",
                              "block":"minecraft:basalt",
                              "continue_through":{"type":"minecraft:matching_blocks",
                                                  "blocks":"minecraft:air"},
                              "can_replace":{"type":"minecraft:matching_blocks",
                                             "blocks":"minecraft:air"},
                              "cannot_place_on":["minecraft:bedrock"],
                              "cluster_reach":2,
                              "column_count":8,
                              "column_reach":1,
                              "height":3})")) {
            return;
        }
        auto random = make_random(8);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 8, .y = 5, .z = 8}));
        expect(environment.count_blocks("minecraft:basalt") > 0);
    };
};

suite<"worldgen/state-provider-noise"> state_provider_noise_tests = [] {
    "noise providers choose a state without consuming randomness"_test = [] {
        block_state_registry registry;
        auto decoded =
            decode_state_provider(parse(R"({"type":"minecraft:noise","seed":7,
                      "noise":{"base_amplitude":1.0,"base_octave":0},
                      "scale":0.5,
                      "states":["minecraft:stone","minecraft:dirt"]})"),
                                  registry);
        expect(decoded.has_value()) << decoded.error().message;
        if (!decoded) {
            return;
        }
        const placement_context context{};
        auto first = make_random(11);
        auto second = make_random(12);
        const auto at = block_pos{.x = 3, .y = 4, .z = 5};
        const auto a = (*decoded)->get(context, first, at);
        const auto b = (*decoded)->get(context, second, at);
        expect(a == b);
        expect(registry.name(a) == registry.name(b));
    };

    "dual noise providers expand the candidate range with the slow noise"_test =
        [] {
            block_state_registry registry;
            auto decoded = decode_state_provider(
                parse(R"({"type":"minecraft:dual_noise","seed":2,
                          "variety":[1,3],
                          "noise":{"base_amplitude":1.0,"base_octave":0},
                          "slow_noise":{"base_amplitude":1.0,"base_octave":-2},
                          "scale":1.0,"slow_scale":1.0,
                          "states":["minecraft:stone","minecraft:dirt",
                                    "minecraft:grass_block"]})"),
                registry);
            expect(decoded.has_value()) << decoded.error().message;
            if (!decoded) {
                return;
            }
            const placement_context context{};
            auto random = make_random(13);
            const auto state = (*decoded)->get(
                context, random, block_pos{.x = 1, .y = 2, .z = 3});
            expect(state != no_block_state);
            const auto name = registry.name(state);
            expect(name == "minecraft:stone" || name == "minecraft:dirt" ||
                   name == "minecraft:grass_block")
                << name;
        };

    "random block providers draw uniformly from their list"_test = [] {
        block_state_registry registry;
        const auto stone = registry.intern("minecraft:stone", {.air = false});
        const auto dirt = registry.intern("minecraft:dirt", {.air = false});
        expect(stone.has_value() && dirt.has_value());
        if (!stone || !dirt) {
            return;
        }
        const std::array<block_state_id, 2> blocks{*stone, *dirt};
        registry.add_tag("oracle:random_blocks", blocks);
        auto decoded =
            decode_state_provider(parse(R"({"type":"minecraft:random_block",
                      "blocks":"#oracle:random_blocks"})"),
                                  registry);
        expect(decoded.has_value()) << decoded.error().message;
        if (!decoded) {
            return;
        }
        auto expected_random = make_random(21);
        const auto expected = expected_random.next_int(2) == 0 ? *stone : *dirt;
        auto random = make_random(21);
        const placement_context context{};
        expect((*decoded)->get(context, random,
                               block_pos{.x = 0, .y = 0, .z = 0}) == expected);
    };

    "random block providers reject empty tags"_test = [] {
        block_state_registry registry;
        expect(!decode_state_provider(parse(R"({"type":"minecraft:random_block",
                              "blocks":"#oracle:missing"})"),
                                      registry)
                    .has_value());
    };
};

namespace {

auto tree_document(std::string_view trunk, std::string_view foliage,
                   std::string_view wood) -> std::string {
    std::string document;
    document += "{\"type\":\"minecraft:tree\",";
    document += "\"below_trunk_provider\":{\"type\":\"minecraft:rule_based\","
                "\"rules\":[]},";
    document += "\"trunk_provider\":{\"id\":\"";
    document += wood;
    document += "\",\"properties\":{\"axis\":\"y\"}},";
    document += "\"foliage_provider\":{\"id\":\"minecraft:oak_leaves\"},";
    document += "\"trunk_placer\":";
    document += trunk;
    document += ",\"foliage_placer\":";
    document += foliage;
    document +=
        ",\"minimum_size\":{\"type\":\"minecraft:two_layers_feature_size\","
        "\"limit\":1,\"lower_size\":0,\"upper_size\":1},";
    document += "\"decorators\":[],\"ignore_vines\":true}";
    return document;
}

} // namespace

suite<"worldgen/tree-new"> tree_new_tests = [] {
    "the cherry trunk and foliage placers place logs and leaves"_test = [] {
        fixture environment;
        decoded_feature feature{environment.registry};
        const auto document = tree_document(
            R"({"type":"minecraft:cherry_trunk_placer","base_height":4,
                "height_rand_a":0,"height_rand_b":0,
                "branch_count":1,"branch_horizontal_length":2,
                "branch_start_offset_from_top":{"min_inclusive":-2,
                                                "max_inclusive":-1},
                "branch_end_offset_from_top":0})",
            R"({"type":"minecraft:cherry_foliage_placer","radius":2,"offset":0,
                "height":2,"wide_bottom_layer_hole_chance":0.0,
                "corner_hole_chance":0.0,"hanging_leaves_chance":0.0,
                "hanging_leaves_extension_chance":0.0})",
            "minecraft:cherry_log");
        if (!feature.load(document)) {
            return;
        }
        auto random = make_random(31);
        const bool placed = feature.place(environment.context, random,
                                          block_pos{.x = 8, .y = 5, .z = 8});
        expect(placed);
        expect(environment.count_blocks("minecraft:cherry_log") > 0);
        expect(environment.registry.name(environment.registry.air()) ==
               "minecraft:air");
        jint leaves = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    if (environment.name_at(x, y, z).starts_with(
                            "minecraft:oak_leaves")) {
                        ++leaves;
                    }
                }
            }
        }
        expect(leaves > 0) << leaves;
    };

    "the poplar trunk and foliage placers place logs and leaves"_test = [] {
        fixture environment;
        decoded_feature feature{environment.registry};
        const auto document = tree_document(
            R"({"type":"minecraft:poplar_trunk_placer","base_height":5,
                "height_rand_a":0,"height_rand_b":0,
                "trunk_height_above_branches":2,"branch_amount":1})",
            R"({"type":"minecraft:poplar_foliage_placer","radius":2,"offset":0,
                "height":3,"side_hole_chance":0.0})",
            "minecraft:poplar_log");
        if (!feature.load(document)) {
            return;
        }
        auto random = make_random(32);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 8, .y = 5, .z = 8}));
        expect(environment.count_blocks("minecraft:poplar_log") > 0);
        jint leaves = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    if (environment.name_at(x, y, z).starts_with(
                            "minecraft:oak_leaves")) {
                        ++leaves;
                    }
                }
            }
        }
        expect(leaves > 0) << leaves;
    };

    "the bending trunk placer bends away from the origin"_test = [] {
        fixture environment;
        decoded_feature feature{environment.registry};
        const auto document = tree_document(
            R"({"type":"minecraft:bending_trunk_placer","base_height":4,
                "height_rand_a":0,"height_rand_b":0,
                "min_height_for_leaves":2,"bend_length":1})",
            R"({"type":"minecraft:random_spread_foliage_placer","radius":2,
                "offset":0,"foliage_height":2,
                "leaf_placement_attempts":8})",
            "minecraft:oak_log");
        if (!feature.load(document)) {
            return;
        }
        auto random = make_random(33);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 8, .y = 5, .z = 8}));
        expect(environment.count_blocks("minecraft:oak_log") > 0);
        jint leaves = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    if (environment.name_at(x, y, z).starts_with(
                            "minecraft:oak_leaves")) {
                        ++leaves;
                    }
                }
            }
        }
        expect(leaves > 0) << leaves;
    };

    "a pine foliage placer grows a canopy from its height provider"_test = [] {
        fixture environment;
        decoded_feature feature{environment.registry};
        const auto document = tree_document(
            R"({"type":"minecraft:straight_trunk_placer","base_height":6,
                "height_rand_a":0,"height_rand_b":0})",
            R"({"type":"minecraft:pine_foliage_placer","radius":1,"offset":1,
                "height":3})",
            "minecraft:spruce_log");
        if (!feature.load(document)) {
            return;
        }
        const auto stone = environment.resolve("minecraft:stone");
        environment.fill_layer(4, stone);
        auto random = make_random(35);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 8, .y = 5, .z = 8}));
        expect(environment.count_blocks("minecraft:spruce_log") > 0);
        jint leaves = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    if (environment.name_at(x, y, z).starts_with(
                            "minecraft:oak_leaves")) {
                        ++leaves;
                    }
                }
            }
        }
        expect(leaves > 0);
    };

    "a mangrove root placer writes roots below the trunk origin"_test = [] {
        fixture environment;
        decoded_feature feature{environment.registry};
        std::string document = R"({"type":"minecraft:tree",
            "below_trunk_provider":{"type":"minecraft:rule_based","rules":[]},
            "trunk_provider":{"id":"minecraft:mangrove_log",
                              "properties":{"axis":"y"}},
            "foliage_provider":{"id":"minecraft:mangrove_leaves"},
            "root_placer":{"type":"minecraft:mangrove_root_placer",
              "trunk_offset_y":1,
              "root_provider":{"id":"minecraft:mangrove_roots"},
              "mangrove_root_placement":{
                 "can_grow_through":"#oracle:mangrove_can_grow",
                 "muddy_roots_in":["minecraft:mud"],
                 "muddy_roots_provider":{"id":"minecraft:muddy_mangrove_roots"},
                 "max_root_width":4,"max_root_length":4,
                 "random_skew_chance":0.0}},
            "trunk_placer":{"type":"minecraft:upwards_branching_trunk_placer",
              "base_height":3,"height_rand_a":0,"height_rand_b":0,
              "extra_branch_steps":1,"extra_branch_length":0,
              "place_branch_per_log_probability":0.0,
              "can_grow_through":"#oracle:mangrove_can_grow"},
            "foliage_placer":{"type":"minecraft:random_spread_foliage_placer",
              "radius":2,"offset":0,"foliage_height":2,
              "leaf_placement_attempts":8},
            "minimum_size":{"type":"minecraft:two_layers_feature_size",
                            "limit":1,"lower_size":0,"upper_size":1},
            "decorators":[],"ignore_vines":true})";
        const auto stone = environment.resolve("minecraft:stone");
        environment.fill_layer(4, stone);
        const auto air = environment.resolve("minecraft:air");
        const std::array<block_state_id, 1> grow{air};
        environment.registry.add_tag("oracle:mangrove_can_grow", grow);
        if (!feature.load(document)) {
            return;
        }
        auto random = make_random(34);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 8, .y = 5, .z = 8}));
        expect(environment.count_blocks("minecraft:mangrove_log") > 0);
        expect(environment.count_blocks("minecraft:mangrove_roots") > 0);
    };

    "the creaking heart decorator replaces a log surrounded by logs"_test = [] {
        fixture environment;
        const auto log = environment.resolve("minecraft:oak_log[axis=y]");
        const std::array<block_state_id, 1> logs_tag{log};
        environment.registry.add_tag("minecraft:logs", logs_tag);
        const block_pos center{.x = 8, .y = 10, .z = 8};
        for (jint dy = -1; dy <= 1; ++dy) {
            for (jint dz = -1; dz <= 1; ++dz) {
                for (jint dx = -1; dx <= 1; ++dx) {
                    (void)environment.chunk.set_state(
                        center.x + dx, center.y + dy, center.z + dz, log);
                }
            }
        }
        auto random = make_random(35);
        const std::vector<block_pos> logs{center};
        const std::vector<block_pos> leaves;
        tree_decorator_context context{.context = environment.context,
                                       .states = environment.registry,
                                       .random = random,
                                       .logs = logs,
                                       .leaves = leaves};
        const creaking_heart_decorator decorator{1.0F};
        decorator.place(context);
        expect(environment.name_at(center.x, center.y, center.z)
                   .starts_with("minecraft:creaking_heart"))
            << environment.name_at(center.x, center.y, center.z);
    };

    "the pale moss decorator hangs moss below logs and leaves"_test = [] {
        fixture environment;
        const auto log = environment.resolve("minecraft:oak_log[axis=y]");
        const block_pos log_pos{.x = 5, .y = 10, .z = 5};
        const block_pos leaf_pos{.x = 6, .y = 10, .z = 5};
        (void)environment.chunk.set_state(log_pos.x, log_pos.y, log_pos.z, log);
        auto random = make_random(36);
        const std::vector<block_pos> logs{log_pos};
        const std::vector<block_pos> leaves{leaf_pos};
        tree_decorator_context context{.context = environment.context,
                                       .states = environment.registry,
                                       .random = random,
                                       .logs = logs,
                                       .leaves = leaves};
        const pale_moss_decorator decorator{1.0F, 1.0F, 0.0F,
                                            "minecraft:pale_moss_patch"};
        decorator.place(context);
        expect(environment.name_at(log_pos.x, log_pos.y - 1, log_pos.z)
                   .starts_with("minecraft:pale_hanging_moss"))
            << environment.name_at(log_pos.x, log_pos.y - 1, log_pos.z);
        expect(environment.name_at(leaf_pos.x, leaf_pos.y - 1, leaf_pos.z)
                   .starts_with("minecraft:pale_hanging_moss"))
            << environment.name_at(leaf_pos.x, leaf_pos.y - 1, leaf_pos.z);
    };
};

suite<"worldgen/feature-closure"> feature_closure_tests = [] {
    "a selector string follows its placed feature into the closure"_test = [] {
        block_state_registry states;
        feature_registry registry{states};
        feature_data_set data;
        configured_feature_document selector;
        selector.id = "oracle:selector";
        selector.document = parse(
            R"({"type":"minecraft:random_selector",
                "default":"oracle:tree","features":[]})");
        data.features.emplace(selector.id, std::move(selector));
        configured_feature_document tree;
        tree.id = "oracle:tree";
        tree.document = parse(R"({"type":"minecraft:simple_block",
                       "to_place":{"id":"minecraft:oak_sapling"}})");
        data.features.emplace(tree.id, std::move(tree));
        placed_feature_document entry;
        entry.id = "oracle:entry";
        entry.feature = "oracle:selector";
        entry.placement = parse("[]");
        data.placed.emplace(entry.id, std::move(entry));
        placed_feature_document placed_tree;
        placed_tree.id = "oracle:tree";
        placed_tree.feature = "oracle:tree";
        placed_tree.placement = parse("[]");
        data.placed.emplace(placed_tree.id, std::move(placed_tree));

        const std::array<std::string, 1> ids{"oracle:entry"};
        const auto loaded = registry.load_placed(data, ids);
        expect(loaded.has_value()) << loaded.error().message;
        if (!loaded) {
            return;
        }
        expect(registry.placed().contains("oracle:entry"));
        expect(registry.placed().contains("oracle:tree"));
    };

    "an unresolvable placed reference is reported"_test = [] {
        block_state_registry states;
        feature_registry registry{states};
        feature_data_set data;
        configured_feature_document selector;
        selector.id = "oracle:selector";
        selector.document = parse(
            R"({"type":"minecraft:simple_random_selector",
                "features":["oracle:missing"]})");
        data.features.emplace(selector.id, std::move(selector));
        placed_feature_document entry;
        entry.id = "oracle:entry";
        entry.feature = "oracle:selector";
        entry.placement = parse("[]");
        data.placed.emplace(entry.id, std::move(entry));
        const std::array<std::string, 1> ids{"oracle:entry"};
        const auto loaded = registry.load_placed(data, ids);
        expect(!loaded.has_value());
        if (!loaded) {
            expect(loaded.error().code == error_code::not_found);
        }
    };
};

suite<"worldgen/feature-sculk"> sculk_feature_tests = [] {
    "a sculk patch spreads charges over its substrate"_test = [] {
        fixture environment;
        const auto stone = environment.resolve("minecraft:stone");
        const auto sculk = environment.resolve("minecraft:sculk");
        environment.fill_layer(4, stone);
        (void)environment.chunk.set_state(5, 5, 5, sculk);
        (void)environment.chunk.set_state(9, 5, 9, sculk);
        const std::array<block_state_id, 2> replaceable{stone, sculk};
        environment.registry.add_tag("minecraft:sculk_replaceable_world_gen",
                                     replaceable);
        environment.registry.add_tag("minecraft:sculk_replaceable",
                                     replaceable);
        decoded_feature feature{environment.registry};
        if (!feature.load(R"({"type":"minecraft:sculk_patch",
                              "charge_count":8,
                              "amount_per_charge":100,
                              "spread_attempts":16,
                              "growth_rounds":4,
                              "spread_rounds":1})")) {
            return;
        }
        auto random = make_random(41);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 5, .y = 5, .z = 5}));
        jint sculk_blocks = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    if (environment.name_at(x, y, z).starts_with(
                            "minecraft:sculk")) {
                        ++sculk_blocks;
                    }
                }
            }
        }
        expect(sculk_blocks > 2) << sculk_blocks;
    };

    "a simple block feature rejects vegetation without a supporting block"_test =
        [] {
            fixture environment;
            const auto grass = environment.resolve("minecraft:grass_block");
            const auto water = environment.resolve("minecraft:water");
            const auto short_grass =
                environment.resolve("minecraft:short_grass");
            expect(grass != no_block_state);
            expect(water != no_block_state);
            expect(short_grass != no_block_state);
            const std::array<block_state_id, 1> motion{grass};
            const std::array<block_state_id, 1> plants{short_grass};
            environment.registry.add_tag("minecraft:blocks_motion_in_heightmap",
                                         motion);
            environment.registry.add_tag("minecraft:replaceable_by_trees",
                                         plants);
            environment.chunk.set_state(8, 10, 8, water);
            decoded_feature feature{environment.registry};
            if (!feature.load(
                    R"({"type":"minecraft:simple_block",
                        "to_place":{"id":"minecraft:short_grass"}})")) {
                return;
            }
            auto random = make_random(7);
            expect(!feature.place(environment.context, random,
                                  block_pos{.x = 8, .y = 11, .z = 8}));
            expect(environment.name_at(8, 11, 8) == "minecraft:air");
        };

    "a simple block feature places both halves of a double plant"_test = [] {
        fixture environment;
        const auto grass = environment.resolve("minecraft:grass_block");
        expect(grass != no_block_state);
        const std::array<block_state_id, 1> motion{grass};
        environment.registry.add_tag("minecraft:blocks_motion_in_heightmap",
                                     motion);
        environment.chunk.set_state(8, 10, 8, grass);
        decoded_feature feature{environment.registry};
        if (!feature.load(
                R"({"type":"minecraft:simple_block",
                    "to_place":{"id":"minecraft:tall_grass",
                                "properties":{"half":"lower"}}})")) {
            return;
        }
        auto random = make_random(9);
        expect(feature.place(environment.context, random,
                             block_pos{.x = 8, .y = 11, .z = 8}));
        expect(
            environment.name_at(8, 11, 8).starts_with("minecraft:tall_grass"));
        expect(environment.name_at(8, 11, 8).find("half=lower") !=
               std::string_view::npos);
        expect(
            environment.name_at(8, 12, 8).starts_with("minecraft:tall_grass"));
        expect(environment.name_at(8, 12, 8).find("half=upper") !=
               std::string_view::npos);
    };
};
