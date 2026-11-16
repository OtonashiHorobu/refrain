// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Re-buildable content view: layered load, override priority, validation
// diagnostics, registry inputs and their change description.

#include <refrain/core/io.hpp>
#include <refrain/data/content_view.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/resource_location.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace data = refrain::data;
namespace io = refrain::core::io;
using refrain::test_support::scratch_directory;

constexpr std::string_view recipe_alpha{
    R"({"type":"minecraft:crafting_shapeless","group":"alpha"})"};

constexpr std::string_view recipe_beta{
    R"({"type":"minecraft:crafting_shapeless","group":"beta"})"};

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

[[nodiscard]] auto write_bytes(const std::filesystem::path &root,
                               std::string_view relative, std::string_view text)
    -> bool {
    const auto target{root / std::filesystem::path{std::string{relative}}};
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_file(target.string(),
                          std::as_bytes(std::span{text.data(), text.size()}))
        .has_value();
}

[[nodiscard]] auto directory_source(const std::filesystem::path &root,
                                    std::string_view label = {})
    -> data::content_source {
    return data::content_source{
        .kind = data::content_source_kind::directory,
        .path = root.string(),
        .label = std::string{label},
    };
}

/// Layer-list helper: `content_view::build` takes a span, and a braced
/// initializer list does not bind to one through a template context here.
[[nodiscard]] auto layers(std::initializer_list<data::content_source> sources)
    -> std::vector<data::content_source> {
    return std::vector<data::content_source>{sources};
}

/// A datapack with one recipe, one loot table, one advancement, one worldgen
/// document and one tag.
[[nodiscard]] auto write_documents(const std::filesystem::path &root) -> bool {
    return write_file(root, "data/test/recipe/alpha.json", recipe_alpha) &&
           write_file(root, "data/test/loot_table/chest.json",
                      R"({"type":"minecraft:chest"})") &&
           write_file(
               root, "data/test/advancement/root.json",
               R"({"criteria":{"tick":{"trigger":"minecraft:tick"}}})") &&
           write_file(root, "data/test/worldgen/biome/plains.json",
                      R"({"temperature":0.8})") &&
           write_file(root, "data/test/tags/block/logs.json",
                      R"({"values":["minecraft:oak_log"]})");
}

[[nodiscard]] auto recipe_group(const data::content_view &view,
                                std::string_view path) -> std::string {
    auto id{data::resource_location::parse(std::string{"test:"} +
                                           std::string{path})};
    if (!id) {
        return {};
    }
    const auto *entry{view.find(data::content_kind::recipe, *id)};
    if (entry == nullptr) {
        return {};
    }
    const auto *group{entry->raw.find("group")};
    if (group == nullptr) {
        return {};
    }
    const auto text{group->as_string()};
    return text ? std::string{*text} : std::string{};
}

} // namespace

