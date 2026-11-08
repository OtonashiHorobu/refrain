// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Jigsaw start expansion against the reference piece list. The reference chunk
// is a generated village start; the test compares every piece's bounds,
// rotation, element and junctions with the generated start. Heights come from
// the unbearded noise fill, which is the height source `getFirstFreeHeight`
// sees during the structure-starts stage.
//
// Inputs: REFRAIN_VANILLA_JIGSAW_REGION, REFRAIN_VANILLA_WORLDGEN and
// REFRAIN_VANILLA_STRUCTURES.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include "worldgen/jigsaw_support.hpp"

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

constexpr jint oracle_min_y = -64;
constexpr jint oracle_height = 384;

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
    const auto compression = bytes[offset * 4096 + 4];
    if (std::to_integer<int>(compression) != 2) {
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

auto rotation_name(structure::template_rotation rotation) -> std::string_view {
    switch (rotation) {
    case structure::template_rotation::clockwise_90:
        return "CLOCKWISE_90";
    case structure::template_rotation::clockwise_180:
        return "CLOCKWISE_180";
    case structure::template_rotation::counterclockwise_90:
        return "COUNTERCLOCKWISE_90";
    case structure::template_rotation::none:
        break;
    }
    return "NONE";
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

struct digest {
    std::uint64_t value{1469598103934665603ULL};

    void mix(std::string_view text) {
        for (const char ch : text) {
            value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
            value *= 1099511628211ULL;
        }
    }

    void number(jint number) { mix(std::to_string(number) + ","); }
};

auto element_kind_number(std::string_view type) -> jint {
    if (type == "minecraft:single_pool_element") {
        return 1;
    }
    if (type == "minecraft:legacy_single_pool_element") {
        return 2;
    }
    if (type == "minecraft:list_pool_element") {
        return 3;
    }
    if (type == "minecraft:feature_pool_element") {
        return 4;
    }
    return 0;
}

/// The identity fields both sides of the comparison expose.
auto identity_digest(jint min_x, jint min_y, jint min_z, jint max_x, jint max_y,
                     jint max_z, jint origin_x, jint origin_y, jint origin_z,
                     std::string_view rotation, jint ground_delta, jint kind,
                     std::string_view template_id, std::string_view feature,
                     jint projection) -> std::uint64_t {
    digest hash;
    hash.number(min_x);
    hash.number(min_y);
    hash.number(min_z);
    hash.number(max_x);
    hash.number(max_y);
    hash.number(max_z);
    hash.number(origin_x);
    hash.number(origin_y);
    hash.number(origin_z);
    hash.mix(rotation);
    hash.number(ground_delta);
    hash.number(kind);
    hash.mix(template_id);
    hash.mix(feature);
    hash.number(projection);
    return hash.value;
}

auto reference_piece_digest(const data::nbt::value &piece) -> std::uint64_t {
    const auto *compound = piece.as_compound();
    std::array<jint, 6> bounds{};
    if (const auto *box = child(compound, "BB");
        box != nullptr && box->as_int_array().has_value()) {
        const auto values = *box->as_int_array();
        if (values.size() == 6U) {
            for (std::size_t i = 0; i < 6U; ++i) {
                bounds[i] = values[i];
            }
        }
    }
    std::string template_id;
    std::string feature;
    jint kind = 0;
    jint projection = 0;
    const auto *element = child(compound, "pool_element");
    if (element != nullptr && element->as_compound() != nullptr) {
        const auto *entries = element->as_compound();
        kind = element_kind_number(string_field(entries, "element_type", ""));
        template_id = string_field(entries, "location", "");
        feature = string_field(entries, "feature", "");
        projection =
            string_field(entries, "projection", "") == "terrain_matching" ? 1
                                                                          : 0;
    }
    return identity_digest(
        bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5],
        integer_field(compound, "PosX", 0), integer_field(compound, "PosY", 0),
        integer_field(compound, "PosZ", 0),
        string_field(compound, "rotation", "NONE"),
        integer_field(compound, "ground_level_delta", 0), kind, template_id,
        feature, projection);
}

auto generated_piece_digest(const structure::structure_piece &piece)
    -> std::uint64_t {
    jint ground_delta = 0;
    jint kind = 0;
    std::string template_id;
    std::string feature;
    jint projection = 0;
    if (piece.jigsaw.has_value()) {
        ground_delta = piece.jigsaw->ground_level_delta;
        kind = static_cast<jint>(piece.jigsaw->element.kind);
        template_id = piece.jigsaw->element.template_id;
        feature = piece.jigsaw->element.feature;
        projection = static_cast<jint>(piece.jigsaw->element.projection);
    }
    return identity_digest(
        piece.bounds.min_x, piece.bounds.min_y, piece.bounds.min_z,
        piece.bounds.max_x, piece.bounds.max_y, piece.bounds.max_z,
        piece.template_origin.x, piece.template_origin.y,
        piece.template_origin.z,
        rotation_name(
            static_cast<structure::template_rotation>(piece.template_rotation)),
        ground_delta, kind, template_id, feature, projection);
}

struct height_source {
    const density::noise_settings *settings{nullptr};
    block_state_registry *states{nullptr};
    jlong seed{0};
    terrain::block_ids ids{};
    terrain::fluid_picker picker{};
    std::map<chunk_pos, std::unique_ptr<proto_chunk>> chunks{};

    auto at(jint x, jint z, heightmap_kind kind) -> jint {
        const chunk_pos pos{.x = x >> 4, .z = z >> 4};
        auto found = chunks.find(pos);
        if (found == chunks.end()) {
            const auto &noise = *this->settings;
            auto random = new_positional_factory(algorithm::xoroshiro, seed);
            auto aquifer_factory =
                random->from_hash_of("minecraft:aquifer")->fork_positional();
            auto chunk = std::make_unique<proto_chunk>(
                *states, pos.x, pos.z, oracle_min_y, oracle_height);
            chunk->set_status(chunk_status::biomes);
            const volume chunk_volume{16,         oracle_height, 16,
                                      pos.x * 16, oracle_min_y,  pos.z * 16};
            const density::sampler_context sampler_context;
            terrain::noise_aquifer aquifer{noise.aquifer, *aquifer_factory,
                                           chunk_volume,  picker,
                                           ids,           sampler_context};
            terrain::fill_proto_chunk(*chunk, options(),
                                      *noise.router.final_density,
                                      sampler_context, aquifer);
            found = chunks.emplace(pos, std::move(chunk)).first;
        }
        return found->second->first_available(kind, x & 15, z & 15);
    }

    [[nodiscard]] auto options() const -> terrain::proto_fill_options {
        return terrain::proto_fill_options{
            .air = ids.air,
            .default_block = ids.default_block,
            .default_fluid = ids.default_fluid,
            .lava = ids.lava,
        };
    }
};

} // namespace

