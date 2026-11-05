// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/model.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace world = refrain::world;

using world::block_state_key;
using world::chunk;
using world::chunk_pos;
using world::chunk_status;
using world::heightmap_record;
using world::light_section;
using world::palette_entry;
using world::section_kind;
using world::section_record;

[[nodiscard]] auto make_single_section(std::int8_t y) -> section_record {
    section_record section{};
    section.y = y;
    section.kind = section_kind::single_value;
    section.bits_per_entry = 0;
    section.palette.push_back(
        palette_entry{.key = "minecraft:air", .count = 4096, .state = 0});
    section.biome_kind = section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    return section;
}

/// Palette section with `entries` states and deterministic packed data.
[[nodiscard]] auto make_palette_section(std::int8_t y, std::size_t entries)
    -> section_record {
    section_record section{};
    section.y = y;
    section.kind = section_kind::palette;
    section.bits_per_entry = world::bits_for_palette(entries);
    for (std::size_t index{0}; index < entries; ++index) {
        section.palette.push_back(palette_entry{
            .key = "minecraft:test_block_" + std::to_string(index),
            .count = static_cast<std::uint16_t>(index % 4096),
            .state = static_cast<std::uint32_t>(index)});
    }
    std::vector<std::uint32_t> indices(world::section_volume);
    for (std::size_t index{0}; index < indices.size(); ++index) {
        indices[index] = static_cast<std::uint32_t>(index % entries);
    }
    expect(static_cast<bool>(world::pack_palette_indices(
        indices, section.bits_per_entry, entries, section.data)));
    section.biome_kind = section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    return section;
}

[[nodiscard]] auto make_light(std::int8_t y) -> light_section {
    light_section light{};
    light.y = y;
    light.sky_light.assign(world::light_nibble_bytes, std::byte{0x0F});
    return light;
}

[[nodiscard]] auto make_valid_chunk() -> chunk {
    chunk value{};
    value.format_version = world::chunk_format_version;
    value.status = chunk_status::full;
    value.y_pos = -4;
    value.last_update = 42;
    value.inhabited_time = 9001;
    value.sections.push_back(make_single_section(-4));
    value.sections.push_back(make_palette_section(0, 17));
    heightmap_record heightmap{};
    heightmap.name = "MOTION_BLOCKING";
    heightmap.bits_per_entry = 9;
    heightmap.data.assign(37, 0);
    value.heightmaps.push_back(std::move(heightmap));
    value.post_processing.resize(value.sections.size());
    return value;
}

} // namespace