const suite content_view_tests = [] {
    "content view/no layers builds an empty view"_test = [] {
        auto built{data::content_view::build({})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        expect(built->empty());
        expect(built->layer_count() == 0U);
        expect(built->file_count() == 0U);
        expect(built->datapacks() == nullptr);
        expect(built->tags() == nullptr);
        expect(built->find(data::content_kind::recipe,
                           data::resource_location{}) == nullptr);
        expect(built->registry_inputs().empty());
        expect(data::format_content_view_summary(*built) ==
               "no content layers are mounted");
    };

    "content view/loads and indexes datapack documents"_test = [] {
        scratch_directory scratch{};
        const auto pack_dir{scratch.path() / "pack"};
        expect(write_documents(pack_dir));
        auto built{
            data::content_view::build(layers({directory_source(pack_dir)}))};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        expect(!built->empty());
        expect(built->layer_count() == 1U);
        expect(built->file_count() == 5U);
        const auto &counts{built->counts()};
        expect(counts.recipe == 1U);
        expect(counts.loot_table == 1U);
        expect(counts.advancement == 1U);
        expect(counts.worldgen == 1U);
        expect(counts.tags == 1U);
        expect(built->tags() != nullptr);
        if (built->tags() != nullptr) {
            expect(built->tags()->size() == 1U);
        }
        expect(recipe_group(*built, "alpha") == "alpha");
        expect(built->registry_inputs().empty());
        expect(data::format_content_view_summary(*built).find(
                   "1 content layers, 5 files, 1 recipes") !=
               std::string::npos);
    };

    "content view/higher priority layers override lower ones"_test = [] {
        scratch_directory scratch{};
        const auto low{scratch.path() / "low"};
        const auto high{scratch.path() / "high"};
        expect(write_file(low, "data/test/recipe/alpha.json", recipe_alpha));
        expect(write_file(high, "data/test/recipe/alpha.json", recipe_beta));
        auto built{data::content_view::build(layers(
            {directory_source(low, "low"), directory_source(high, "high")}))};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        expect(built->layer_count() == 2U);
        expect(recipe_group(*built, "alpha") == "beta");
        const auto *entry{
            built->find(data::content_kind::recipe,
                        *data::resource_location::parse("test:alpha"))};
        expect(entry != nullptr);
        if (entry != nullptr) {
            expect(entry->source_label == "high");
            expect(entry->source_path == "data/test/recipe/alpha.json");
        }
        // Both layers are still visible through the pack stack, which is what
        // layer-aware consumers (tags, assets) read.
        expect(built->packs().pack_count() == 2U);
    };

    "content view/invalid content fails with the layer and path"_test = [] {
        scratch_directory scratch{};
        const auto pack_dir{scratch.path() / "pack"};
        expect(write_file(pack_dir, "data/test/recipe/broken.json",
                          R"({"type":})"));
        auto built{data::content_view::build(
            layers({directory_source(pack_dir, "broken-pack")}))};
        expect(!built);
        if (!built) {
            expect(built.error().code == refrain::error_code::parse_error);
            expect(built.error().message.find("broken-pack") !=
                   std::string::npos);
            expect(built.error().message.find("data/test/recipe/broken.json") !=
                   std::string::npos);
        }
    };

    "content view/records the files that name registry data"_test = [] {
        scratch_directory scratch{};
        const auto pack_dir{scratch.path() / "pack"};
        expect(write_file(pack_dir, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        expect(write_file(pack_dir, "data/test/item/ruby.json",
                          R"({"type":"minecraft:item"})"));
        expect(write_file(pack_dir, "data/test/entity_type/ruby.json",
                          R"({"type":"minecraft:entity_type"})"));
        expect(
            write_file(pack_dir, "data/test/recipe/alpha.json", recipe_alpha));
        expect(write_bytes(pack_dir, "tables.rvtb", "registry-table"));
        auto built{
            data::content_view::build(layers({directory_source(pack_dir)}))};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        // Installed content has no block registry documents, so boot normally
        // records none; a fixture that ships them must record every one.
        const auto inputs{built->registry_inputs()};
        expect(inputs.size() == 4U);
        std::vector<std::string> paths{};
        for (const auto &input : inputs) {
            paths.push_back(input.path);
        }
        const std::vector<std::string> expected{
            "data/test/block/ruby.json", "data/test/entity_type/ruby.json",
            "data/test/item/ruby.json", "tables.rvtb"};
        expect(paths == expected);
    };

    "content view/is_registry_input_path only matches registry documents"_test =
        [] {
            expect(data::is_registry_input_path("data/test/block/ruby.json"));
            expect(data::is_registry_input_path("data/test/item/ruby.json"));
            expect(data::is_registry_input_path(
                "data/test/entity_type/ruby.json"));
            expect(data::is_registry_input_path("tables.rvtb"));
            expect(data::is_registry_input_path("protocol_registry.json"));
            expect(
                !data::is_registry_input_path("data/test/recipe/alpha.json"));
            expect(!data::is_registry_input_path("data/test/block"));
            expect(!data::is_registry_input_path("data/test/block/"));
            expect(
                !data::is_registry_input_path("assets/test/block/ruby.json"));
            expect(!data::is_registry_input_path(""));
        };

    "content view/describes the registry change between two views"_test = [] {
        scratch_directory scratch{};
        const auto live{scratch.path() / "live"};
        expect(write_documents(live));
        auto current{
            data::content_view::build(layers({directory_source(live)}))};
        expect(static_cast<bool>(current));
        if (!current) {
            return;
        }
        const auto changed{scratch.path() / "changed"};
        expect(write_file(changed, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        auto candidate{
            data::content_view::build(layers({directory_source(changed)}))};
        expect(static_cast<bool>(candidate));
        if (!candidate) {
            return;
        }
        const auto description{
            data::describe_registry_change(*current, *candidate)};
        expect(description.find("data/test/block/ruby.json") !=
               std::string::npos);
        expect(description.find("added") != std::string::npos);
        expect(data::describe_registry_change(*current, *current).empty());
        // A changed file at the same path is named as changed, not added.
        expect(write_file(changed, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block","note":"v2"})"));
        auto edited{
            data::content_view::build(layers({directory_source(changed)}))};
        expect(static_cast<bool>(edited));
        if (edited) {
            // Compare against the view that already carries the same path: the
            // content hash changed, so the path is reported as changed.
            const auto edited_description{
                data::describe_registry_change(*candidate, *edited)};
            expect(edited_description.find("changed") != std::string::npos);
            expect(edited_description.find("data/test/block/ruby.json") !=
                   std::string::npos);
        }
    };

    "content view/skips a composition layer that is not published"_test = [] {
        scratch_directory scratch{};
        const auto store{scratch.path() / "store"};
        auto built{data::content_view::build(layers({data::content_source{
            .kind = data::content_source_kind::composition,
            .path = store.string(),
            .composition = "gamepack-test",
            .label = "test-composition",
        }}))};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        // A store without a published composition is not a fatal error at
        // boot, and a reload must be able to publish an empty view.
        expect(built->empty());
        expect(built->skipped_sources().size() == 1U);
        expect(built->warnings().size() == 1U);
        if (!built->warnings().empty()) {
            expect(built->warnings().front().find("test-composition") !=
                   std::string::npos);
        }
    };

    "content view/build is deterministic for the same layers"_test = [] {
        scratch_directory scratch{};
        const auto pack_dir{scratch.path() / "pack"};
        expect(write_documents(pack_dir));
        auto first{
            data::content_view::build(layers({directory_source(pack_dir)}))};
        auto second{
            data::content_view::build(layers({directory_source(pack_dir)}))};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        expect(first->fingerprint() == second->fingerprint());
        expect(first->registry_fingerprint() == second->registry_fingerprint());
    };

    "content view/an added layer changes the structural fingerprint"_test = [] {
        scratch_directory scratch{};
        const auto one{scratch.path() / "one"};
        const auto two{scratch.path() / "two"};
        expect(write_documents(one));
        expect(write_file(two, "data/test/recipe/alpha.json", recipe_beta));
        auto single{data::content_view::build(layers({directory_source(one)}))};
        auto layered{data::content_view::build(
            layers({directory_source(one), directory_source(two)}))};
        expect(static_cast<bool>(single));
        expect(static_cast<bool>(layered));
        if (!single || !layered) {
            return;
        }
        expect(single->fingerprint() != layered->fingerprint());
    };
};
