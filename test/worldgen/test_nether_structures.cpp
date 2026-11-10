// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Bastion and nether fossil structures. The env-gated cases compare a start
// against a reference world that keeps one structure set; the synthetic cases
// pin the fossil column scan, template choice and processor behaviour without
// a reference world.

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/nether_fossil.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include "worldgen/jigsaw_support.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::structure;

auto child(const data::nbt::compound *entries, std::string_view key)
    -> const data::nbt::value * {
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found = entries->find(std::string{key});
    return found == entries->end() ? nullptr : &found->second;
}

auto read_chunk(std::string_view path, jint chunk_x, jint chunk_z)
    -> std::optional<data::nbt::value> {
    std::ifstream stream{std::string{path}, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    const auto local_x = static_cast<std::size_t>(((chunk_x % 32) + 32) % 32);
    const auto local_z = static_cast<std::size_t>(((chunk_z % 32) + 32) % 32);
    const auto index = local_x + local_z * 32;
    if ((index + 1) * 4 > bytes.size()) {
        return std::nullopt;
    }
    const auto read_u32 = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
               static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    const auto offset = static_cast<std::size_t>(read_u32(index * 4) >> 8U);
    if (offset == 0 || offset * 4096 + 5 > bytes.size()) {
        return std::nullopt;
    }
    const auto length = read_u32(offset * 4096);
    if (length < 1 || offset * 4096 + 4 + length > bytes.size()) {
        return std::nullopt;
    }
    if (std::to_integer<int>(bytes[offset * 4096 + 4]) != 2) {
        return std::nullopt;
    }
    const auto payload = std::span{bytes.data() + offset * 4096 + 5,
                                   static_cast<std::size_t>(length) - 1U};
    auto parsed = data::nbt::read(payload, data::nbt::compression::zlib);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

auto integer_field(const data::nbt::compound *entries, std::string_view key,
                   jint fallback) -> jint {
    const auto *value = child(entries, key);
    if (value == nullptr) {
        return fallback;
    }
    if (const auto number = value->as_int(); number.has_value()) {
        return *number;
    }
    if (const auto number = value->as_byte(); number.has_value()) {
        return *number;
    }
    return fallback;
}

auto string_field(const data::nbt::compound *entries, std::string_view key,
                  std::string_view fallback) -> std::string {
    const auto *value = child(entries, key);
    if (value == nullptr) {
        return std::string{fallback};
    }
    const auto text = value->as_string();
    return text.has_value() ? std::string{*text} : std::string{fallback};
}

auto rotation_from_name(std::string_view name) -> jint {
    if (name == "CLOCKWISE_90") {
        return 1;
    }
    if (name == "CLOCKWISE_180") {
        return 2;
    }
    if (name == "COUNTERCLOCKWISE_90") {
        return 3;
    }
    return 0;
}

auto piece_bounds(const data::nbt::value &entry)
    -> std::optional<structure_bbox> {
    const auto *bb = child(entry.as_compound(), "BB");
    const auto values = bb == nullptr ? std::nullopt : bb->as_int_array();
    if (!values.has_value() || values->size() != 6U) {
        return std::nullopt;
    }
    return structure_bbox{.min_x = (*values)[0],
                          .min_y = (*values)[1],
                          .min_z = (*values)[2],
                          .max_x = (*values)[3],
                          .max_y = (*values)[4],
                          .max_z = (*values)[5]};
}

auto env_int(const char *name) -> std::optional<jint> {
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return static_cast<jint>(std::atoi(value));
}

auto env_seed() -> std::optional<jlong> {
    const char *value = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
    if (value == nullptr) {
        return std::nullopt;
    }
    return static_cast<jlong>(std::strtoll(value, nullptr, 10));
}

auto ints(std::initializer_list<jint> values) -> data::nbt::value {
    auto list = data::nbt::make_list(data::nbt::tag_type::int_);
    for (const auto value : values) {
        (void)list.push(data::nbt::value{value});
    }
    return list;
}

auto named_palette_entry(std::string name, data::nbt::compound properties = {})
    -> data::nbt::compound {
    data::nbt::compound entry;
    entry["Name"] = data::nbt::value{std::move(name)};
    if (!properties.empty()) {
        entry["Properties"] = data::nbt::value{std::move(properties)};
    }
    return entry;
}

auto palette_block(std::initializer_list<jint> pos, jint state)
    -> data::nbt::compound {
    data::nbt::compound entry;
    entry["pos"] = ints(pos);
    entry["state"] = data::nbt::value{state};
    return entry;
}

auto write_template(const std::filesystem::path &root, std::string_view name,
                    std::initializer_list<jint> size,
                    std::initializer_list<std::string_view> palette,
                    const std::vector<data::nbt::compound> &blocks) -> bool {
    data::nbt::compound root_tag;
    root_tag["size"] = ints(size);
    auto palette_list = data::nbt::make_list(data::nbt::tag_type::compound);
    for (const auto entry : palette) {
        (void)palette_list.push(
            data::nbt::value{named_palette_entry(std::string{entry})});
    }
    root_tag["palette"] = std::move(palette_list);
    auto block_list = data::nbt::make_list(data::nbt::tag_type::compound);
    for (const auto &block : blocks) {
        (void)block_list.push(data::nbt::value{block});
    }
    root_tag["blocks"] = std::move(block_list);
    const auto directory = root / "minecraft" / "structure" / "nether_fossils";
    std::filesystem::create_directories(directory);
    const auto path = directory / (std::string{name} + ".nbt");
    return data::nbt::write_file(path.string(), data::nbt::value{root_tag}, "",
                                 data::nbt::compression::gzip)
        .has_value();
}

auto make_fossil_templates(const std::filesystem::path &root) -> bool {
    for (jint index = 1; index <= 14; ++index) {
        const auto name = "fossil_" + std::to_string(index);
        if (!write_template(root, name, {1, 1, 1}, {"minecraft:bone_block"},
                            {palette_block({0, 0, 0}, 0)})) {
            return false;
        }
    }
    return true;
}

auto make_states(block_state_registry &states) -> bool {
    if (!states.intern("minecraft:air", {.air = true})) {
        return false;
    }
    if (!states.intern("minecraft:netherrack",
                       {.air = false, .blocks_motion = true})) {
        return false;
    }
    if (!states.intern("minecraft:soul_sand",
                       {.air = false, .blocks_motion = true})) {
        return false;
    }
    if (!states.intern("minecraft:bone_block",
                       {.air = false, .blocks_motion = true})) {
        return false;
    }
    return states
        .intern("minecraft:structure_block",
                {.air = false, .blocks_motion = true})
        .has_value();
}

auto make_fossil_structure(const data::json::value &config)
    -> structure_document {
    return structure_document{
        .id = "minecraft:nether_fossil",
        .type = "minecraft:nether_fossil",
        .step = "underground_decoration",
        .config = config,
    };
}

auto fossil_document(jint height) -> data::json::value {
    const std::string text =
        R"({"type":"minecraft:nether_fossil","height":{"type":"minecraft:constant","value":{"absolute":)" +
        std::to_string(height) + R"(}}})";
    return *data::json::value::parse(text);
}

} // namespace

suite<"worldgen/nether-structures"> nether_structure_tests = [] {
    "the bastion start matches the reference piece list"_test = [] {
        const char *region_path = std::getenv("REFRAIN_VANILLA_BASTION_REGION");
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        const auto chunk_x = env_int("REFRAIN_VANILLA_BASTION_CHUNK_X");
        const auto chunk_z = env_int("REFRAIN_VANILLA_BASTION_CHUNK_Z");
        const auto seed = env_seed();
        if (region_path == nullptr || data_root == nullptr ||
            templates_root == nullptr || !chunk_x || !chunk_z || !seed) {
            return;
        }
        const auto reference = read_chunk(region_path, *chunk_x, *chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *entry = child(reference->as_compound(), "structures");
        const auto *reference_starts =
            entry == nullptr ? nullptr : child(entry->as_compound(), "starts");
        const auto *reference_bastion =
            reference_starts == nullptr ? nullptr
                                        : child(reference_starts->as_compound(),
                                                "minecraft:bastion_remnant");
        const auto *children =
            reference_bastion == nullptr
                ? nullptr
                : child(reference_bastion->as_compound(), "Children");
        const auto reference_pieces =
            children == nullptr ? nullptr : children->as_list();
        expect(reference_pieces != nullptr);
        if (reference_pieces == nullptr) {
            return;
        }
        const auto data = load_structure_data(data_root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        block_state_registry states;
        expect(make_states(states));
        test_support::prime_template_states(states, templates_root,
                                            "minecraft/structure/bastion");
        structure_template_manager manager{templates_root, states};
        const structure_start_context context{
            .seed = *seed,
            .data = &*data,
            .min_y = 0,
            .world_height = 256,
            .sea_level = 32,
            .generation_min_y = 0,
            .generation_height = 128,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:nether_wastes";
            },
            .height_at = [](jint, jint, heightmap_kind) -> jint { return 70; },
            .templates = &manager,
        };
        const auto starts = generate_structure_starts(
            chunk_pos{.x = *chunk_x, .z = *chunk_z}, context);
        const auto found = std::find_if(
            starts.begin(), starts.end(),
            [](const structure::structure_start &start) {
                return start.structure == "minecraft:bastion_remnant";
            });
        expect(found != starts.end());
        if (found == starts.end()) {
            return;
        }
        expect(found->pieces.size() == reference_pieces->items.size());
        if (found->pieces.size() != reference_pieces->items.size()) {
            return;
        }
        for (std::size_t index = 0; index < found->pieces.size(); ++index) {
            const auto &piece = found->pieces[index];
            const auto &reference_piece = reference_pieces->items[index];
            const auto *room = reference_piece.as_compound();
            const auto expected_bounds = piece_bounds(reference_piece);
            expect(expected_bounds.has_value());
            if (!expected_bounds) {
                return;
            }
            expect(piece.bounds.min_x == expected_bounds->min_x);
            expect(piece.bounds.min_y == expected_bounds->min_y);
            expect(piece.bounds.min_z == expected_bounds->min_z);
            expect(piece.bounds.max_x == expected_bounds->max_x);
            expect(piece.bounds.max_y == expected_bounds->max_y);
            expect(piece.bounds.max_z == expected_bounds->max_z);
            expect(piece.template_origin.x == integer_field(room, "PosX", 0));
            expect(piece.template_origin.y == integer_field(room, "PosY", 0));
            expect(piece.template_origin.z == integer_field(room, "PosZ", 0));
            expect(piece.template_rotation ==
                   rotation_from_name(string_field(room, "rotation", "NONE")));
            expect(piece.jigsaw.has_value());
            if (!piece.jigsaw) {
                return;
            }
            const auto *element = child(room, "pool_element");
            expect(element != nullptr);
            if (element == nullptr) {
                return;
            }
            const auto *element_entries = element->as_compound();
            expect(piece.jigsaw->element.template_id ==
                   string_field(element_entries, "location", ""));
            expect(piece.jigsaw->element.projection ==
                   (string_field(element_entries, "projection", "rigid") ==
                            "terrain_matching"
                        ? pool_projection::terrain_matching
                        : pool_projection::rigid));
            expect(piece.jigsaw->ground_level_delta ==
                   integer_field(room, "ground_level_delta", 0));
            const auto *junctions = child(room, "junctions");
            const auto *reference_junctions =
                junctions == nullptr ? nullptr : junctions->as_list();
            expect(piece.jigsaw->junctions.size() ==
                   (reference_junctions == nullptr
                        ? 0U
                        : reference_junctions->items.size()));
            if (reference_junctions != nullptr &&
                piece.jigsaw->junctions.size() ==
                    reference_junctions->items.size()) {
                for (std::size_t junction_index = 0;
                     junction_index < reference_junctions->items.size();
                     ++junction_index) {
                    const auto *junction =
                        reference_junctions->items[junction_index]
                            .as_compound();
                    const auto &actual =
                        piece.jigsaw->junctions[junction_index];
                    expect(actual.source_x ==
                           integer_field(junction, "source_x", 0));
                    expect(actual.source_z ==
                           integer_field(junction, "source_z", 0));
                    expect(actual.source_ground_y ==
                           integer_field(junction, "source_ground_y", 0));
                    expect(actual.delta_y ==
                           integer_field(junction, "delta_y", 0));
                }
            }
        }
    };

    "a fossil start scans for air above a supported block"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-nether-fossil-start";
        std::filesystem::remove_all(directory);
        expect(make_fossil_templates(directory));
        block_state_registry states;
        expect(make_states(states));
        structure_template_manager manager{directory.string(), states};
        const auto document = fossil_document(100);
        const auto structure = make_fossil_structure(document);
        const block_state_id air = states.air();
        const auto soul_sand =
            states.find("minecraft:soul_sand").value_or(no_block_state);
        const auto netherrack =
            states.find("minecraft:netherrack").value_or(no_block_state);
        const auto block_at = [&](jint, jint y, jint) -> block_state_id {
            if (y > 50) {
                return air;
            }
            return y == 50 ? soul_sand : netherrack;
        };
        constexpr jlong seed = 1234;
        const auto context = nether_fossil_generation_context{
            .structure = &structure,
            .templates = &manager,
            .states = &states,
            .block_at = block_at,
            .seed = seed,
            .min_y = 0,
            .world_height = 256,
            .sea_level = 32,
            .generation_min_y = 0,
            .generation_height = 128,
            .valid_biome = [](const block_pos &) { return true; },
        };
        const auto start =
            nether_fossil_start(chunk_pos{.x = 0, .z = 0}, context);
        expect(start.has_value());
        if (!start) {
            return;
        }
        expect(start->pieces.size() == 1U);
        expect(start->pieces.front().template_origin.y == 50);
        expect(start->pieces.front().bounds.min_y == 50);
        expect(start->pieces.front().type == "minecraft:nether_fossil");
        worldgen_random expected{std::make_unique<legacy_random_source>(0)};
        expected.set_large_feature_seed(seed, 0, 0);
        const jint block_x = expected.next_int(16);
        const jint block_z = expected.next_int(16);
        const jint rotation = expected.next_int(4);
        const jint fossil_index = expected.next_int(14);
        expect(start->pieces.front().template_origin.x == block_x);
        expect(start->pieces.front().template_origin.z == block_z);
        expect(start->pieces.front().template_rotation == rotation);
        expect(start->pieces.front().template_id ==
               "minecraft:nether_fossils/fossil_" +
                   std::to_string(fossil_index + 1));
    };

    "a fossil start rejects columns without support or below sea level"_test =
        [] {
            const auto directory =
                std::filesystem::path{core::io::temp_directory()} /
                "refrain-nether-fossil-reject";
            std::filesystem::remove_all(directory);
            expect(make_fossil_templates(directory));
            block_state_registry states;
            expect(make_states(states));
            structure_template_manager manager{directory.string(), states};
            const auto document = fossil_document(100);
            const auto structure = make_fossil_structure(document);
            const block_state_id air = states.air();
            const auto netherrack =
                states.find("minecraft:netherrack").value_or(no_block_state);
            const auto make_context =
                [&](jint sea_level, bool valid_biome,
                    const std::function<block_state_id(jint, jint, jint)>
                        &block_at) {
                    return nether_fossil_generation_context{
                        .structure = &structure,
                        .templates = &manager,
                        .states = &states,
                        .block_at = block_at,
                        .seed = 1234,
                        .min_y = 0,
                        .world_height = 256,
                        .sea_level = sea_level,
                        .generation_min_y = 0,
                        .generation_height = 128,
                        .valid_biome =
                            [valid_biome](const block_pos &) {
                                return valid_biome;
                            },
                    };
                };
            const auto empty_column =
                [air](jint, jint, jint) -> block_state_id { return air; };
            expect(!nether_fossil_start(chunk_pos{.x = 0, .z = 0},
                                        make_context(32, true, empty_column))
                        .has_value());
            const auto low_column = [air, netherrack](jint, jint y,
                                                      jint) -> block_state_id {
                return y <= 50 ? netherrack : air;
            };
            expect(!nether_fossil_start(chunk_pos{.x = 0, .z = 0},
                                        make_context(60, true, low_column))
                        .has_value());
            const auto supported_column =
                [air, netherrack](jint, jint y, jint) -> block_state_id {
                return y <= 50 ? netherrack : air;
            };
            expect(
                !nether_fossil_start(chunk_pos{.x = 0, .z = 0},
                                     make_context(32, false, supported_column))
                     .has_value());
        };

    "a fossil piece ignores air and structure blocks"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-nether-fossil-piece";
        std::filesystem::remove_all(directory);
        expect(write_template(directory, "fossil_1", {3, 1, 1},
                              {"minecraft:bone_block", "minecraft:air",
                               "minecraft:structure_block"},
                              {palette_block({0, 0, 0}, 0),
                               palette_block({1, 0, 0}, 1),
                               palette_block({2, 0, 0}, 2)}));
        block_state_registry states;
        expect(make_states(states));
        structure_template_manager manager{directory.string(), states};
        proto_chunk chunk{states, 0, 0, 0, 64};
        chunk_region region{chunk, 1};
        const auto netherrack =
            states.find("minecraft:netherrack").value_or(no_block_state);
        for (jint x = 0; x < 3; ++x) {
            expect(region.set_block_state(x, 0, 0, netherrack));
        }
        std::unordered_map<std::string, jint> adjusted;
        const auto resolve = [&](std::string_view key) -> block_state_id {
            return resolve_block_state(states, key);
        };
        const structure_place_context context{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .state = resolve,
            .templates = &manager,
            .registry = &states,
            .seed = 1234,
            .sea_level = 32,
            .adjusted_heights = &adjusted,
        };
        const structure_piece_record piece{
            .type = "minecraft:nether_fossil",
            .min_x = 0,
            .min_y = 0,
            .min_z = 0,
            .max_x = 2,
            .max_y = 0,
            .max_z = 0,
            .template_id = "minecraft:nether_fossils/fossil_1",
            .template_x = 0,
            .template_y = 0,
            .template_z = 0,
            .template_rotation = 0,
        };
        worldgen_random random{std::make_unique<legacy_random_source>(0)};
        place_nether_fossil_piece(context, piece, random);
        expect(states.name(chunk.state_at(0, 0, 0)) == "minecraft:bone_block");
        expect(states.name(chunk.state_at(1, 0, 0)) == "minecraft:netherrack");
        expect(states.name(chunk.state_at(2, 0, 0)) == "minecraft:netherrack");
        // The piece is placed once per start; a second call is a no-op.
        (void)region.set_block_state(0, 0, 0, netherrack);
        place_nether_fossil_piece(context, piece, random);
        expect(states.name(chunk.state_at(0, 0, 0)) == "minecraft:netherrack");
    };

    "a fossil piece adds a rotated dried ghast above air"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-nether-fossil-ghast";
        std::filesystem::remove_all(directory);
        expect(write_template(directory, "fossil_1", {3, 1, 1},
                              {"minecraft:air"},
                              {palette_block({0, 0, 0}, 0)}));
        block_state_registry states;
        expect(make_states(states));
        structure_template_manager manager{directory.string(), states};
        const auto resolve = [&](std::string_view key) -> block_state_id {
            return resolve_block_state(states, key);
        };
        std::optional<jlong> chosen_seed;
        jint expected_x = 0;
        jint expected_z = 0;
        for (jlong seed = 1; seed < 100; ++seed) {
            auto factory = new_positional_factory(algorithm::legacy, seed);
            auto positional = factory->at(1, 0, 0);
            if (positional->next_float() >= 0.5F) {
                continue;
            }
            expected_x = positional->next_int(3);
            expected_z = positional->next_int(1);
            chosen_seed = seed;
            break;
        }
        expect(chosen_seed.has_value());
        if (!chosen_seed) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 64};
        chunk_region region{chunk, 1};
        std::unordered_map<std::string, jint> adjusted;
        const structure_place_context context{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .state = resolve,
            .templates = &manager,
            .registry = &states,
            .seed = *chosen_seed,
            .sea_level = 32,
            .adjusted_heights = &adjusted,
        };
        const structure_piece_record piece{
            .type = "minecraft:nether_fossil",
            .min_x = 0,
            .min_y = 0,
            .min_z = 0,
            .max_x = 2,
            .max_y = 0,
            .max_z = 0,
            .template_id = "minecraft:nether_fossils/fossil_1",
            .template_x = 0,
            .template_y = 0,
            .template_z = 0,
        };
        worldgen_random random{std::make_unique<legacy_random_source>(0)};
        place_nether_fossil_piece(context, piece, random);
        const std::string placed{
            states.name(chunk.state_at(expected_x, 0, expected_z))};
        expect(placed.starts_with("minecraft:dried_ghast["));
    };
};