suite world_model = [] {
    "chunk_pos_key_and_format"_test = [] {
        const chunk_pos pos{.x = -13, .z = 7};
        expect(pos.key() == "-13.7");
        expect(world::format_chunk_pos(pos) == "(-13, 7)");
        const auto parsed{world::parse_chunk_key("-13.7")};
        expect(static_cast<bool>(parsed));
        expect(*parsed == pos);
    };

    "chunk_pos_parse_rejects_malformed"_test = [] {
        for (const std::string_view text :
             {"", ".", "1", "1.", ".2", "1.2.3", "+1.2", "1.2x", "x.2"}) {
            const auto parsed{world::parse_chunk_key(text)};
            expect(!parsed) << "accepted '" << text << "'";
            if (!parsed) {
                expect(parsed.error().code == refrain::error_code::parse_error);
            }
        }
        for (const std::string_view text :
             {"0.0", "-1.0", "2147483647.-2147483648"}) {
            expect(static_cast<bool>(world::parse_chunk_key(text)))
                << "rejected '" << text << "'";
        }
    };

    "block_state_key_plain"_test = [] {
        const auto key{world::parse_block_state_key("minecraft:stone")};
        expect(static_cast<bool>(key));
        expect(key->name == "minecraft:stone");
        expect(key->properties.empty());
        expect(world::format_block_state_key(*key) == "minecraft:stone");
    };

    "block_state_key_sorts_properties"_test = [] {
        const auto key{world::parse_block_state_key(
            "minecraft:oak_stairs[facing=north,half=top,waterlogged=true]")};
        expect(static_cast<bool>(key));
        expect(key->properties.size() == 3U);
        expect(key->properties[0].first == "facing");
        expect(key->properties[1].first == "half");
        expect(key->properties[2].first == "waterlogged");
        expect(world::format_block_state_key(*key) ==
               "minecraft:oak_stairs[facing=north,half=top,waterlogged=true]");
    };

    "block_state_key_canonicalises_input"_test = [] {
        const auto canonical = world::canonical_block_state_key(
            "minecraft:oak_log",
            std::vector<std::pair<std::string, std::string>>{{"axis", "y"}});
        expect(static_cast<bool>(canonical));
        expect(*canonical == "minecraft:oak_log[axis=y]");
        const auto parsed{world::parse_block_state_key(*canonical)};
        expect(static_cast<bool>(parsed));
    };

    "block_state_key_rejects_bad_input"_test = [] {
        for (const std::string_view text :
             {"", "minecraft:stone[", "minecraft:stone[axis=y",
              "minecraft:stone[]", "minecraft:stone[=y]", "minecraft:stone[a]",
              "minecraft:stone[a=]", "minecraft:stone[a=1,a=2]",
              "minecraft:stone[a=1]x", "mine[craft:stone", "minecraft stone"}) {
            const auto key{world::parse_block_state_key(text)};
            expect(!key) << "accepted '" << text << "'";
        }
        // Whitespace around properties is trimmed.
        const auto spaced{
            world::parse_block_state_key("minecraft:stone[a = 1 , b=2]")};
        expect(static_cast<bool>(spaced));
        expect(world::format_block_state_key(*spaced) ==
               "minecraft:stone[a=1,b=2]");
    };

    "bits_for_palette_matches_vanilla"_test = [] {
        expect(world::bits_for_palette(0) == 0);
        expect(world::bits_for_palette(1) == 0);
        expect(world::bits_for_palette(2) == 1);
        expect(world::bits_for_palette(3) == 2);
        expect(world::bits_for_palette(4) == 2);
        expect(world::bits_for_palette(5) == 3);
        expect(world::bits_for_palette(8) == 3);
        expect(world::bits_for_palette(9) == 4);
        expect(world::bits_for_palette(16) == 4);
        expect(world::bits_for_palette(17) == 5);
        expect(world::bits_for_palette(32) == 5);
        expect(world::bits_for_palette(33) == 6);
        expect(world::bits_for_palette(4096) == 12);
    };

    "packed_word_count_padded_layout"_test = [] {
        expect(world::packed_word_count(0, 4096) == 0U);
        expect(world::packed_word_count(1, 4096) == 64U);
        expect(world::packed_word_count(2, 4096) == 128U);
        expect(world::packed_word_count(5, 4096) == 342U);
        expect(world::packed_word_count(9, 256) == 37U);
        expect(world::packed_word_count(32, 4096) == 2048U);
        expect(world::packed_word_count(33, 4096) == 0U);
    };

    "palette_roundtrip_all_widths"_test = [] {
        for (const std::uint8_t bits :
             {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 32}) {
            const auto palette_size{std::size_t{1} << (bits == 32 ? 12 : bits)};
            std::vector<std::uint32_t> indices(world::section_volume);
            for (std::size_t index{0}; index < indices.size(); ++index) {
                indices[index] =
                    static_cast<std::uint32_t>(index % palette_size);
            }
            std::vector<std::uint64_t> words;
            const auto packed{world::pack_palette_indices(indices, bits,
                                                          palette_size, words)};
            expect(static_cast<bool>(packed)) << "bits=" << bits;
            expect(words.size() ==
                   world::packed_word_count(bits, indices.size()));
            std::vector<std::uint32_t> unpacked(indices.size());
            const auto decoded{world::unpack_palette_indices(
                words, bits, indices.size(), palette_size, unpacked)};
            expect(static_cast<bool>(decoded)) << "bits=" << bits;
            expect(unpacked == indices) << "bits=" << bits;
        }
    };

    "palette_pack_rejects_out_of_range"_test = [] {
        std::vector<std::uint32_t> indices(world::section_volume, 0);
        indices[17] = 5;
        std::vector<std::uint64_t> words;
        const auto packed{world::pack_palette_indices(indices, 2, 4, words)};
        expect(!packed);
        expect(packed.error().code == refrain::error_code::parse_error);
    };

    "palette_unpack_rejects_corrupt"_test = [] {
        std::vector<std::uint64_t> words(128, 0);
        words[1] = 3; // index 32 = 3, outside a 3-entry palette
        std::vector<std::uint32_t> out(world::section_volume);
        const auto decoded{world::unpack_palette_indices(
            words, 2, world::section_volume, 3, out)};
        expect(!decoded);
        expect(decoded.error().code == refrain::error_code::parse_error);

        std::vector<std::uint64_t> truncated(2, 0);
        const auto bad_words{world::unpack_palette_indices(
            truncated, 2, world::section_volume, 3, out)};
        expect(!bad_words);

        const auto zero_bits{world::unpack_palette_indices(
            {}, 0, world::section_volume, 3, out)};
        expect(!zero_bits);
        expect(zero_bits.error().code == refrain::error_code::invalid_argument);
    };

    "validate_accepts_minimal_chunk"_test = [] {
        const auto value{make_valid_chunk()};
        expect(static_cast<bool>(world::validate(value)));
    };

    "validate_rejects_version_and_status"_test = [] {
        auto value{make_valid_chunk()};
        value.format_version =
            static_cast<std::uint16_t>(world::chunk_format_version + 1);
        expect(!world::validate(value));
        value = make_valid_chunk();
        value.status = static_cast<chunk_status>(200);
        expect(!world::validate(value));
    };

    "validate_accepts_light_layers"_test = [] {
        auto value{make_valid_chunk()};
        value.light_sections.push_back(make_light(-5));
        value.light_sections.push_back(make_light(-4));
        value.light_sections.push_back(make_light(0));
        value.light_sections.push_back(make_light(1));
        expect(static_cast<bool>(world::validate(value)));
    };

    "validate_rejects_light_layer_problems"_test = [] {
        auto below{make_valid_chunk()};
        below.light_sections.push_back(make_light(-6));
        expect(!world::validate(below));

        auto above{make_valid_chunk()};
        above.light_sections.push_back(make_light(2));
        expect(!world::validate(above));

        auto gap{make_valid_chunk()};
        gap.light_sections.push_back(make_light(-3)); // between -4 and 0
        expect(!world::validate(gap));

        auto duplicate{make_valid_chunk()};
        duplicate.light_sections.push_back(make_light(-5));
        duplicate.light_sections.push_back(make_light(-5));
        expect(!world::validate(duplicate));

        auto unordered{make_valid_chunk()};
        unordered.light_sections.push_back(make_light(0));
        unordered.light_sections.push_back(make_light(-5));
        expect(!world::validate(unordered));

        auto empty{make_valid_chunk()};
        auto no_light{make_light(-5)};
        no_light.sky_light.clear();
        empty.light_sections.push_back(std::move(no_light));
        expect(!world::validate(empty));

        auto bad_size{make_valid_chunk()};
        auto light{make_light(-5)};
        light.sky_light.assign(10, std::byte{0});
        bad_size.light_sections.push_back(std::move(light));
        expect(!world::validate(bad_size));

        chunk no_sections{};
        no_sections.format_version = world::chunk_format_version;
        no_sections.status = chunk_status::full;
        no_sections.y_pos = -4;
        no_sections.light_sections.push_back(make_light(-5));
        expect(!world::validate(no_sections));
    };

    "validate_rejects_section_problems"_test = [] {
        auto value{make_valid_chunk()};
        value.sections[1].y = -4; // duplicate / out of order
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.sections[0].data.push_back(0); // single-value section with data
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.sections[1].bits_per_entry = 2; // 17 entries need 5 bits
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.sections[1].data.pop_back(); // wrong packed word count
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.sections[1].palette.back().key = "minecraft:oak_log[b=2,a=1]";
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.light_sections.push_back(make_light(-5));
        value.light_sections.back().sky_light.assign(10, std::byte{0});
        expect(!world::validate(value));
    };

    "validate_rejects_heightmap_and_post_problems"_test = [] {
        auto value{make_valid_chunk()};
        value.heightmaps[0].data.pop_back();
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.heightmaps.push_back(value.heightmaps.front());
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.post_processing.resize(24);
        expect(static_cast<bool>(world::validate(value)));

        value = make_valid_chunk();
        value.post_processing.assign(world::max_sections_per_chunk + 1, {});
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.heightmaps[0].bits_per_entry = 0;
        expect(!world::validate(value));
    };

    "validate_enforces_collection_limits"_test = [] {
        auto value{make_valid_chunk()};
        value.sections.resize(world::max_sections_per_chunk + 1,
                              make_single_section(0));
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.block_entities.resize(world::max_block_entities + 1);
        expect(!world::validate(value));

        value = make_valid_chunk();
        value.extra.assign(world::max_payload_bytes + 1, std::byte{0});
        expect(!world::validate(value));
    };

    "section_lookup_and_status_names"_test = [] {
        const auto value{make_valid_chunk()};
        expect(world::section_at(value, -4) != nullptr);
        expect(world::section_at(value, 0) != nullptr);
        expect(world::section_at(value, 1) == nullptr);

        for (const auto status :
             {chunk_status::empty, chunk_status::terrain,
              chunk_status::initialize_light, chunk_status::full}) {
            const auto name{world::chunk_status_name(status)};
            const auto parsed{world::parse_chunk_status(name)};
            expect(static_cast<bool>(parsed));
            expect(*parsed == status);
        }
        const auto prefixed{world::parse_chunk_status("minecraft:full")};
        expect(static_cast<bool>(prefixed));
        expect(*prefixed == chunk_status::full);
        expect(!world::parse_chunk_status("minecraft:generating"));
        expect(!world::parse_chunk_status(""));
    };
};
