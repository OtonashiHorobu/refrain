// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Content generation snapshot: the published content view bound to the
// installed datapack root, the merged block tags it carries and the generators
// that capture it. The content-backed case needs the locally generated
// datapack snapshot and is skipped when the checkout has none.

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_view.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/dimension_generator.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace data = refrain::data;
namespace io = refrain::core::io;
namespace worldgen = refrain::worldgen;
using refrain::test_support::scratch_directory;

[[nodiscard]] auto write_file(const std::filesystem::path &root,
                              std::string_view relative, std::string_view text)
    -> bool {
    const auto target{root / std::filesystem::path{std::string{relative}}};
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(target.string(), text).has_value();
}

[[nodiscard]] auto read_file(const std::filesystem::path &path) -> std::string {
    auto text{io::read_text(path.string())};
    return text ? *text : std::string{};
}

[[nodiscard]] auto directory_source(const std::filesystem::path &root)
    -> data::content_source {
    return data::content_source{
        .kind = data::content_source_kind::directory,
        .path = root.string(),
        .label = root.string(),
    };
}

[[nodiscard]] auto
build_view(std::initializer_list<data::content_source> sources)
    -> std::shared_ptr<const data::content_view> {
    const std::vector<data::content_source> layers{sources};
    auto built{data::content_view::build(layers)};
    expect(static_cast<bool>(built)) << "content view build failed";
    if (!built) {
        return nullptr;
    }
    return std::make_shared<const data::content_view>(std::move(*built));
}

/// A stable fingerprint of one generated column: every palette key, packed
/// word and biome key of every section, plus the status.
[[nodiscard]] auto chunk_fingerprint(const refrain::world::chunk &chunk)
    -> data::digest32 {
    std::string text{fmt::format("y={} status={}", chunk.y_pos,
                                 static_cast<int>(chunk.status))};
    for (const auto &section : chunk.sections) {
        text += fmt::format("|s{} k{} b{} bc{}", section.y,
                            static_cast<int>(section.kind),
                            section.bits_per_entry, section.block_count);
        for (const auto &entry : section.palette) {
            text += ",";
            text += entry.key;
        }
        for (const auto word : section.data) {
            text += fmt::format(":{:x}", word);
        }
        text += fmt::format("|bk{} bb{}", static_cast<int>(section.biome_kind),
                            section.biome_bits_per_entry);
        for (const auto &biome : section.biome_palette) {
            text += ",";
            text += biome;
        }
        for (const auto word : section.biome_data) {
            text += fmt::format(":{:x}", word);
        }
    }
    return data::blake3(std::as_bytes(std::span{text.data(), text.size()}));
}

[[nodiscard]] auto generate_one(worldgen::chunk_generator &generator,
                                worldgen::chunk_pos origin)
    -> std::optional<data::digest32> {
    auto generated{generator.generate_tile(origin)};
    if (!generated || generated->empty() || (*generated)[0] == nullptr) {
        return std::nullopt;
    }
    auto column{refrain::world::from_proto_chunk(*(*generated)[0],
                                                 generator.biome_names())};
    if (!column) {
        return std::nullopt;
    }
    return chunk_fingerprint(*column);
}

/// The content root for the content-backed case: the environment override or
/// the locally generated snapshot. Prints one line and returns empty when the
/// checkout has no datapack.
[[nodiscard]] auto content_root() -> std::string {
    if (const auto *value{std::getenv("REFRAIN_CONTENT_DATA_ROOT")};
        value != nullptr && std::string_view{value}.size() > 0) {
        return std::string{value};
    }
    const auto root{std::string{REFRAIN_SOURCE_DIR} +
                    "/content/vanilla/26.3/data"};
    if (!std::filesystem::is_directory(root + "/minecraft/worldgen")) {
        std::fprintf(stderr, "generation data: datapack snapshot missing; set "
                             "REFRAIN_CONTENT_DATA_ROOT or run "
                             "scripts/fetch-content.sh <server.jar>\n");
        return {};
    }
    return root;
}

} // namespace