namespace {

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

auto palette_block(std::initializer_list<jint> pos, jint state,
                   data::nbt::compound nbt = {}) -> data::nbt::compound {
    data::nbt::compound entry;
    entry["pos"] = ints(pos);
    entry["state"] = data::nbt::value{state};
    if (!nbt.empty()) {
        entry["nbt"] = data::nbt::value{std::move(nbt)};
    }
    return entry;
}

auto jigsaw_nbt(std::string name, std::string target, std::string pool,
                std::string joint) -> data::nbt::compound {
    data::nbt::compound tag;
    tag["name"] = data::nbt::value{std::move(name)};
    tag["target"] = data::nbt::value{std::move(target)};
    tag["pool"] = data::nbt::value{std::move(pool)};
    tag["joint"] = data::nbt::value{std::move(joint)};
    tag["final_state"] = data::nbt::value{std::string{"minecraft:stone"}};
    return tag;
}

/// Writes a one-jigsaw connector template. `top` names the orientation's top
/// direction; the jigsaw always points up or down.
auto write_connector_template(std::string_view root, std::string_view name,
                              jint size_y, std::string_view orientation,
                              std::string_view jigsaw_name,
                              std::string_view jigsaw_target,
                              std::string_view pool) -> bool {
    data::nbt::compound properties;
    properties["orientation"] = data::nbt::value{std::string{orientation}};
    data::nbt::compound root_tag;
    root_tag["size"] = ints({3, size_y, 3});
    auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
    (void)palette.push(data::nbt::value{
        named_palette_entry("minecraft:jigsaw", std::move(properties))});
    root_tag["palette"] = std::move(palette);
    auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
    (void)blocks.push(data::nbt::value{palette_block(
        {1, 0, 1}, 0,
        jigsaw_nbt(std::string{jigsaw_name}, std::string{jigsaw_target},
                   std::string{pool}, "aligned"))});
    root_tag["blocks"] = std::move(blocks);
    const auto directory = std::filesystem::path{root} / "test" / "structure";
    std::filesystem::create_directories(directory);
    const auto written = data::nbt::write_file(
        (directory / (std::string{name} + ".nbt")).string(),
        data::nbt::value{root_tag}, "", data::nbt::compression::gzip);
    return written.has_value();
}

struct jigsaw_spec {
    jint x{0};
    jint y{0};
    jint z{0};
    std::string orientation{};
    data::nbt::compound tag{};
};

auto write_multi_jigsaw_template(std::string_view root, std::string_view name,
                                 jint size_x, jint size_y, jint size_z,
                                 const std::vector<jigsaw_spec> &jigsaws)
    -> bool {
    data::nbt::compound root_tag;
    root_tag["size"] = ints({size_x, size_y, size_z});
    auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
    (void)palette.push(
        data::nbt::value{named_palette_entry("minecraft:stone")});
    for (const auto &jigsaw : jigsaws) {
        data::nbt::compound properties;
        properties["orientation"] =
            data::nbt::value{std::string{jigsaw.orientation}};
        (void)palette.push(data::nbt::value{
            named_palette_entry("minecraft:jigsaw", std::move(properties))});
    }
    root_tag["palette"] = std::move(palette);
    auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
    for (std::size_t index = 0; index < jigsaws.size(); ++index) {
        const auto &jigsaw = jigsaws[index];
        (void)blocks.push(data::nbt::value{
            palette_block({jigsaw.x, jigsaw.y, jigsaw.z},
                          static_cast<jint>(index) + 1, jigsaw.tag)});
    }
    root_tag["blocks"] = std::move(blocks);
    const auto directory = std::filesystem::path{root} / "test" / "structure";
    std::filesystem::create_directories(directory);
    return data::nbt::write_file(
               (directory / (std::string{name} + ".nbt")).string(),
               data::nbt::value{root_tag}, "", data::nbt::compression::gzip)
        .has_value();
}

auto connector(structure::jigsaw_front front, structure::jigsaw_front top,
               bool rollable, std::string target,
               std::string name = "test:connector")
    -> structure::jigsaw_connector {
    structure::jigsaw_connector value{
        .front = front, .top = top, .rollable = rollable};
    value.has_name = true;
    value.name = std::move(name);
    value.target = std::move(target);
    return value;
}

} // namespace

