// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/data/vfs.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace fs = std::filesystem;
namespace nbt = refrain::data::nbt;
using refrain::data::content_entry;
using refrain::data::content_kind;
using refrain::data::datapack_load_options;
using refrain::data::datapack_set;
using refrain::data::datapack_stats;
using refrain::data::layer_info;
using refrain::data::pack;
using refrain::data::pack_format;
using refrain::data::pack_source;
using refrain::data::resource_location;
using refrain::data::tag_record;
using refrain::data::tag_set;
using refrain::data::vfs;

// ---------------------------------------------------------------------------
// scratch directories / pack building
// ---------------------------------------------------------------------------

[[nodiscard]] auto make_workspace_path(std::string_view name) -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_datapack_test_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++) + "_" + std::string{name})};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

struct temp_workspace {
    fs::path root{};

    explicit temp_workspace(std::string_view name)
        : root{make_workspace_path(name)} {}
    temp_workspace(const temp_workspace &) = delete;
    auto operator=(const temp_workspace &) -> temp_workspace & = delete;
    ~temp_workspace() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    [[nodiscard]] auto path(std::string_view relative) const -> fs::path {
        return root / fs::path{std::string{relative}};
    }

    auto write(std::string_view relative, std::string_view content) const
        -> bool {
        const auto target{path(relative)};
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return false;
        }
        return refrain::core::io::write_text(target.string(), content)
            .has_value();
    }

    auto write_bytes(std::string_view relative,
                     std::span<const std::byte> data) const -> bool {
        const auto target{path(relative)};
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return false;
        }
        return refrain::core::io::write_file(target.string(), data).has_value();
    }
};

[[nodiscard]] auto open_layer(const fs::path &path, std::string_view label)
    -> std::unique_ptr<pack> {
    auto opened = refrain::data::open_dir_pack(path.string(), label);
    expect(static_cast<bool>(opened))
        << "open_dir_pack failed for " << path.string();
    return opened ? std::move(*opened) : nullptr;
}

/// Load explicitly layered packs (low -> high priority). Null entries are
/// rejected by the loader; tests that want to observe that pass null directly.
[[nodiscard]] auto load_stack(std::vector<std::unique_ptr<pack>> &layers,
                              datapack_load_options options = {})
    -> refrain::result<datapack_set> {
    std::vector<const pack *> pointers;
    pointers.reserve(layers.size());
    for (const auto &layer : layers) {
        pointers.push_back(layer.get());
    }
    return datapack_set::load(
        std::span<const pack *const>{pointers.data(), pointers.size()},
        options);
}

template <typename Value>
[[nodiscard]] auto error_code_of(const refrain::result<Value> &value) -> int {
    return value.has_value() ? -1 : static_cast<int>(value.error().code);
}

/// Owns the opened packs and hands the loader a layer list (low -> high).
template <typename... Layers>
[[nodiscard]] auto stack_of(Layers... layers)
    -> std::vector<std::unique_ptr<pack>> {
    std::vector<std::unique_ptr<pack>> storage;
    storage.reserve(sizeof...(Layers));
    (storage.push_back(std::move(layers)), ...);
    return storage;
}

[[nodiscard]] auto text_of(const std::vector<std::string> &lines)
    -> std::string {
    std::string text;
    for (const auto &line : lines) {
        text += line;
        text += '\n';
    }
    return text;
}

[[nodiscard]] auto contains(const std::vector<std::string> &lines,
                            std::string_view needle) -> bool {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) {
                           return line.find(needle) != std::string::npos;
                       });
}

[[nodiscard]] auto find_tag(const datapack_set &set, std::string_view type,
                            std::string_view id) -> const tag_record * {
    auto location = resource_location::parse(id);
    expect(static_cast<bool>(location)) << "invalid tag id " << id;
    return location ? set.tags().find(type, *location) : nullptr;
}

[[nodiscard]] auto find_entry(const datapack_set &set, content_kind kind,
                              std::string_view id) -> const content_entry * {
    auto location = resource_location::parse(id);
    expect(static_cast<bool>(location)) << "invalid content id " << id;
    return location ? set.find(kind, *location) : nullptr;
}

// Common JSON snippets (all ids are refrain_test:*; no Mojang content).
constexpr std::string_view recipe_shaped{
    R"({"type":"minecraft:crafting_shaped"})"};
constexpr std::string_view recipe_smelting{R"({"type":"minecraft:smelting"})"};
constexpr std::string_view loot_block{R"({"type":"minecraft:block"})"};
constexpr std::string_view worldgen_biome{R"({"type":"minecraft:overworld"})"};
constexpr std::string_view advancement_ok{
    R"({"criteria":{"tick":{"trigger":"minecraft:tick"}}})"};
constexpr std::string_view manifest_format_121{
    R"({"pack":{"pack_format":121,"description":"test pack"}})"};

// ---------------------------------------------------------------------------
// manifest
// ---------------------------------------------------------------------------

