// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/content.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>

#include <boost/ut.hpp>

#include <string>

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/content"> worldgen_content_tests = [] {
    "the shipped snapshot materialises without the game directory"_test = [] {
        const std::string archive = std::string{REFRAIN_SOURCE_DIR} +
                                    "/content/vanilla/26.3/datapack.zip";
        const auto root = materialize_worldgen_data(archive);
        expect(root.has_value()) << "cannot materialise the snapshot";
        if (!root) {
            return;
        }
        const auto tags = load_block_tags(archive);
        expect(tags.has_value()) << "cannot load block tags";
        const auto structures = structure::load_structure_data(archive);
        expect(structures.has_value()) << "cannot load structure data";
        if (!structures) {
            return;
        }
        expect(structures->sets.size() >= 20U);
        const auto buried =
            structures->structures.find("minecraft:buried_treasure");
        expect(buried != structures->structures.end());
        if (buried != structures->structures.end()) {
            expect(structures->allows_biome(buried->second, "minecraft:beach"));
        }
        const auto settings = density::load_noise_settings(
            archive, "minecraft/overworld", algorithm::xoroshiro, 12345);
        expect(settings.has_value()) << "cannot load overworld noise settings";
        if (settings) {
            expect(settings->router.final_density != nullptr);
            expect(settings->sea_level == 63);
        }
    };

    "the manifest records the archive hash and entry count"_test = [] {
        const std::string directory =
            std::string{REFRAIN_SOURCE_DIR} + "/content/vanilla/26.3";
        auto manifest = refrain::data::json::value::parse_file(
            directory + "/MANIFEST.json");
        expect(manifest.has_value()) << "cannot read MANIFEST.json";
        if (!manifest) {
            return;
        }
        const auto *version = manifest->find("game_version");
        const auto *archive = manifest->find("archive");
        const auto *hash = manifest->find("archive_sha256");
        const auto *entries = manifest->find("entry_count");
        expect(version != nullptr && archive != nullptr && hash != nullptr &&
               entries != nullptr);
        if (version == nullptr || archive == nullptr || hash == nullptr ||
            entries == nullptr) {
            return;
        }
        expect(version->as_string() == "26.3");
        expect(archive->as_string() == "datapack.zip");
        expect(hash->as_string()->size() == 64U);
        expect(entries->as_int64().value_or(0) > 1000);
    };
};