suite<"worldgen/jigsaw"> jigsaw_tests = [] {
    "pool weights expand in declaration order"_test = [] {
        const auto document = data::json::value::parse(
            R"({"fallback":"minecraft:empty","elements":[
                {"element":{"element_type":"minecraft:single_pool_element","location":"test:first","processors":"minecraft:empty","projection":"rigid"},"weight":2},
                {"element":{"element_type":"minecraft:empty_pool_element"},"weight":1}]})");
        expect(document.has_value());
        if (!document) {
            return;
        }
        auto pool = structure::decode_template_pool("test:pool", *document);
        expect(pool.has_value());
        if (!pool) {
            return;
        }
        expect(pool->size() == 3);
        expect(pool->at(0).template_id == "test:first");
        expect(pool->at(1).template_id == "test:first");
        expect(pool->at(2).kind == structure::pool_element_kind::empty);
        worldgen_random drawn{std::make_unique<legacy_random_source>(17)};
        worldgen_random expected{std::make_unique<legacy_random_source>(17)};
        const auto index = expected.next_int(3);
        const auto *element = pool->get_random_template(drawn);
        expect(element != nullptr);
        if (element != nullptr) {
            expect(element->kind ==
                   pool->at(static_cast<std::size_t>(index)).kind);
            expect(element->template_id ==
                   pool->at(static_cast<std::size_t>(index)).template_id);
        }
        const auto shuffled = pool->get_shuffled_templates(expected);
        expect(shuffled.size() == 3);
        std::size_t first_count = 0;
        for (const auto *entry : shuffled) {
            if (entry->template_id == "test:first") {
                ++first_count;
            }
        }
        expect(first_count == 2);
    };

    "connectors attach only through opposite fronts and matching tops"_test =
        [] {
            const auto source = connector(structure::jigsaw_front::up,
                                          structure::jigsaw_front::north, false,
                                          "test:connector");
            expect(structure::connectors_attach(
                source, connector(structure::jigsaw_front::down,
                                  structure::jigsaw_front::north, false,
                                  "test:source")));
            // An aligned joint needs equal tops.
            expect(!structure::connectors_attach(
                source, connector(structure::jigsaw_front::down,
                                  structure::jigsaw_front::south, false,
                                  "test:source")));
            // The destination name must equal the source target.
            expect(!structure::connectors_attach(
                source, connector(structure::jigsaw_front::down,
                                  structure::jigsaw_front::north, false,
                                  "test:source", "test:other")));
            // A rollable source ignores the top but not the front.
            expect(structure::connectors_attach(
                connector(structure::jigsaw_front::up,
                          structure::jigsaw_front::north, true,
                          "test:connector"),
                connector(structure::jigsaw_front::down,
                          structure::jigsaw_front::south, false,
                          "test:source")));
            expect(!structure::connectors_attach(
                source, connector(structure::jigsaw_front::up,
                                  structure::jigsaw_front::north, false,
                                  "test:source")));
        };

    "a jigsaw start expands connectors deterministically"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-jigsaw-start";
        std::filesystem::remove_all(directory);
        expect(write_connector_template(directory.string(), "source", 3,
                                        "up_north", "test:source",
                                        "test:connector", "test:children"));
        expect(write_connector_template(directory.string(), "child", 1,
                                        "down_north", "test:connector",
                                        "test:source", "test:empty"));

        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        for (const auto *key :
             {"minecraft:stone", "minecraft:jigsaw[orientation=up_north]",
              "minecraft:jigsaw[orientation=down_north]"}) {
            expect(structure::resolve_block_state(states, key) !=
                   no_block_state);
        }
        structure::structure_template_manager manager{directory.string(),
                                                      states};

        structure::structure_data_set data;
        const auto add_pool =
            [&data](std::string id, std::string fallback,
                    std::vector<structure::pool_element> elements) {
                structure::jigsaw_template_pool pool;
                pool.id = id;
                pool.fallback = std::move(fallback);
                for (auto &element : elements) {
                    pool.raw_elements.push_back(
                        structure::weighted_pool_element{
                            .element = std::move(element), .weight = 1});
                }
                pool.build_index();
                data.template_pools.insert_or_assign(std::move(id),
                                                     std::move(pool));
            };
        add_pool("test:start", "test:empty",
                 {structure::pool_element{
                     .kind = structure::pool_element_kind::single,
                     .projection = structure::pool_projection::rigid,
                     .template_id = "test:source"}});
        add_pool("test:children", "test:empty",
                 {structure::pool_element{
                     .kind = structure::pool_element_kind::single,
                     .projection = structure::pool_projection::rigid,
                     .template_id = "test:child"}});
        add_pool("test:empty", "test:empty", {});

        structure::structure_document document_value{
            .id = "test:jigsaw",
            .type = "minecraft:jigsaw",
            .step = "surface_structures",
            .jigsaw =
                structure::jigsaw_structure_config{
                    .start_pool = "test:start",
                    .max_depth = 1,
                    .use_expansion_hack = false,
                    .max_distance_horizontal = 80,
                    .max_distance_vertical = 80,
                },
        };
        document_value.jigsaw->start_height = structure::height_provider{
            .type = structure::height_provider::kind::constant,
            .min = structure::height_bound{.value = 0},
            .max = structure::height_bound{.value = 0},
        };
        data.structures.emplace("test:jigsaw", document_value);
        const auto generate = [&]() {
            return structure::jigsaw_start(
                chunk_pos{.x = 0, .z = 0},
                structure::jigsaw_generation_context{
                    .structure = &data.structures.at("test:jigsaw"),
                    .data = &data,
                    .templates = &manager,
                    .seed = 7,
                    .min_y = -64,
                    .world_height = 384,
                    .sea_level = 63,
                    .height_at = [](jint, jint, heightmap_kind) { return 0; },
                });
        };
        const auto first = generate();
        const auto second = generate();
        expect(first.has_value() && second.has_value());
        if (!first || !second) {
            return;
        }
        expect(first->pieces.size() == 2);
        expect(first->pieces.size() == second->pieces.size());
        if (first->pieces.size() == 2) {
            expect(first->pieces[1].jigsaw.has_value());
            if (first->pieces[1].jigsaw) {
                expect(first->pieces[1].jigsaw->element.template_id ==
                       "test:child");
            }
        }
        std::uint64_t first_digest = 1469598103934665603ULL;
        std::uint64_t second_digest = 1469598103934665603ULL;
        const auto mix_digest = [](std::uint64_t &hash,
                                   const structure::structure_piece &piece) {
            const auto mix = [&hash](jint value) {
                hash ^= static_cast<std::uint64_t>(value) + 0x9e3779b9U +
                        (hash << 6U) + (hash >> 2U);
            };
            mix(piece.bounds.min_x);
            mix(piece.bounds.min_y);
            mix(piece.bounds.min_z);
            mix(piece.bounds.max_x);
            mix(piece.bounds.max_y);
            mix(piece.bounds.max_z);
            mix(piece.template_origin.x);
            mix(piece.template_origin.y);
            mix(piece.template_origin.z);
            mix(piece.template_rotation);
        };
        for (const auto &piece : first->pieces) {
            mix_digest(first_digest, piece);
        }
        for (const auto &piece : second->pieces) {
            mix_digest(second_digest, piece);
        }
        expect(first_digest == second_digest);

        // A child whose front also points up cannot attach.
        expect(write_connector_template(directory.string(), "bad_child", 1,
                                        "up_north", "test:connector",
                                        "test:source", "test:empty"));
        add_pool("test:children", "test:empty",
                 {structure::pool_element{
                     .kind = structure::pool_element_kind::single,
                     .projection = structure::pool_projection::rigid,
                     .template_id = "test:bad_child"}});
        const auto rejected = generate();
        expect(rejected.has_value());
        if (rejected) {
            expect(rejected->pieces.size() == 1);
        }
    };

    "a child of an inside piece expands against the parent free box"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-jigsaw-inherited-free";
        std::filesystem::remove_all(directory);
        expect(write_multi_jigsaw_template(
            directory.string(), "parent", 7, 7, 7,
            {jigsaw_spec{.x = 3,
                         .y = 0,
                         .z = 3,
                         .orientation = "up_north",
                         .tag = jigsaw_nbt("test:source", "test:child",
                                           "test:children", "aligned")}}));
        expect(write_multi_jigsaw_template(
            directory.string(), "child", 3, 3, 3,
            {jigsaw_spec{.x = 1,
                         .y = 0,
                         .z = 1,
                         .orientation = "down_north",
                         .tag = jigsaw_nbt("test:child", "test:source",
                                           "test:empty", "aligned")},
             jigsaw_spec{.x = 2,
                         .y = 2,
                         .z = 2,
                         .orientation = "east_up",
                         .tag = jigsaw_nbt("test:child_exit", "test:grandchild",
                                           "test:grandchildren", "aligned")}}));
        expect(write_multi_jigsaw_template(
            directory.string(), "grandchild", 1, 1, 1,
            {jigsaw_spec{.x = 0,
                         .y = 0,
                         .z = 0,
                         .orientation = "west_up",
                         .tag = jigsaw_nbt("test:grandchild", "test:child_exit",
                                           "test:empty", "aligned")}}));

        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        for (const auto *key :
             {"minecraft:stone", "minecraft:jigsaw[orientation=up_north]",
              "minecraft:jigsaw[orientation=down_north]",
              "minecraft:jigsaw[orientation=east_up]",
              "minecraft:jigsaw[orientation=west_up]"}) {
            expect(structure::resolve_block_state(states, key) !=
                   no_block_state);
        }
        structure::structure_template_manager manager{directory.string(),
                                                      states};
        structure::structure_data_set data;
        const auto add_pool =
            [&data](std::string id, std::string fallback,
                    std::vector<structure::pool_element> elements) {
                structure::jigsaw_template_pool pool;
                pool.id = id;
                pool.fallback = std::move(fallback);
                for (auto &element : elements) {
                    pool.raw_elements.push_back(
                        structure::weighted_pool_element{
                            .element = std::move(element), .weight = 1});
                }
                pool.build_index();
                data.template_pools.insert_or_assign(std::move(id),
                                                     std::move(pool));
            };
        const auto single = [](std::string template_id) {
            return structure::pool_element{
                .kind = structure::pool_element_kind::single,
                .projection = structure::pool_projection::rigid,
                .template_id = std::move(template_id)};
        };
        add_pool("test:start", "test:empty", {single("test:parent")});
        add_pool("test:children", "test:empty", {single("test:child")});
        add_pool("test:grandchildren", "test:empty",
                 {single("test:grandchild")});
        add_pool("test:empty", "test:empty", {});
        structure::structure_document document_value{
            .id = "test:jigsaw",
            .type = "minecraft:jigsaw",
            .step = "surface_structures",
            .jigsaw =
                structure::jigsaw_structure_config{
                    .start_pool = "test:start",
                    .max_depth = 2,
                    .use_expansion_hack = false,
                    .max_distance_horizontal = 80,
                    .max_distance_vertical = 80,
                },
        };
        document_value.jigsaw->start_height = structure::height_provider{
            .type = structure::height_provider::kind::constant,
            .min = structure::height_bound{.value = 0},
            .max = structure::height_bound{.value = 0},
        };
        data.structures.emplace("test:jigsaw", document_value);
        const auto start = structure::jigsaw_start(
            chunk_pos{.x = 0, .z = 0},
            structure::jigsaw_generation_context{
                .structure = &data.structures.at("test:jigsaw"),
                .data = &data,
                .templates = &manager,
                .seed = 7,
                .min_y = -64,
                .world_height = 384,
                .sea_level = 63,
                .height_at = [](jint, jint, heightmap_kind) { return 0; },
            });
        expect(start.has_value());
        if (!start) {
            return;
        }
        expect(start->pieces.size() == 3U);
        if (start->pieces.size() == 3U) {
            expect(start->pieces[1].jigsaw.has_value());
            expect(start->pieces[2].jigsaw.has_value());
            if (start->pieces[1].jigsaw && start->pieces[2].jigsaw) {
                expect(start->pieces[1].jigsaw->element.template_id ==
                       "test:child");
                expect(start->pieces[2].jigsaw->element.template_id ==
                       "test:grandchild");
                const auto &grand = start->pieces[2].bounds;
                const auto &child_bounds = start->pieces[1].bounds;
                const auto &parent_bounds = start->pieces[0].bounds;
                expect(grand.min_x >= parent_bounds.min_x &&
                       grand.max_x <= parent_bounds.max_x &&
                       grand.min_y >= parent_bounds.min_y &&
                       grand.max_y <= parent_bounds.max_y &&
                       grand.min_z >= parent_bounds.min_z &&
                       grand.max_z <= parent_bounds.max_z);
                expect(grand.min_x < child_bounds.min_x ||
                       grand.max_x > child_bounds.max_x ||
                       grand.min_y < child_bounds.min_y ||
                       grand.max_y > child_bounds.max_y ||
                       grand.min_z < child_bounds.min_z ||
                       grand.max_z > child_bounds.max_z);
            }
        }
    };

    "the village start matches the reference piece list"_test = [] {
        const char *region = std::getenv("REFRAIN_VANILLA_JIGSAW_REGION");
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || data_root == nullptr ||
            templates_root == nullptr) {
            return;
        }
        const char *chunk_x_raw = std::getenv("REFRAIN_JIGSAW_CHUNK_X");
        const char *chunk_z_raw = std::getenv("REFRAIN_JIGSAW_CHUNK_Z");
        const char *seed_raw = std::getenv("REFRAIN_JIGSAW_SEED");
        if (chunk_x_raw == nullptr || chunk_z_raw == nullptr ||
            seed_raw == nullptr) {
            std::fprintf(stderr,
                         "skipping the jigsaw piece oracle: set "
                         "REFRAIN_JIGSAW_CHUNK_X, REFRAIN_JIGSAW_CHUNK_Z and "
                         "REFRAIN_JIGSAW_SEED to the reference candidate\n");
            return;
        }
        const jint chunk_x = std::atoi(chunk_x_raw);
        const jint chunk_z = std::atoi(chunk_z_raw);
        const jlong seed = std::strtoll(seed_raw, nullptr, 10);
        std::string structure_id = "minecraft:village_plains";
        const char *structure_env = std::getenv("REFRAIN_JIGSAW_STRUCTURE");
        if (structure_env != nullptr) {
            structure_id = structure_env;
        }
        const auto document = read_chunk(region, chunk_x, chunk_z);
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto *structures = child(document->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *start = starts == nullptr ? nullptr
                                              : child(starts->as_compound(),
                                                      structure_id.c_str());
        const auto *children = start == nullptr
                                   ? nullptr
                                   : child(start->as_compound(), "Children");
        const auto pieces = children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr);
        if (pieces == nullptr) {
            return;
        }
        auto data = structure::load_structure_data(data_root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        const auto found = data->structures.find(structure_id);
        expect(found != data->structures.end());
        if (found == data->structures.end()) {
            return;
        }
        const auto settings = density::load_noise_settings(
            data_root, "minecraft/overworld", algorithm::xoroshiro, seed);
        expect(settings.has_value());
        if (!settings) {
            return;
        }
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        (void)states.intern("minecraft:stone",
                            {.air = false, .blocks_motion = true});
        (void)states.intern("minecraft:water",
                            {.air = false, .fluid = true, .water = true});
        (void)states.intern("minecraft:lava", {.air = false, .fluid = true});
        test_support::prime_template_states(states, templates_root,
                                            "minecraft/structure/village");
        structure::structure_template_manager manager{templates_root, states};
        height_source heights{
            .settings = &*settings,
            .states = &states,
            .seed = seed,
            .ids = terrain::block_ids{.air = 0,
                                      .default_block = 1,
                                      .default_fluid = 2,
                                      .lava = 3},
            .picker =
                terrain::fluid_picker{
                    .sea_level = settings->sea_level,
                    .default_fluid = 2,
                    .lava = 3,
                    .air = 0,
                },
        };
        const structure::structure_start_context start_context{
            .seed = seed,
            .data = &*data,
            .min_y = oracle_min_y,
            .world_height = oracle_height,
            .sea_level = settings->sea_level,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:plains";
            },
            .height_at =
                [&](jint x, jint z, heightmap_kind kind) {
                    return heights.at(x, z, kind);
                },
            .templates = &manager,
        };
        const auto generated = structure::jigsaw_start(
            chunk_pos{.x = chunk_x, .z = chunk_z},
            structure::jigsaw_generation_context{
                .structure = &found->second,
                .data = &*data,
                .templates = &manager,
                .seed = seed,
                .min_y = oracle_min_y,
                .world_height = oracle_height,
                .sea_level = settings->sea_level,
                .height_at = start_context.height_at,
                .valid_biome = [](const block_pos &) { return true; },
                .column_allowed = [](jint, jint) { return true; },
            });
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        expect(generated->pieces.size() == pieces->items.size());
        const auto count =
            std::min(generated->pieces.size(), pieces->items.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto *expected = pieces->items[index].as_compound();
            expect(expected != nullptr);
            if (expected == nullptr) {
                continue;
            }
            const auto &actual = generated->pieces[index];
            const auto *box = child(expected, "BB");
            if (box == nullptr || !box->as_int_array().has_value()) {
                std::fprintf(stderr, "piece %zu has no bounding box\n", index);
                expect(false);
                continue;
            }
            const auto values = *box->as_int_array();
            if (values.size() != 6U) {
                std::fprintf(stderr, "piece %zu has a short bounding box\n",
                             index);
                expect(false);
                continue;
            }
            const bool bounds_match = actual.bounds.min_x == values[0] &&
                                      actual.bounds.min_y == values[1] &&
                                      actual.bounds.min_z == values[2] &&
                                      actual.bounds.max_x == values[3] &&
                                      actual.bounds.max_y == values[4] &&
                                      actual.bounds.max_z == values[5];
            const jint position_y = integer_field(expected, "PosY", 0);
            const bool position_match =
                actual.template_origin.x ==
                    integer_field(expected, "PosX", 0) &&
                actual.template_origin.y == position_y &&
                actual.template_origin.z == integer_field(expected, "PosZ", 0);
            const bool rotation_match =
                rotation_name(static_cast<structure::template_rotation>(
                    actual.template_rotation)) ==
                string_field(expected, "rotation", "NONE");
            const bool ground_match =
                actual.jigsaw.has_value() &&
                actual.jigsaw->ground_level_delta ==
                    integer_field(expected, "ground_level_delta", 0);
            const auto *element = child(expected, "pool_element");
            bool element_match = actual.jigsaw.has_value();
            if (element_match && element != nullptr &&
                element->as_compound() != nullptr) {
                const auto *entries = element->as_compound();
                const auto type = string_field(entries, "element_type", "");
                const auto location = string_field(entries, "location", "");
                const auto feature = string_field(entries, "feature", "");
                element_match =
                    actual.jigsaw->element.template_id == location &&
                    actual.jigsaw->element.feature == feature;
                const bool legacy =
                    type == "minecraft:legacy_single_pool_element";
                element_match =
                    element_match &&
                    (actual.jigsaw->element.kind ==
                     structure::pool_element_kind::legacy_single) == legacy;
            }
            if (!bounds_match || !position_match || !rotation_match ||
                !ground_match || !element_match) {
                const std::string rotation_text{
                    rotation_name(static_cast<structure::template_rotation>(
                        actual.template_rotation))};
                std::fprintf(stderr,
                             "piece %zu mismatch: bounds=%d pos=%d rotation=%d "
                             "ground=%d element=%d ref=[%d,%d,%d,%d,%d,%d] "
                             "pos=(%d,%d,%d) rot=%s\n",
                             index, bounds_match ? 1 : 0,
                             position_match ? 1 : 0, rotation_match ? 1 : 0,
                             ground_match ? 1 : 0, element_match ? 1 : 0,
                             values[0], values[1], values[2], values[3],
                             values[4], values[5], actual.template_origin.x,
                             actual.template_origin.y, actual.template_origin.z,
                             rotation_text.c_str());
                if (actual.jigsaw.has_value()) {
                    std::fprintf(stderr, "  template=%s feature=%s\n",
                                 actual.jigsaw->element.template_id.c_str(),
                                 actual.jigsaw->element.feature.c_str());
                }
            }
            expect(bounds_match);
            expect(position_match);
            expect(rotation_match);
            expect(ground_match);
            expect(element_match);
            if (!bounds_match || !position_match || !rotation_match ||
                !ground_match || !element_match) {
                return;
            }
        }
        digest expected_digest;
        digest actual_digest;
        for (const auto &piece : pieces->items) {
            expected_digest.mix(std::to_string(reference_piece_digest(piece)) +
                                ";");
        }
        for (const auto &piece : generated->pieces) {
            actual_digest.mix(std::to_string(generated_piece_digest(piece)) +
                              ";");
        }
        expect(expected_digest.value == actual_digest.value);
    };
};