const suite generation_data_tests = [] {
    "generation data/a data-only view keeps the installed roots"_test = [] {
        scratch_directory scratch{};
        const auto base{scratch.path() / "base"};
        expect(write_file(base,
                          "minecraft/worldgen/noise_settings/overworld.json",
                          R"({"noise":{"min_y":-64,"height":384}})"));
        expect(write_file(base, "minecraft/tags/block/logs.json",
                          R"({"values":["minecraft:oak_log"]})"));
        const auto pack{scratch.path() / "pack"};
        expect(write_file(pack, "data/test/recipe/alpha.json",
                          R"({"type":"minecraft:crafting_shapeless"})"));
        auto view{build_view({directory_source(pack)})};
        expect(view != nullptr);
        if (!view) {
            return;
        }
        auto generation{worldgen::generation_data::build(view, nullptr, 5, 1,
                                                         base.string(), "")};
        expect(static_cast<bool>(generation));
        if (!generation) {
            return;
        }
        expect((*generation)->view_generation() == 5U);
        expect((*generation)->registry_generation() == 1U);
        // Nothing chunk generation reads changed: the installed root is used
        // as it is, so the chunks stay byte-identical to the boot generator.
        expect(!(*generation)->overlays_generation_content());
        expect((*generation)->data_root() == base.string());
        expect((*generation)->templates_root().empty());
        expect((*generation)->block_tags().has("minecraft:logs"));
        expect((*generation)->registries().empty());
    };

    "generation data/overlays the view on the installed datapack"_test = [] {
        scratch_directory scratch{};
        const auto base{scratch.path() / "base"};
        const auto templates{scratch.path() / "templates"};
        expect(write_file(base, "minecraft/worldgen/biome/plains.json",
                          R"({"temperature":0.5})"));
        expect(write_file(base,
                          "minecraft/worldgen/noise_settings/overworld.json",
                          R"({"note":"base-noise"})"));
        expect(write_file(base, "minecraft/tags/block/logs.json",
                          R"({"values":["minecraft:oak_log"]})"));
        expect(write_file(templates, "minecraft/structure/hut.nbt",
                          "base-template"));
        expect(write_file(templates, "minecraft/structure/keep.nbt",
                          "kept-template"));
        const auto low{scratch.path() / "low"};
        expect(write_file(low, "data/minecraft/worldgen/biome/plains.json",
                          R"({"temperature":0.6})"));
        expect(write_file(low, "data/minecraft/tags/block/logs.json",
                          R"({"values":["minecraft:birch_log"]})"));
        expect(write_file(low, "data/test/recipe/alpha.json",
                          R"({"type":"minecraft:crafting_shapeless"})"));
        const auto high{scratch.path() / "high"};
        expect(write_file(high, "data/minecraft/worldgen/biome/plains.json",
                          R"({"temperature":0.7})"));
        auto view{build_view({directory_source(low), directory_source(high)})};
        expect(view != nullptr);
        if (!view) {
            return;
        }
        std::string root{};
        {
            auto generation{worldgen::generation_data::build(
                view, nullptr, 2, 1, base.string(), templates.string())};
            expect(static_cast<bool>(generation));
            if (!generation) {
                return;
            }
            expect((*generation)->overlays_generation_content());
            expect((*generation)->view_generation() == 2U);
            root = (*generation)->data_root();
            expect(root != base.string());
            expect((*generation)->templates_root() == root);
            // Highest-priority layer wins in the merged tree; the untouched
            // base documents stay available under the same root.
            expect(read_file(std::filesystem::path{root} /
                             "minecraft/worldgen/biome/plains.json") ==
                   R"({"temperature":0.7})");
            expect(
                read_file(std::filesystem::path{root} /
                          "minecraft/worldgen/noise_settings/overworld.json") ==
                R"({"note":"base-noise"})");
            // The installed structure templates are merged into the same root,
            // so the generator that reads templates from it still finds them.
            expect(read_file(std::filesystem::path{root} /
                             "minecraft/structure/hut.nbt") == "base-template");
            expect(read_file(std::filesystem::path{root} /
                             "minecraft/structure/keep.nbt") ==
                   "kept-template");
            // The installed block tag keeps its values and the view's merged
            // tag values are appended on top.
            expect((*generation)->block_tags().has("minecraft:logs"));
            const auto resolved{
                (*generation)->block_tags().resolve("minecraft:logs")};
            expect(static_cast<bool>(resolved));
            if (resolved) {
                expect(std::ranges::find(*resolved,
                                         std::string{"minecraft:oak_log"}) !=
                       resolved->end());
                expect(std::ranges::find(*resolved,
                                         std::string{"minecraft:birch_log"}) !=
                       resolved->end());
            }
        }
        // The materialized tree is removed once the last generator that holds
        // the generation releases it.
        expect(!std::filesystem::is_directory(root));
    };

    "generation data/merges every tag kind with the datapack rules"_test = [] {
        scratch_directory scratch{};
        const auto base{scratch.path() / "base"};
        expect(write_file(base,
                          "minecraft/worldgen/noise_settings/overworld.json",
                          R"({"note":"base"})"));
        expect(write_file(base, "minecraft/tags/block/logs.json",
                          R"({"values":["minecraft:oak_log"]})"));
        expect(write_file(base, "minecraft/tags/block/leaves.json",
                          R"({"values":["minecraft:oak_leaves"]})"));
        expect(write_file(base, "minecraft/tags/worldgen/biome/temperate.json",
                          R"({"values":["minecraft:plains"]})"));
        expect(write_file(base, "minecraft/tags/worldgen/biome/frozen.json",
                          R"({"values":["minecraft:snowy_plains"]})"));
        const auto low{scratch.path() / "low"};
        expect(write_file(low, "data/minecraft/tags/block/logs.json",
                          R"({"values":["minecraft:birch_log"]})"));
        expect(write_file(low,
                          "data/minecraft/tags/worldgen/biome/temperate.json",
                          R"({"values":["minecraft:forest"]})"));
        const auto high{scratch.path() / "high"};
        expect(write_file(
            high, "data/minecraft/tags/block/leaves.json",
            R"({"replace":true,"values":["minecraft:birch_leaves"]})"));
        expect(write_file(high,
                          "data/minecraft/tags/worldgen/biome/temperate.json",
                          R"({"values":["minecraft:taiga"]})"));
        expect(write_file(
            high, "data/minecraft/tags/worldgen/biome/frozen.json",
            R"({"replace":true,"values":["minecraft:ice_spikes"]})"));
        auto view{build_view({directory_source(low), directory_source(high)})};
        expect(view != nullptr);
        if (!view) {
            return;
        }
        auto generation{worldgen::generation_data::build(view, nullptr, 2, 1,
                                                         base.string(), "")};
        expect(static_cast<bool>(generation));
        if (!generation) {
            return;
        }
        expect((*generation)->overlays_generation_content());
        const auto root{(*generation)->data_root()};
        const auto logs{read_file(std::filesystem::path{root} /
                                  "minecraft/tags/block/logs.json")};
        expect(logs.find("minecraft:oak_log") != std::string::npos);
        expect(logs.find("minecraft:birch_log") != std::string::npos);
        // An installed value precedes the view's appended value.
        expect(logs.find("minecraft:oak_log") <
               logs.find("minecraft:birch_log"));
        // replace=true clears the installed values of that tag.
        const auto leaves{read_file(std::filesystem::path{root} /
                                    "minecraft/tags/block/leaves.json")};
        expect(leaves.find("minecraft:birch_leaves") != std::string::npos);
        expect(leaves.find("minecraft:oak_leaves") == std::string::npos);
        // A non-block tag keeps the installed values unless a view layer
        // replaces it.
        const auto temperate{
            read_file(std::filesystem::path{root} /
                      "minecraft/tags/worldgen/biome/temperate.json")};
        expect(temperate.find("minecraft:plains") != std::string::npos);
        expect(temperate.find("minecraft:forest") != std::string::npos);
        expect(temperate.find("minecraft:taiga") != std::string::npos);
        expect(temperate.find("minecraft:plains") <
               temperate.find("minecraft:forest"));
        expect(temperate.find("minecraft:forest") <
               temperate.find("minecraft:taiga"));
        const auto frozen{
            read_file(std::filesystem::path{root} /
                      "minecraft/tags/worldgen/biome/frozen.json")};
        expect(frozen.find("minecraft:ice_spikes") != std::string::npos);
        expect(frozen.find("minecraft:snowy_plains") == std::string::npos);
        // The block tag table the generation applies follows the merged tree.
        const auto resolved{
            (*generation)->block_tags().resolve("minecraft:leaves")};
        expect(static_cast<bool>(resolved));
        if (resolved) {
            expect(std::ranges::find(*resolved,
                                     std::string{"minecraft:birch_leaves"}) !=
                   resolved->end());
            expect(std::ranges::find(*resolved,
                                     std::string{"minecraft:oak_leaves"}) ==
                   resolved->end());
        }
        expect(worldgen::materialization_cleanup_failures() == 0U);
    };

    "generation data/is immutable and deterministic for one view"_test = [] {
        scratch_directory scratch{};
        const auto base{scratch.path() / "base"};
        expect(write_file(base,
                          "minecraft/worldgen/noise_settings/overworld.json",
                          R"({"note":"base"})"));
        expect(write_file(base, "minecraft/tags/block/logs.json",
                          R"({"values":["minecraft:oak_log"]})"));
        const auto pack{scratch.path() / "pack"};
        expect(write_file(pack, "data/minecraft/worldgen/biome/plains.json",
                          R"({"temperature":0.8})"));
        expect(write_file(pack, "data/test/recipe/alpha.json",
                          R"({"type":"minecraft:crafting_shapeless"})"));
        auto view{build_view({directory_source(pack)})};
        expect(view != nullptr);
        if (!view) {
            return;
        }
        auto first{worldgen::generation_data::build(view, nullptr, 3, 2,
                                                    base.string(), "")};
        auto second{worldgen::generation_data::build(view, nullptr, 3, 2,
                                                     base.string(), "")};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        expect((*first)->data_root() != (*second)->data_root());
        expect(read_file(std::filesystem::path{(*first)->data_root()} /
                         "minecraft/worldgen/biome/plains.json") ==
               read_file(std::filesystem::path{(*second)->data_root()} /
                         "minecraft/worldgen/biome/plains.json"));
        expect((*first)->registry_generation() == 2U);
    };

    "generation data/requires a published view"_test = [] {
        auto generation{
            worldgen::generation_data::build(nullptr, nullptr, 1, 1, "", "")};
        expect(!generation);
        if (!generation) {
            expect(generation.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "generation data/binds a generator to the reloaded worldgen data"_test =
        [] {
            const auto base{content_root()};
            if (base.empty()) {
                return;
            }
            const auto settings_path{
                std::filesystem::path{base} /
                "minecraft/worldgen/noise_settings/overworld.json"};
            auto settings{io::read_text(settings_path.string())};
            expect(static_cast<bool>(settings));
            if (!settings) {
                return;
            }
            constexpr std::string_view stone{
                R"("default_block": "minecraft:stone")"};
            constexpr std::string_view glass{
                R"("default_block": "minecraft:glass")"};
            if (settings->find(stone) == std::string::npos) {
                std::fprintf(stderr,
                             "generation data: the datapack snapshot has no "
                             "overworld default block to override\n");
                return;
            }
            scratch_directory scratch{};
            const auto pack{scratch.path() / "pack"};
            auto changed{*settings};
            changed.replace(changed.find(stone), stone.size(), glass);
            expect(write_file(pack,
                              "data/minecraft/worldgen/noise_settings/"
                              "overworld.json",
                              changed));
            auto view{build_view({directory_source(pack)})};
            expect(view != nullptr);
            if (!view) {
                return;
            }
            const auto boot_config{worldgen::chunk_generator_config{
                .data_root = base,
                .seed = 1,
                .kind = worldgen::dimension_kind::overworld,
                .target = worldgen::generation_target::terrain,
            }};
            auto boot{worldgen::chunk_generator::create(boot_config)};
            expect(static_cast<bool>(boot)) << "boot generator creation failed";
            if (!boot) {
                return;
            }
            const auto boot_fingerprint{
                generate_one(**boot, worldgen::chunk_pos{.x = 0, .z = 0})};
            expect(boot_fingerprint.has_value());
            if (!boot_fingerprint) {
                return;
            }
            // A published generation over the same root carries the changed
            // noise settings.
            auto generation{worldgen::generation_data::build(view, nullptr, 2,
                                                             1, base, "")};
            expect(static_cast<bool>(generation));
            if (!generation) {
                return;
            }
            expect((*generation)->overlays_generation_content());
            auto bound_config{boot_config};
            bound_config.generation = *generation;
            auto bound{worldgen::chunk_generator::create(bound_config)};
            expect(static_cast<bool>(bound));
            if (!bound) {
                return;
            }
            expect((*bound)->view_generation() == 2U);
            expect((*bound)->registry_generation() == 1U);
            const auto bound_fingerprint{
                generate_one(**bound, worldgen::chunk_pos{.x = 0, .z = 0})};
            expect(bound_fingerprint.has_value());
            if (!bound_fingerprint) {
                return;
            }
            // The new chunk uses the reloaded document; the chunk the boot
            // generator produced before the publish is unchanged.
            expect(*bound_fingerprint != *boot_fingerprint);
            const auto boot_again{
                generate_one(**boot, worldgen::chunk_pos{.x = 0, .z = 0})};
            expect(boot_again.has_value());
            if (boot_again) {
                expect(*boot_again == *boot_fingerprint);
            }
            // A data-only generation (no worldgen document) reproduces the
            // boot chunk exactly.
            const auto data_pack{scratch.path() / "data-only"};
            expect(write_file(data_pack, "data/test/recipe/alpha.json",
                              R"({"type":"minecraft:crafting_shapeless"})"));
            auto data_view{build_view({directory_source(data_pack)})};
            expect(data_view != nullptr);
            if (!data_view) {
                return;
            }
            auto data_generation{worldgen::generation_data::build(
                data_view, nullptr, 3, 2, base, "")};
            expect(static_cast<bool>(data_generation));
            if (!data_generation) {
                return;
            }
            expect(!(*data_generation)->overlays_generation_content());
            auto data_config{boot_config};
            data_config.generation = *data_generation;
            auto data_bound{worldgen::chunk_generator::create(data_config)};
            expect(static_cast<bool>(data_bound));
            if (!data_bound) {
                return;
            }
            const auto data_fingerprint{generate_one(
                **data_bound, worldgen::chunk_pos{.x = 0, .z = 0})};
            expect(data_fingerprint.has_value());
            if (data_fingerprint) {
                expect(*data_fingerprint == *boot_fingerprint);
            }
            // A registry document that keeps its entry name and only changes
            // its content digest is not generation-observable: the mapping
            // fingerprint changes, the generated chunk does not.
            const auto digest_one{scratch.path() / "digest-one"};
            expect(write_file(
                digest_one,
                "data/minecraft/worldgen/noise_settings/overworld.json",
                changed));
            expect(write_file(digest_one, "data/test/block/ruby.json",
                              R"({"type":"minecraft:block","note":"v1"})"));
            auto digest_one_view{build_view({directory_source(digest_one)})};
            expect(digest_one_view != nullptr);
            if (!digest_one_view) {
                return;
            }
            const auto digest_two{scratch.path() / "digest-two"};
            expect(write_file(
                digest_two,
                "data/minecraft/worldgen/noise_settings/overworld.json",
                changed));
            expect(write_file(digest_two, "data/test/block/ruby.json",
                              R"({"type":"minecraft:block","note":"v2"})"));
            auto digest_two_view{build_view({directory_source(digest_two)})};
            expect(digest_two_view != nullptr);
            if (!digest_two_view) {
                return;
            }
            expect(digest_one_view->registries().fingerprint() !=
                   digest_two_view->registries().fingerprint());
            const auto digest_change{data::classify_registry_change(
                *digest_one_view, *digest_two_view)};
            expect(digest_change.kind == data::registry_change_kind::documents);
            expect(!digest_change.entry_names_changed());
            std::array<std::optional<data::digest32>, 2> fingerprints{};
            const std::array<std::shared_ptr<const data::content_view>, 2>
                views{digest_one_view, digest_two_view};
            for (std::size_t index{0}; index < views.size(); ++index) {
                auto digest_generation{worldgen::generation_data::build(
                    views[index], nullptr, 3U + index, 2U, base, "")};
                expect(static_cast<bool>(digest_generation));
                if (!digest_generation) {
                    return;
                }
                auto digest_config{boot_config};
                digest_config.generation = *digest_generation;
                auto digest_bound{
                    worldgen::chunk_generator::create(digest_config)};
                expect(static_cast<bool>(digest_bound));
                if (!digest_bound) {
                    return;
                }
                fingerprints[index] = generate_one(
                    **digest_bound, worldgen::chunk_pos{.x = 0, .z = 0});
            }
            expect(fingerprints[0].has_value());
            expect(fingerprints[1].has_value());
            if (fingerprints[0] && fingerprints[1]) {
                expect(*fingerprints[0] == *fingerprints[1]);
            }
        };

    "generation data/the experimental scheduler refuses a bound generation"_test =
        [] {
            scratch_directory scratch{};
            const auto pack{scratch.path() / "pack"};
            expect(write_file(pack, "data/test/recipe/alpha.json",
                              R"({"type":"minecraft:crafting_shapeless"})"));
            auto view{build_view({directory_source(pack)})};
            expect(view != nullptr);
            if (!view) {
                return;
            }
            auto generation{
                worldgen::generation_data::build(view, nullptr, 2, 1, "", "")};
            expect(static_cast<bool>(generation));
            if (!generation) {
                return;
            }
            auto config{worldgen::chunk_generator_config{
                .data_root = "missing",
                .seed = 1,
                .kind = worldgen::dimension_kind::overworld,
                .target = worldgen::generation_target::terrain,
            }};
            config.generation = *generation;
            auto scheduler{worldgen::chunk_scheduler::create(
                worldgen::chunk_scheduler::config{
                    .generator = std::move(config), .workers = 1})};
            expect(!scheduler);
            if (!scheduler) {
                expect(scheduler.error().code ==
                       refrain::error_code::unsupported);
            }
        };
};