const suite datapack_manifest_tests = [] {
    "datapack/manifest parsed and pack_format exposed"_test = [] {
        temp_workspace work{"manifest"};
        expect(work.write("pack_a/pack.mcmeta", manifest_format_121));
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(loaded->layer_count() == 1U);
        const auto &layer = loaded->stats().layers.at(0);
        expect(layer.has_manifest);
        expect(layer.manifest_pack_format == 121U);
        expect(layer.label == "pack-a");
        expect(layer.format == pack_format::directory);
    };

    "datapack/missing pack.mcmeta is allowed"_test = [] {
        temp_workspace work{"no-manifest"};
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "raw-data"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (loaded) {
            expect(!loaded->stats().layers.at(0).has_manifest);
        }
    };

    "datapack/invalid manifest JSON reports pack and path"_test = [] {
        temp_workspace work{"bad-manifest"};
        expect(work.write("pack_a/pack.mcmeta", R"({"pack":{)"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "broken-pack"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(error_code_of(loaded) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(loaded.error().message.find("broken-pack") !=
                   std::string::npos);
            expect(loaded.error().message.find("pack.mcmeta") !=
                   std::string::npos);
        }
    };

    "datapack/manifest missing pack_format is rejected"_test = [] {
        temp_workspace work{"manifest-no-format"};
        expect(work.write("pack_a/pack.mcmeta",
                          R"({"pack":{"description":"x"}})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().message.find("pack.pack_format") !=
                   std::string::npos);
        }
    };

    "datapack/manifest non-positive pack_format is rejected"_test = [] {
        temp_workspace work{"manifest-negative"};
        expect(
            work.write("pack_a/pack.mcmeta", R"({"pack":{"pack_format":0}})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().message.find("must be positive") !=
                   std::string::npos);
        }
    };

    "datapack/expected_pack_format mismatch is version_mismatch"_test = [] {
        temp_workspace work{"format-mismatch"};
        expect(work.write("pack_a/pack.mcmeta", manifest_format_121));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers, {.expected_pack_format = 120});
        expect(!loaded);
        if (!loaded) {
            expect(error_code_of(loaded) ==
                   static_cast<int>(refrain::error_code::version_mismatch));
            expect(loaded.error().message.find("121") != std::string::npos);
            expect(loaded.error().message.find("120") != std::string::npos);
        }
    };

    "datapack/expected_pack_format accepts match and zero"_test = [] {
        temp_workspace work{"format-match"};
        expect(work.write("pack_a/pack.mcmeta", manifest_format_121));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto matching = load_stack(layers, {.expected_pack_format = 121});
        expect(static_cast<bool>(matching));
        const auto any = load_stack(layers);
        expect(static_cast<bool>(any));
    };
};

// ---------------------------------------------------------------------------
// discovery / classification
// ---------------------------------------------------------------------------

const suite datapack_discovery_tests = [] {
    "datapack/discovery counts modelled kinds"_test = [] {
        temp_workspace work{"counts"};
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        expect(work.write("pack_a/data/refrain_test/recipe/b.json",
                          recipe_smelting));
        expect(work.write("pack_a/data/refrain_test/loot_table/blocks/a.json",
                          loot_block));
        expect(
            work.write("pack_a/data/refrain_test/advancement/story/root.json",
                       advancement_ok));
        expect(work.write(
            "pack_a/data/refrain_test/worldgen/biome/metal_plains.json",
            worldgen_biome));
        expect(work.write(
            "pack_a/data/refrain_test/worldgen/configured_feature/ore.json",
            R"({"type":"minecraft:ore"})"));
        expect(work.write("pack_a/data/refrain_test/tags/block/metal.json",
                          R"({"values":["refrain_test:iron_block"]})"));
        expect(work.write("pack_a/data/refrain_test/tags/item/metal.json",
                          R"({"values":["refrain_test:iron_ingot"]})"));
        expect(work.write("pack_a/data/refrain_test/function/init.mcfunction",
                          "say hello\n"));
        // Structure: tiny gzip NBT root compound.
        auto root = nbt::make_compound();
        expect(static_cast<bool>(root.set("version", nbt::make_int(1))));
        const auto encoded =
            nbt::write(root, "structure", nbt::compression::gzip);
        expect(static_cast<bool>(encoded));
        if (encoded) {
            expect(work.write_bytes(
                "pack_a/data/refrain_test/structure/hut.nbt", *encoded));
        }

        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto &stats = loaded->stats();
        expect(stats.counts.recipe == 2U);
        expect(stats.counts.loot_table == 1U);
        expect(stats.counts.advancement == 1U);
        expect(stats.counts.worldgen == 2U);
        expect(stats.counts.structure == 1U);
        expect(stats.counts.tags == 2U);
        expect(stats.counts.other == 1U);
        expect(stats.parsed.recipe == 2U);
        expect(stats.data_files == 10U);
        expect(stats.other_kinds.size() == 1U);
        expect(stats.other_kinds.at("function") == 1U);
        expect(loaded->other_counts().at("function") == 1U);
    };

    "datapack/unknown kinds are counted not fatal"_test = [] {
        temp_workspace work{"unknown-kinds"};
        expect(work.write("pack_a/data/refrain_test/function/a.mcfunction",
                          "say a\n"));
        expect(work.write("pack_a/data/refrain_test/predicate/a.json",
                          R"({"condition":"minecraft:random_chance"})"));
        expect(work.write("pack_a/data/refrain_test/item_modifier/a.json",
                          R"({"function":"minecraft:set_count"})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (loaded) {
            const auto &stats = loaded->stats();
            expect(stats.other_files == 3U);
            expect(stats.other_kinds.size() == 3U);
            expect(stats.counts.other == 3U);
            expect(stats.counts.recipe == 0U);
            expect(loaded->entries(content_kind::other).empty());
        }
    };

    "datapack/unknown kind with invalid JSON fails load"_test = [] {
        temp_workspace work{"unknown-bad-json"};
        expect(work.write("pack_a/data/refrain_test/predicate/broken.json",
                          R"({"condition":)"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(error_code_of(loaded) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(loaded.error().message.find("predicate/broken.json") !=
                   std::string::npos);
        }
    };

    "datapack/JSON syntax error reports pack label and path"_test = [] {
        temp_workspace work{"bad-json"};
        expect(work.write("pack_a/data/minecraft/recipe/broken.json",
                          R"({"type":)"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-bad"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(error_code_of(loaded) ==
                   static_cast<int>(refrain::error_code::parse_error));
            const auto message = loaded.error().message;
            expect(message.find("pack-bad") != std::string::npos);
            expect(message.find("data/minecraft/recipe/broken.json") !=
                   std::string::npos);
            expect(message.find("invalid JSON") != std::string::npos);
        }
    };

    "datapack/invalid recipe type is rejected"_test = [] {
        const std::array<std::pair<std::string_view, std::string_view>, 3>
            variants{{{"missing", R"({"pattern":[]})"},
                      {"number", R"({"type":7})"},
                      {"badid", R"({"type":"Minecraft:Bad"})"}}};
        for (const auto &[name, content] : variants) {
            temp_workspace work{"bad-type"};
            const auto relative{"pack_a/data/refrain_test/recipe/" +
                                std::string{name} + ".json"};
            expect(work.write(relative, content));
            auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
            const auto loaded = load_stack(layers);
            expect(!loaded)
                << "expected recipe '" << name << "' to be rejected";
            if (!loaded) {
                expect(loaded.error().message.find("type") !=
                       std::string::npos);
            }
        }
    };

    "datapack/advancement follows vanilla shape (no type field)"_test = [] {
        temp_workspace work{"advancement"};
        expect(
            work.write("pack_a/data/refrain_test/advancement/story/root.json",
                       advancement_ok));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (loaded) {
            expect(loaded->counts().advancement == 1U);
            expect(loaded->validate().ok());
        }

        temp_workspace missing{"advancement-missing-criteria"};
        expect(missing.write(
            "pack_a/data/refrain_test/advancement/story/root.json",
            R"({"display":{}})"));
        auto layers_missing =
            stack_of(open_layer(missing.path("pack_a"), "pack-a"));
        const auto rejected = load_stack(layers_missing);
        expect(!rejected);
        if (!rejected) {
            expect(rejected.error().message.find("criteria") !=
                   std::string::npos);
        }
    };

    "datapack/worldgen ids include the subkind"_test = [] {
        temp_workspace work{"worldgen-id"};
        expect(work.write("pack_a/data/refrain_test/worldgen/biome/taiga.json",
                          worldgen_biome));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(find_entry(*loaded, content_kind::worldgen,
                          "refrain_test:biome/taiga") != nullptr);
        expect(find_entry(*loaded, content_kind::worldgen,
                          "refrain_test:taiga") == nullptr);
        expect(loaded->entries(content_kind::worldgen).size() == 1U);
    };

    "datapack/invalid layout without a kind directory is rejected"_test = [] {
        temp_workspace work{"layout"};
        expect(
            work.write("pack_a/data/refrain_test/stray.json", recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().message.find("invalid layout") !=
                   std::string::npos);
        }
    };

    "datapack/uppercase path segments are rejected"_test = [] {
        temp_workspace work{"case"};
        expect(work.write("pack_a/data/Refrain_Test/recipe/a.json",
                          recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().message.find("not lowercase") !=
                   std::string::npos);
        }
    };

    "datapack/empty content id path is rejected"_test = [] {
        temp_workspace work{"empty-id"};
        expect(
            work.write("pack_a/data/refrain_test/recipe/.json", recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().message.find("invalid content id path") !=
                   std::string::npos);
        }
    };

    "datapack/null layer is rejected"_test = [] {
        auto layers = stack_of();
        const std::array<const pack *, 1> null_layers{nullptr};
        const auto loaded = datapack_set::load(std::span<const pack *const>{
            null_layers.data(), null_layers.size()});
        expect(!loaded);
        if (!loaded) {
            expect(error_code_of(loaded) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }
    };
};

// ---------------------------------------------------------------------------
// content override / structures
// ---------------------------------------------------------------------------

const suite datapack_content_tests = [] {
    "datapack/later pack overrides earlier content"_test = [] {
        temp_workspace work{"override"};
        expect(work.write("pack_a/data/refrain_test/recipe/block.json",
                          recipe_shaped));
        expect(work.write("pack_b/data/refrain_test/recipe/block.json",
                          recipe_smelting));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"),
                               open_layer(work.path("pack_b"), "pack-b"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto *entry{
            find_entry(*loaded, content_kind::recipe, "refrain_test:block")};
        expect(entry != nullptr);
        if (entry != nullptr) {
            expect(entry->source_label == "pack-b");
            const auto *type{entry->raw.find("type")};
            expect(type != nullptr);
            expect(type != nullptr &&
                   type->as_string().value_or("") == "minecraft:smelting");
        }
        expect(loaded->stats().override_count == 1U);
        expect(loaded->stats().counts.recipe == 1U);
        expect(loaded->stats().parsed.recipe == 2U);
        expect(loaded->duplicates().size() == 1U);
        expect(contains(loaded->duplicates(), "overrides"));
    };

    "datapack/duplicate overrides become validation warnings"_test = [] {
        temp_workspace work{"override-warning"};
        expect(work.write("pack_a/data/refrain_test/recipe/block.json",
                          recipe_shaped));
        expect(work.write("pack_b/data/refrain_test/recipe/block.json",
                          recipe_smelting));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"),
                               open_layer(work.path("pack_b"), "pack-b"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto report = loaded->validate();
        expect(report.ok());
        expect(contains(report.warnings, "overrides"));
        expect(contains(report.warnings, "refrain_test:block"));
    };

    "datapack/structure NBT gzip template loads with node counts"_test = [] {
        temp_workspace work{"structure"};
        auto root = nbt::make_compound();
        expect(static_cast<bool>(root.set("version", nbt::make_int(3))));
        auto palette = nbt::make_list(nbt::tag_type::compound);
        auto element = nbt::make_compound();
        expect(static_cast<bool>(
            element.set("name", nbt::make_string("minecraft:air"))));
        expect(static_cast<bool>(palette.push(std::move(element))));
        expect(static_cast<bool>(root.set("palette", std::move(palette))));
        const auto encoded =
            nbt::write(root, "structure", nbt::compression::gzip);
        expect(static_cast<bool>(encoded));
        if (!encoded) {
            return;
        }
        expect(work.write_bytes("pack_a/data/refrain_test/structure/hut.nbt",
                                *encoded));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto *entry{
            find_entry(*loaded, content_kind::structure, "refrain_test:hut")};
        expect(entry != nullptr);
        if (entry != nullptr) {
            expect(entry->structure.has_value());
            expect(entry->structure_info.nodes >= 4U);
            expect(entry->structure_info.depth >= 2U);
            expect(entry->byte_size > 0U);
        }
        expect(loaded->validate().ok());
    };

    "datapack/invalid structure NBT is a parse error"_test = [] {
        temp_workspace work{"bad-structure"};
        const std::string junk{"this is not gzip NBT"};
        const auto bytes{
            std::as_bytes(std::span<const char>{junk.data(), junk.size()})};
        expect(work.write_bytes("pack_a/data/refrain_test/structure/bad.nbt",
                                bytes));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(error_code_of(loaded) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(loaded.error().message.find("structure/bad.nbt") !=
                   std::string::npos);
            expect(loaded.error().message.find("NBT") != std::string::npos);
        }
    };

    "datapack/parse_structures=false skips NBT decoding"_test = [] {
        temp_workspace work{"skip-structures"};
        const std::string junk{"not gzip"};
        const auto bytes{
            std::as_bytes(std::span<const char>{junk.data(), junk.size()})};
        expect(work.write_bytes("pack_a/data/refrain_test/structure/bad.nbt",
                                bytes));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers, {.parse_structures = false});
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(loaded->stats().parsed.structure == 1U);
        const auto *entry{
            find_entry(*loaded, content_kind::structure, "refrain_test:bad")};
        expect(entry != nullptr);
        if (entry != nullptr) {
            expect(!entry->structure.has_value());
        }
    };
};

// ---------------------------------------------------------------------------
// tags
// ---------------------------------------------------------------------------

const suite datapack_tag_tests = [] {
    "datapack/tags append across packs in priority order"_test = [] {
        temp_workspace work{"tag-append"};
        expect(work.write(
            "pack_a/data/refrain_test/tags/block/metal.json",
            R"({"replace":false,"values":["refrain_test:iron_block"]})"));
        expect(work.write("pack_b/data/refrain_test/tags/block/metal.json",
                          R"({"values":["refrain_test:gold_block"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"),
                               open_layer(work.path("pack_b"), "pack-b"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto *tag{find_tag(*loaded, "block", "refrain_test:metal")};
        expect(tag != nullptr);
        if (tag != nullptr) {
            expect(tag->values.size() == 2U);
            expect(tag->sources == 2U);
            expect(tag->values.at(0).id.to_string() ==
                   "refrain_test:iron_block");
            expect(tag->values.at(1).id.to_string() ==
                   "refrain_test:gold_block");
        }
        const auto &stats = loaded->stats();
        expect(stats.counts.tags == 1U);
        expect(stats.tag_value_count == 2U);
        expect(stats.merged_tag_count == 1U);
        expect(stats.parsed.tags == 2U);
    };

    "datapack/tag replace clears accumulated values"_test = [] {
        temp_workspace work{"tag-replace"};
        expect(work.write(
            "pack_a/data/refrain_test/tags/block/metal.json",
            R"({"values":["refrain_test:iron_block","refrain_test:gold_block"]})"));
        expect(work.write(
            "pack_b/data/refrain_test/tags/block/metal.json",
            R"({"replace":true,"values":["refrain_test:copper_block"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"),
                               open_layer(work.path("pack_b"), "pack-b"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto *tag{find_tag(*loaded, "block", "refrain_test:metal")};
        expect(tag != nullptr);
        if (tag != nullptr) {
            expect(tag->values.size() == 1U);
            expect(tag->values.front().id.to_string() ==
                   "refrain_test:copper_block");
            expect(tag->sources == 2U);
        }
        expect(loaded->stats().merged_tag_count == 1U);
    };

    "datapack/tag object values carry required flag"_test = [] {
        temp_workspace work{"tag-objects"};
        expect(work.write(
            "pack_a/data/refrain_test/tags/block/fuel.json",
            R"({"values":["refrain_test:coal_block",{"id":"refrain_test:lava_bucket","required":false}]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto *tag{find_tag(*loaded, "block", "refrain_test:fuel")};
        expect(tag != nullptr);
        if (tag != nullptr) {
            expect(tag->values.size() == 2U);
            expect(!tag->values.at(1).tag_reference);
            expect(!tag->values.at(1).required);
        }
    };

    "datapack/tag reference entries are parsed and counted"_test = [] {
        temp_workspace work{"tag-refs"};
        expect(work.write("pack_a/data/refrain_test/tags/block/metal.json",
                          R"({"values":["refrain_test:iron_block"]})"));
        expect(work.write(
            "pack_a/data/refrain_test/tags/block/valuable.json",
            R"({"values":["#refrain_test:metal","refrain_test:diamond"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto *tag{find_tag(*loaded, "block", "refrain_test:valuable")};
        expect(tag != nullptr);
        if (tag != nullptr) {
            expect(tag->values.size() == 2U);
            expect(tag->values.at(0).tag_reference);
            expect(tag->values.at(1).tag_reference == false);
        }
        const auto report = loaded->validate();
        expect(report.errors.empty());
    };

    "datapack/invalid tag value is a parse error"_test = [] {
        temp_workspace work{"tag-bad-value"};
        expect(work.write("pack_a/data/refrain_test/tags/block/bad.json",
                          R"({"values":["#UPPER:Bad"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().message.find("resource location") !=
                   std::string::npos);
        }

        temp_workspace missing_id{"tag-missing-object-id"};
        expect(missing_id.write("pack_a/data/refrain_test/tags/block/bad.json",
                                R"({"values":[{"required":true}]})"));
        auto second = stack_of(open_layer(missing_id.path("pack_a"), "pack-a"));
        const auto rejected = load_stack(second);
        expect(!rejected);
        if (!rejected) {
            expect(rejected.error().message.find("'id'") != std::string::npos);
        }
    };

    "datapack/tag missing values array is rejected"_test = [] {
        temp_workspace work{"tag-no-values"};
        expect(work.write("pack_a/data/refrain_test/tags/block/bad.json",
                          R"({"replace":false})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().message.find("'values'") !=
                   std::string::npos);
        }
    };

    "datapack/tag types and iteration are sorted"_test = [] {
        temp_workspace work{"tag-types"};
        expect(work.write("pack_a/data/refrain_test/tags/block/b1.json",
                          R"({"values":["refrain_test:stone"]})"));
        expect(work.write("pack_a/data/refrain_test/tags/block/b0.json",
                          R"({"values":["refrain_test:dirt"]})"));
        expect(work.write("pack_a/data/refrain_test/tags/item/i0.json",
                          R"({"values":["refrain_test:stick"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto &tags = loaded->tags();
        expect(tags.size() == 3U);
        expect(tags.value_count() == 3U);
        expect(tags.tag_types() == (std::vector<std::string>{"block", "item"}));
        expect(tags.all().size() == 3U);
        const auto block_tags = tags.tags_of("block");
        expect(block_tags.size() == 2U);
        if (block_tags.size() == 2U) {
            expect(block_tags.at(0)->id.to_string() == "refrain_test:b0");
            expect(block_tags.at(1)->id.to_string() == "refrain_test:b1");
        }
        expect(tags.tags_of("missing").empty());
    };

    "datapack/tag duplicate values are preserved"_test = [] {
        temp_workspace work{"tag-duplicates"};
        expect(work.write(
            "pack_a/data/refrain_test/tags/block/dup.json",
            R"({"values":["refrain_test:stone","refrain_test:stone"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (loaded) {
            const auto *tag{find_tag(*loaded, "block", "refrain_test:dup")};
            expect(tag != nullptr);
            if (tag != nullptr) {
                expect(tag->values.size() == 2U);
            }
        }
    };
};

// ---------------------------------------------------------------------------
// validation
// ---------------------------------------------------------------------------

const suite datapack_validation_tests = [] {
    "datapack/missing required tag reference is an error"_test = [] {
        temp_workspace work{"missing-ref"};
        expect(work.write("pack_a/data/refrain_test/tags/block/valuable.json",
                          R"({"values":["#refrain_test:metal"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto report = loaded->validate();
        expect(!report.ok());
        expect(report.errors.size() == 1U);
        expect(contains(report.errors, "refrain_test:metal"));
        expect(contains(report.errors, "missing tag"));
        expect(contains(report.errors, "block/refrain_test:valuable"));
    };

    "datapack/optional missing tag reference is a warning"_test = [] {
        temp_workspace work{"optional-ref"};
        expect(work.write(
            "pack_a/data/refrain_test/tags/block/valuable.json",
            R"({"values":[{"id":"#refrain_test:metal","required":false}]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto report = loaded->validate();
        expect(report.ok());
        expect(report.errors.empty());
        expect(contains(report.warnings, "optional reference"));
    };

    "datapack/tag reference cycle is detected deterministically"_test = [] {
        temp_workspace work{"tag-cycle"};
        expect(work.write("pack_a/data/refrain_test/tags/block/a.json",
                          R"({"values":["#refrain_test:b"]})"));
        expect(work.write("pack_a/data/refrain_test/tags/block/b.json",
                          R"({"values":["#refrain_test:a"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto first = loaded->validate();
        expect(!first.ok());
        expect(contains(first.errors, "cycle"));
        expect(contains(first.errors, "refrain_test:a"));
        expect(contains(first.errors, "refrain_test:b"));
        const auto second = loaded->validate();
        expect(first.errors == second.errors);
    };

    "datapack/self tag reference cycle is detected"_test = [] {
        temp_workspace work{"tag-self-cycle"};
        expect(work.write("pack_a/data/refrain_test/tags/block/loop.json",
                          R"({"values":["#refrain_test:loop"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto report = loaded->validate();
        expect(!report.ok());
        expect(contains(report.errors, "cycle"));
        expect(contains(report.errors, "loop -> block/refrain_test:loop"));
    };

    "datapack/tag reference chain deeper than the limit is rejected"_test = [] {
        temp_workspace work{"tag-chain-depth"};
        // One tag more than the validator's depth cap (1024): t0 -> t1 ->
        // ... -> t1024 (empty).
        constexpr std::size_t chain_length{1025};
        for (std::size_t index{0}; index < chain_length; ++index) {
            std::string values{R"({"values":[]})"};
            if (index + 1 < chain_length) {
                values = "{\"values\":[\"#refrain_test:t" +
                         std::to_string(index + 1) + "\"]}";
            }
            const std::string path{"pack_a/data/refrain_test/tags/block/t" +
                                   std::to_string(index) + ".json"};
            expect(work.write(path, values))
                << "failed to write chain tag " << path;
        }
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(loaded->tags().size() == chain_length);
        const auto report = loaded->validate();
        expect(!report.ok());
        const auto depth_errors{std::count_if(
            report.errors.begin(), report.errors.end(),
            [](const std::string &line) {
                return line.find("maximum depth") != std::string::npos;
            })};
        expect(depth_errors == 1);
    };

    "datapack/short tag reference chain validates"_test = [] {
        temp_workspace work{"tag-chain-short"};
        expect(work.write("pack_a/data/refrain_test/tags/block/t0.json",
                          R"({"values":["#refrain_test:t1"]})"));
        expect(work.write("pack_a/data/refrain_test/tags/block/t1.json",
                          R"({"values":["#refrain_test:t2"]})"));
        expect(work.write("pack_a/data/refrain_test/tags/block/t2.json",
                          R"({"values":[]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto report = loaded->validate();
        expect(report.ok());
        if (!report.ok()) {
            expect(false) << text_of(report.errors);
        }
    };

    "datapack/tag references only resolve within the same tag type"_test = [] {
        temp_workspace work{"cross-type-ref"};
        expect(work.write("pack_a/data/refrain_test/tags/item/metal.json",
                          R"({"values":["refrain_test:iron_ingot"]})"));
        expect(
            work.write("pack_a/data/refrain_test/tags/block/uses_item_tag.json",
                       R"({"values":["#refrain_test:metal"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto report = loaded->validate();
        expect(!report.ok());
        expect(contains(report.errors, "missing tag"));
    };

    "datapack/validate reports stored counts"_test = [] {
        temp_workspace work{"report-counts"};
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        expect(work.write("pack_a/data/refrain_test/tags/block/a.json",
                          R"({"values":["refrain_test:stone"]})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto report = loaded->validate();
        expect(report.ok());
        expect(report.stats.counts.recipe == 1U);
        expect(report.stats.counts.tags == 1U);
        expect(report.stats.tag_value_count == 1U);
        expect(report.stats.data_files == 2U);
    };
};

// ---------------------------------------------------------------------------
// vanilla 26.3 shapes: jar markers, nested packs, directory-dispatched
// worldgen, slash tag registries and pack format gates
// ---------------------------------------------------------------------------

const suite datapack_vanilla_shape_tests = [] {
    "datapack/jar layout marker is ignored and counted"_test = [] {
        temp_workspace work{"marker"};
        // Vanilla jar root probe: a zero-byte `data/.mcassetsroot` marker.
        expect(work.write("pack_a/data/.mcassetsroot", ""));
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto &stats = loaded->stats();
        expect(stats.data_files == 2U);
        expect(stats.ignored_files == 1U);
        expect(stats.nested_datapack_files == 0U);
        expect(stats.nested_datapacks == 0U);
        expect(stats.counts.recipe == 1U);
        expect(stats.counts.other == 0U);
        expect(stats.other_files == 0U);
        expect(loaded->validate().ok());
    };

    "datapack/nested datapacks are counted but not parsed"_test = [] {
        temp_workspace work{"nested-packs"};
        expect(
            work.write("pack_a/data/refrain_test/datapacks/trade/pack.mcmeta",
                       R"({"pack":{"min_format":121,"max_format":121}})"));
        expect(work.write(
            "pack_a/data/refrain_test/datapacks/trade/data/refrain_test/"
            "recipe/trade.json",
            recipe_shaped));
        // Deliberately invalid JSON: nested pack content is out of scope and
        // must not be parsed or validated by the parent layer.
        expect(work.write(
            "pack_a/data/refrain_test/datapacks/trade/data/refrain_test/"
            "predicate/broken.json",
            R"({"condition":)"));
        expect(work.write(
            "pack_a/data/refrain_test/datapacks/redstone/pack.mcmeta",
            R"({"pack":{"min_format":121,"max_format":121}})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto &stats = loaded->stats();
        expect(stats.data_files == 4U);
        expect(stats.nested_datapack_files == 4U);
        expect(stats.nested_datapacks == 2U);
        expect(stats.ignored_files == 0U);
        expect(stats.counts.recipe == 0U);
        expect(stats.counts.other == 0U);
        expect(stats.other_files == 0U);
        expect(loaded->validate().ok());
    };

    "datapack/worldgen directory subkinds need only an object root"_test = [] {
        temp_workspace work{"worldgen-directory"};
        // Vanilla 26.3: these subkinds carry no top-level "type".
        expect(work.write("pack_a/data/refrain_test/worldgen/biome/plains.json",
                          R"({"has_precipitation":true,"temperature":0.8})"));
        expect(work.write(
            "pack_a/data/refrain_test/worldgen/placed_feature/ore.json",
            R"({"feature":"refrain_test:ore","placement":[]})"));
        expect(work.write("pack_a/data/refrain_test/worldgen/noise/rough.json",
                          R"({"firstOctave":-7,"amplitudes":[1.0,1.0]})"));
        expect(work.write(
            "pack_a/data/refrain_test/worldgen/noise_settings/overworld.json",
            R"({"sea_level":63})"));
        // Dispatch subkinds still need a string "type".
        expect(work.write("pack_a/data/refrain_test/worldgen/feature/ore.json",
                          R"({"type":"minecraft:ore"})"));
        expect(work.write("pack_a/data/refrain_test/worldgen/"
                          "block_state_provider/simple.json",
                          R"({"type":"minecraft:simple_state_provider"})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(loaded->stats().counts.worldgen == 6U);
        expect(loaded->stats().parsed.worldgen == 6U);
        expect(find_entry(*loaded, content_kind::worldgen,
                          "refrain_test:biome/plains") != nullptr);
        expect(find_entry(*loaded, content_kind::worldgen,
                          "refrain_test:placed_feature/ore") != nullptr);
        expect(find_entry(*loaded, content_kind::worldgen,
                          "refrain_test:noise/rough") != nullptr);
        expect(find_entry(*loaded, content_kind::worldgen,
                          "refrain_test:feature/ore") != nullptr);
        expect(loaded->validate().ok());
    };

    "datapack/density_function accepts constants and object codecs"_test = [] {
        temp_workspace work{"density-function"};
        // Vanilla `worldgen/density_function/zero.json` is the number 0.0.
        expect(work.write(
            "pack_a/data/refrain_test/worldgen/density_function/zero.json",
            "0.0"));
        expect(work.write(
            "pack_a/data/refrain_test/worldgen/density_function/reference.json",
            R"("minecraft:overworld")"));
        expect(work.write(
            "pack_a/data/refrain_test/worldgen/density_function/add.json",
            R"({"type":"minecraft:add","argument":0.0})"));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(loaded->stats().counts.worldgen == 3U);
        const auto *zero{find_entry(*loaded, content_kind::worldgen,
                                    "refrain_test:density_function/zero")};
        expect(zero != nullptr);
        if (zero != nullptr) {
            expect(zero->raw.is_number());
        }
        expect(loaded->validate().ok());

        // A dispatch-codec density function still requires a string "type".
        temp_workspace missing{"density-function-no-type"};
        expect(missing.write(
            "pack_a/data/refrain_test/worldgen/density_function/add.json",
            R"({"argument":0.0})"));
        auto missing_layers =
            stack_of(open_layer(missing.path("pack_a"), "pack-a"));
        const auto rejected = load_stack(missing_layers);
        expect(!rejected);
        if (!rejected) {
            expect(rejected.error().message.find("'type'") !=
                   std::string::npos);
        }
    };

    "datapack/worldgen dispatch subkinds still require type"_test = [] {
        const std::array<std::pair<std::string_view, std::string_view>, 4>
            variants{{{"feature", R"({"config":{}})"},
                      {"structure", R"({"biomes":[]})"},
                      {"carver", R"({"config":{}})"},
                      {"material_rule", R"({"rule":{}})"}}};
        for (const auto &[subkind, content] : variants) {
            temp_workspace work{"worldgen-dispatch"};
            const auto relative{"pack_a/data/refrain_test/worldgen/" +
                                std::string{subkind} + "/entry.json"};
            expect(work.write(relative, content));
            auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
            const auto loaded = load_stack(layers);
            expect(!loaded) << "expected worldgen/" << subkind
                            << " without a type to be rejected";
            if (!loaded) {
                expect(loaded.error().message.find("type") !=
                       std::string::npos);
            }
        }
    };

    "datapack/slash tag registries keep identity and resolve references"_test =
        [] {
            temp_workspace work{"tag-registry"};
            expect(work.write(
                "pack_a/data/minecraft/tags/worldgen/biome/is_ocean.json",
                R"({"values":["#minecraft:is_deep_ocean","minecraft:ocean"]})"));
            expect(work.write(
                "pack_a/data/minecraft/tags/worldgen/biome/is_deep_ocean.json",
                R"({"values":["minecraft:deep_ocean"]})"));
            // `block/mineable/*` stays a single-segment registry with a
            // sub-path tag id (longest-match must not invent registries).
            expect(work.write(
                "pack_a/data/refrain_test/tags/block/mineable/pickaxe.json",
                R"({"values":["refrain_test:stone"]})"));
            auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
            const auto loaded = load_stack(layers);
            expect(static_cast<bool>(loaded));
            if (!loaded) {
                return;
            }
            const auto &tags = loaded->tags();
            const auto *ocean{
                find_tag(*loaded, "worldgen/biome", "minecraft:is_ocean")};
            expect(ocean != nullptr);
            if (ocean != nullptr) {
                expect(ocean->tag_type == "worldgen/biome");
                expect(ocean->id.to_string() == "minecraft:is_ocean");
                expect(ocean->source_path ==
                       "data/minecraft/tags/worldgen/biome/is_ocean.json");
            }
            // The first segment alone is not the registry any more.
            auto ocean_id = resource_location::parse("minecraft:is_ocean");
            expect(static_cast<bool>(ocean_id));
            if (ocean_id) {
                expect(tags.find("worldgen", *ocean_id) == nullptr);
            }
            const auto *pickaxe{
                find_tag(*loaded, "block", "refrain_test:mineable/pickaxe")};
            expect(pickaxe != nullptr);
            if (pickaxe != nullptr) {
                expect(pickaxe->tag_type == "block");
            }
            expect(tags.tag_types() ==
                   (std::vector<std::string>{"block", "worldgen/biome"}));
            const auto report = loaded->validate();
            expect(report.ok());
            if (!report.ok()) {
                expect(false) << text_of(report.errors);
            }
        };

    "datapack/tag errors name the source file without a double colon"_test =
        [] {
            temp_workspace work{"tag-error-source"};
            const auto relative{
                "pack_a/data/minecraft/tags/worldgen/biome/is_ocean.json"};
            expect(work.write(relative,
                              R"({"values":["#minecraft:is_deep_ocean"]})"));
            auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
            const auto loaded = load_stack(layers);
            expect(static_cast<bool>(loaded));
            if (!loaded) {
                return;
            }
            const auto report = loaded->validate();
            expect(!report.ok());
            expect(report.errors.size() == 1U);
            expect(
                contains(report.errors, "worldgen/biome/minecraft:is_ocean"));
            expect(contains(report.errors, "minecraft:is_deep_ocean"));
            expect(
                contains(report.errors, "tags/worldgen/biome/is_ocean.json"));
            expect(!contains(report.errors, "worldgen:minecraft:"));
        };

    "datapack/min_format/max_format range satisfies the format gate"_test = [] {
        temp_workspace work{"format-range"};
        expect(work.write(
            "pack_a/pack.mcmeta",
            R"({"pack":{"description":"range","min_format":121,"max_format":123}})"));
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto inside = load_stack(layers, {.expected_pack_format = 122});
        expect(static_cast<bool>(inside));
        if (inside) {
            const auto &layer = inside->stats().layers.at(0);
            expect(layer.has_manifest);
            expect(layer.manifest_pack_format == 0U);
            expect(layer.manifest_min_format == 121U);
            expect(layer.manifest_max_format == 123U);
        }
        const auto outside = load_stack(layers, {.expected_pack_format = 120});
        expect(!outside);
        if (!outside) {
            expect(error_code_of(outside) ==
                   static_cast<int>(refrain::error_code::version_mismatch));
            expect(outside.error().message.find("121") != std::string::npos);
            expect(outside.error().message.find("123") != std::string::npos);
            expect(outside.error().message.find("120") != std::string::npos);
        }

        // The pair must be declared together and ordered.
        temp_workspace unpaired{"format-unpaired"};
        expect(unpaired.write("pack_a/pack.mcmeta",
                              R"({"pack":{"min_format":121}})"));
        auto unpaired_layers =
            stack_of(open_layer(unpaired.path("pack_a"), "pack-a"));
        const auto unpaired_loaded = load_stack(unpaired_layers);
        expect(!unpaired_loaded);
        if (!unpaired_loaded) {
            expect(unpaired_loaded.error().message.find("together") !=
                   std::string::npos);
        }

        temp_workspace inverted{"format-inverted"};
        expect(
            inverted.write("pack_a/pack.mcmeta",
                           R"({"pack":{"min_format":123,"max_format":121}})"));
        auto inverted_layers =
            stack_of(open_layer(inverted.path("pack_a"), "pack-a"));
        const auto inverted_loaded = load_stack(inverted_layers);
        expect(!inverted_loaded);
        if (!inverted_loaded) {
            expect(inverted_loaded.error().message.find("must not exceed") !=
                   std::string::npos);
        }
    };

    "datapack/expected format without a declaration is rejected"_test = [] {
        temp_workspace work{"format-missing"};
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto rejected = load_stack(layers, {.expected_pack_format = 121});
        expect(!rejected);
        if (!rejected) {
            expect(error_code_of(rejected) ==
                   static_cast<int>(refrain::error_code::version_mismatch));
            expect(rejected.error().message.find("pack.mcmeta") !=
                   std::string::npos);
            expect(rejected.error().message.find("121") != std::string::npos);
        }
        // No gate -> a manifest-less data tree still loads (jar-root layout).
        const auto accepted = load_stack(layers);
        expect(static_cast<bool>(accepted));
    };

    "datapack/version.json data_major satisfies the format gate"_test = [] {
        temp_workspace work{"format-version-json"};
        expect(work.write(
            "pack_a/version.json",
            R"({"id":"26.3","pack_version":{"resource_major":97,"data_major":121,"data_minor":0}})"));
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto matching = load_stack(layers, {.expected_pack_format = 121});
        expect(static_cast<bool>(matching));
        if (matching) {
            const auto &layer = matching->stats().layers.at(0);
            expect(!layer.has_manifest);
            expect(layer.has_version_metadata);
            expect(layer.version_data_major == 121U);
        }
        const auto mismatched =
            load_stack(layers, {.expected_pack_format = 120});
        expect(!mismatched);
        if (!mismatched) {
            expect(error_code_of(mismatched) ==
                   static_cast<int>(refrain::error_code::version_mismatch));
            expect(mismatched.error().message.find("version.json") !=
                   std::string::npos);
        }
    };

    "datapack/no-structures skips malformed structure NBT"_test = [] {
        temp_workspace work{"no-structures"};
        expect(work.write("pack_a/data/.mcassetsroot", ""));
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        const std::string junk{"this is not gzip NBT"};
        const auto bytes{
            std::as_bytes(std::span<const char>{junk.data(), junk.size()})};
        expect(work.write_bytes("pack_a/data/refrain_test/structure/bad.nbt",
                                bytes));
        auto rejected_layers =
            stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto rejected = load_stack(rejected_layers);
        expect(!rejected);
        if (!rejected) {
            expect(rejected.error().message.find("NBT") != std::string::npos);
        }
        // `--no-structures` maps to parse_structures=false: the NBT is counted
        // but never decoded, so the same pack loads cleanly.
        auto layers = stack_of(open_layer(work.path("pack_a"), "pack-a"));
        const auto loaded = load_stack(layers, {.parse_structures = false});
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(loaded->stats().parsed.structure == 1U);
        expect(loaded->stats().ignored_files == 1U);
        const auto *entry{
            find_entry(*loaded, content_kind::structure, "refrain_test:bad")};
        expect(entry != nullptr);
        if (entry != nullptr) {
            expect(!entry->structure.has_value());
        }
        expect(loaded->validate().ok());
    };
};

// ---------------------------------------------------------------------------
// vfs integration + synthetic corpus + fixture
// ---------------------------------------------------------------------------

const suite datapack_integration_tests = [] {
    "datapack/load(vfs) works for a single mounted pack"_test = [] {
        temp_workspace work{"vfs-single"};
        expect(work.write("pack_a/data/refrain_test/recipe/a.json",
                          recipe_shaped));
        auto layer = open_layer(work.path("pack_a"), "pack-a");
        expect(layer != nullptr);
        if (layer == nullptr) {
            return;
        }
        vfs stack;
        expect(static_cast<bool>(stack.mount(std::move(layer))));
        stack.freeze();
        const auto loaded = datapack_set::load(stack);
        expect(static_cast<bool>(loaded));
        if (loaded) {
            expect(loaded->counts().recipe == 1U);
            expect(loaded->layer_count() == 1U);
            expect(loaded->stats().layers.at(0).label == "pack-a");
        }
    };

    "datapack/load(vfs) merges tags across a multi-pack stack"_test = [] {
        temp_workspace work{"vfs-multi"};
        expect(work.write("pack_a/data/refrain_test/recipe/block.json",
                          recipe_shaped));
        expect(work.write("pack_a/data/refrain_test/tags/block/metal.json",
                          R"({"values":["refrain_test:iron_block"]})"));
        expect(work.write("pack_b/data/refrain_test/recipe/block.json",
                          recipe_smelting));
        expect(work.write("pack_b/data/refrain_test/tags/block/metal.json",
                          R"({"values":["refrain_test:gold_block"]})"));
        vfs stack;
        expect(static_cast<bool>(stack.mount(pack_source{
            work.path("pack_a").string(), pack_format::directory, "pack-a"})));
        expect(static_cast<bool>(stack.mount(pack_source{
            work.path("pack_b").string(), pack_format::directory, "pack-b"})));
        stack.freeze();
        const auto loaded = datapack_set::load(stack);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(loaded->layer_count() == 2U);
        expect(loaded->stats().layers.at(0).label == "pack-a");
        expect(loaded->stats().layers.at(1).label == "pack-b");
        expect(loaded->counts().recipe == 1U);
        expect(loaded->stats().override_count == 1U);
        const auto *entry{
            find_entry(*loaded, content_kind::recipe, "refrain_test:block")};
        expect(entry != nullptr);
        if (entry != nullptr) {
            expect(entry->source_label == "pack-b");
        }
        // The lower-priority tag file is hidden in the merged view but must
        // still participate in the merge through vfs::packs().
        const auto *tag{find_tag(*loaded, "block", "refrain_test:metal")};
        expect(tag != nullptr);
        if (tag != nullptr) {
            expect(tag->values.size() == 2U);
            expect(tag->values.at(0).id.to_string() ==
                   "refrain_test:iron_block");
            expect(tag->values.at(1).id.to_string() ==
                   "refrain_test:gold_block");
            expect(tag->sources == 2U);
        }
        expect(loaded->stats().merged_tag_count == 1U);
        const auto report = loaded->validate();
        expect(report.ok());
        expect(contains(report.warnings, "overrides"));
    };

    "datapack/load(vfs) rejects an empty vfs"_test = [] {
        const vfs empty_stack;
        const auto loaded = datapack_set::load(empty_stack);
        expect(!loaded);
        if (!loaded) {
            expect(error_code_of(loaded) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }
    };

    "datapack/~200 file synthetic corpus loads deterministically"_test = [] {
        temp_workspace work{"corpus"};
        std::vector<std::string> recipes;
        recipes.reserve(100);
        for (int index{0}; index < 100; ++index) {
            const auto name{"r" + std::to_string(index)};
            recipes.push_back(name);
            expect(
                work.write("pack_a/data/refrain_test/recipe/" + name + ".json",
                           recipe_shaped));
        }
        for (int index{0}; index < 20; ++index) {
            const auto name{"t" + std::to_string(index)};
            expect(work.write(
                "pack_a/data/refrain_test/tags/block/" + name + ".json",
                R"({"values":["refrain_test:a","refrain_test:b"]})"));
        }
        for (int index{0}; index < 10; ++index) {
            expect(work.write("pack_a/data/refrain_test/predicate/p" +
                                  std::to_string(index) + ".json",
                              R"({"condition":"minecraft:random_chance"})"));
        }
        for (int index{0}; index < 5; ++index) {
            expect(work.write("pack_a/data/refrain_test/function/f" +
                                  std::to_string(index) + ".mcfunction",
                              "say corpus\n"));
        }
        // Pack B overrides 50 recipes, appends to 10 tags, adds 10 item mods.
        for (int index{0}; index < 50; ++index) {
            expect(work.write("pack_b/data/refrain_test/recipe/r" +
                                  std::to_string(index) + ".json",
                              recipe_smelting));
        }
        for (int index{0}; index < 10; ++index) {
            expect(work.write("pack_b/data/refrain_test/tags/block/t" +
                                  std::to_string(index) + ".json",
                              R"({"values":["refrain_test:c"]})"));
        }
        for (int index{0}; index < 10; ++index) {
            expect(work.write("pack_b/data/refrain_test/item_modifier/m" +
                                  std::to_string(index) + ".json",
                              R"({"function":"minecraft:set_count"})"));
        }
        auto layers = stack_of(open_layer(work.path("pack_a"), "corpus-a"),
                               open_layer(work.path("pack_b"), "corpus-b"));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto &stats = loaded->stats();
        expect(stats.total_files == 205U);
        expect(stats.data_files == 205U);
        expect(stats.counts.recipe == 100U);
        expect(stats.parsed.recipe == 150U);
        expect(stats.override_count == 50U);
        expect(stats.counts.tags == 20U);
        expect(stats.tag_value_count == 50U);
        expect(stats.merged_tag_count == 10U);
        expect(stats.other_files == 25U);
        expect(stats.other_kinds.at("function") == 5U);
        expect(stats.other_kinds.at("predicate") == 10U);
        expect(stats.other_kinds.at("item_modifier") == 10U);
        const auto report = loaded->validate();
        expect(report.ok());
        expect(report.warnings.size() == 50U);
        expect(loaded->entries(content_kind::recipe).size() == 100U);
        expect(find_entry(*loaded, content_kind::recipe, "refrain_test:r99") !=
               nullptr);
        expect(find_entry(*loaded, content_kind::recipe,
                          "refrain_test:missing") == nullptr);
        // Deterministic: a second identical load produces identical stats.
        auto again = stack_of(open_layer(work.path("pack_a"), "corpus-a"),
                              open_layer(work.path("pack_b"), "corpus-b"));
        const auto reloaded = load_stack(again);
        expect(static_cast<bool>(reloaded));
        if (reloaded) {
            expect(reloaded->stats().counts.recipe == stats.counts.recipe);
            expect(reloaded->stats().tag_value_count == stats.tag_value_count);
            expect(reloaded->stats().override_count == stats.override_count);
        }
    };

    "datapack/lead synthetic fixture loads and validates"_test = [] {
        const fs::path fixture{REFRAIN_SOURCE_DIR "/test/fixtures/datapack"};
        auto layer = open_layer(fixture, "refrain-fixture");
        expect(layer != nullptr);
        if (layer == nullptr) {
            return;
        }
        auto layers = stack_of(std::move(layer));
        const auto loaded = load_stack(layers);
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto &stats = loaded->stats();
        expect(stats.layers.at(0).has_manifest);
        expect(stats.layers.at(0).manifest_pack_format == 121U);
        expect(stats.counts.recipe == 1U);
        expect(stats.counts.loot_table == 1U);
        expect(stats.counts.advancement == 1U);
        expect(stats.counts.worldgen == 1U);
        expect(stats.counts.tags == 2U);
        expect(stats.other_kinds.at("function") == 1U);
        const auto report = loaded->validate();
        expect(report.ok());
        if (!report.ok()) {
            expect(false) << text_of(report.errors);
        }
        const auto *fuel{find_tag(*loaded, "block", "refrain_test:fuel")};
        expect(fuel != nullptr);
        if (fuel != nullptr) {
            expect(fuel->values.size() == 2U);
            expect(fuel->values.at(0).tag_reference);
            expect(fuel->values.at(1).required == false);
        }
        expect(find_entry(*loaded, content_kind::recipe,
                          "refrain_test:metal_block") != nullptr);
        expect(find_entry(*loaded, content_kind::loot_table,
                          "refrain_test:blocks/metal_block") != nullptr);
        expect(find_entry(*loaded, content_kind::worldgen,
                          "refrain_test:biome/metal_plains") != nullptr);
    };
};

} // namespace
