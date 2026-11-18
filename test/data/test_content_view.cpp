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

    "content view/registry generations assign ids in content-key order"_test =
        [] {
            scratch_directory scratch{};
            const auto pack_dir{scratch.path() / "pack"};
            // Deliberately written in non-sorted order: the generation sorts
            // by canonical content key, never by file creation order.
            expect(write_file(pack_dir, "data/test/block/zeta.json",
                              R"({"type":"minecraft:block"})"));
            expect(write_file(pack_dir, "data/test/block/alpha.json",
                              R"({"type":"minecraft:block"})"));
            expect(write_file(pack_dir, "data/test/item/beta.json",
                              R"({"type":"minecraft:item"})"));
            expect(write_file(pack_dir, "data/test/entity_type/mob.json",
                              R"({"type":"minecraft:entity_type"})"));
            expect(write_bytes(pack_dir, "tables.rvtb", "registry-table"));
            auto built{data::content_view::build(
                layers({directory_source(pack_dir)}))};
            expect(static_cast<bool>(built));
            if (!built) {
                return;
            }
            const auto &generation{built->registries()};
            expect(!generation.empty());
            expect(generation.entry_count() == 4U);
            expect(generation.size(data::registry_kind::block) == 2U);
            expect(generation.size(data::registry_kind::item) == 1U);
            expect(generation.size(data::registry_kind::entity_type) == 1U);
            // The registry container is not a per-entry document and never
            // enters the id mapping.
            const auto blocks{generation.entries(data::registry_kind::block)};
            expect(blocks.size() == 2U);
            if (blocks.size() == 2U) {
                expect(blocks[0].location.to_string() == "test:alpha");
                expect(blocks[0].id == data::registry_id{0});
                expect(blocks[0].path == "data/test/block/alpha.json");
                expect(blocks[1].location.to_string() == "test:zeta");
                expect(blocks[1].id == data::registry_id{1});
            }
            auto alpha{data::resource_location::parse("test:alpha")};
            auto missing{data::resource_location::parse("test:missing")};
            expect(alpha.has_value());
            expect(missing.has_value());
            if (alpha && missing) {
                expect(generation.find(data::registry_kind::block, *alpha) !=
                       nullptr);
                expect(generation.find(data::registry_kind::block, *missing) ==
                       nullptr);
            }
            expect(generation.fingerprint() != data::digest32{});
        };

    "content view/a registry generation is deterministic and detects an "
    "edited document"_test = [] {
        scratch_directory scratch{};
        const auto pack_dir{scratch.path() / "pack"};
        expect(write_file(pack_dir, "data/test/block/alpha.json",
                          R"({"type":"minecraft:block"})"));
        expect(write_file(pack_dir, "data/test/block/zeta.json",
                          R"({"type":"minecraft:block"})"));
        auto first{
            data::content_view::build(layers({directory_source(pack_dir)}))};
        auto second{
            data::content_view::build(layers({directory_source(pack_dir)}))};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        expect(first->registries() == second->registries());
        const auto old_generation{first->registries()};
        // Editing one document changes that entry's digest and the generation
        // fingerprint, and leaves the old generation untouched.
        expect(write_file(pack_dir, "data/test/block/alpha.json",
                          R"({"type":"minecraft:block","note":"v2"})"));
        auto edited{
            data::content_view::build(layers({directory_source(pack_dir)}))};
        expect(static_cast<bool>(edited));
        if (!edited) {
            return;
        }
        expect(edited->registries().fingerprint() !=
               old_generation.fingerprint());
        auto alpha{data::resource_location::parse("test:alpha")};
        expect(alpha.has_value());
        if (alpha) {
            const auto *old_entry{
                old_generation.find(data::registry_kind::block, *alpha)};
            const auto *new_entry{
                edited->registries().find(data::registry_kind::block, *alpha)};
            expect(old_entry != nullptr);
            expect(new_entry != nullptr);
            if (old_entry != nullptr && new_entry != nullptr) {
                expect(old_entry->content != new_entry->content);
                // Ids are a function of the key set, so the same key keeps the
                // same id across generations; the content digest changes.
                expect(old_entry->id == new_entry->id);
            }
        }
    };

    "content view/classifies registry changes for the reload policy"_test = [] {
        scratch_directory scratch{};
        const auto live{scratch.path() / "live"};
        expect(write_documents(live));
        expect(write_file(live, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        expect(write_bytes(live, "tables.rvtb", "registry-table-v1"));
        auto current{
            data::content_view::build(layers({directory_source(live)}))};
        expect(static_cast<bool>(current));
        if (!current) {
            return;
        }
        // No registry input changed: the change is not registry-affecting.
        const auto recipe_only{scratch.path() / "recipe-only"};
        expect(write_documents(recipe_only));
        expect(
            write_file(recipe_only, "data/test/recipe/beta.json", recipe_beta));
        expect(write_file(recipe_only, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        expect(write_bytes(recipe_only, "tables.rvtb", "registry-table-v1"));
        auto recipe_candidate{
            data::content_view::build(layers({directory_source(recipe_only)}))};
        expect(static_cast<bool>(recipe_candidate));
        if (recipe_candidate) {
            const auto change{
                data::classify_registry_change(*current, *recipe_candidate)};
            expect(change.empty());
            expect(!change.hot_appliable());
            expect(!change.restart_required());
        }
        // A changed per-entry document is hot-appliable as a new generation.
        const auto documents{scratch.path() / "documents"};
        expect(write_documents(documents));
        expect(write_file(documents, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block","note":"v2"})"));
        expect(write_bytes(documents, "tables.rvtb", "registry-table-v1"));
        auto document_candidate{
            data::content_view::build(layers({directory_source(documents)}))};
        expect(static_cast<bool>(document_candidate));
        if (document_candidate) {
            const auto change{
                data::classify_registry_change(*current, *document_candidate)};
            expect(change.kind == data::registry_change_kind::documents);
            expect(change.hot_appliable());
            expect(!change.restart_required());
            const auto named{change.document_paths()};
            expect(named.size() == 1U);
            expect(!named.empty() &&
                   named.front() == "data/test/block/ruby.json");
            expect(change.container_paths().empty());
            expect(change.description().find("changed") != std::string::npos);
            expect(change.description().find("data/test/block/ruby.json") !=
                   std::string::npos);
        }
        // A changed registry container is restart-required and names the file.
        const auto containers{scratch.path() / "containers"};
        expect(write_documents(containers));
        expect(write_file(containers, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        expect(write_bytes(containers, "tables.rvtb", "registry-table-v2"));
        auto container_candidate{
            data::content_view::build(layers({directory_source(containers)}))};
        expect(static_cast<bool>(container_candidate));
        if (container_candidate) {
            const auto change{
                data::classify_registry_change(*current, *container_candidate)};
            expect(change.kind == data::registry_change_kind::containers);
            expect(change.restart_required());
            expect(!change.hot_appliable());
            const auto named{change.container_paths()};
            expect(named.size() == 1U);
            expect(!named.empty() && named.front() == "tables.rvtb");
            expect(change.description().find("tables.rvtb") !=
                   std::string::npos);
        }
        // Both classes changed: the reload must be refused as one unit.
        const auto mixed{scratch.path() / "mixed"};
        expect(write_documents(mixed));
        expect(write_file(mixed, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block","note":"v3"})"));
        expect(write_bytes(mixed, "tables.rvtb", "registry-table-v2"));
        auto mixed_candidate{
            data::content_view::build(layers({directory_source(mixed)}))};
        expect(static_cast<bool>(mixed_candidate));
        if (mixed_candidate) {
            const auto change{
                data::classify_registry_change(*current, *mixed_candidate)};
            expect(change.kind == data::registry_change_kind::mixed);
            expect(change.restart_required());
            expect(change.paths().size() == 2U);
            expect(change.document_paths().size() == 1U);
            expect(change.container_paths().size() == 1U);
        }
    };

    "content view/separates entry-name changes from digest-only changes"_test =
        [] {
            scratch_directory scratch{};
            const auto live{scratch.path() / "live"};
            expect(write_file(live, "data/test/block/alpha.json",
                              R"({"type":"minecraft:block"})"));
            expect(write_file(live, "data/test/item/beta.json",
                              R"({"type":"minecraft:item"})"));
            auto current{
                data::content_view::build(layers({directory_source(live)}))};
            expect(static_cast<bool>(current));
            if (!current) {
                return;
            }
            // Same entry names, changed document digest: the mapping changes
            // but chunk generation resolves the same names.
            const auto edited{scratch.path() / "edited"};
            expect(write_file(edited, "data/test/block/alpha.json",
                              R"({"type":"minecraft:block","note":"v2"})"));
            expect(write_file(edited, "data/test/item/beta.json",
                              R"({"type":"minecraft:item"})"));
            auto edited_view{
                data::content_view::build(layers({directory_source(edited)}))};
            expect(static_cast<bool>(edited_view));
            if (edited_view) {
                const auto change{
                    data::classify_registry_change(*current, *edited_view)};
                expect(change.kind == data::registry_change_kind::documents);
                expect(change.changed.size() == 1U);
                expect(change.added.empty());
                expect(change.removed.empty());
                expect(!change.entry_names_changed());
                expect(current->registries().fingerprint() !=
                       edited_view->registries().fingerprint());
            }
            // Added and removed documents are entry-name changes.
            const auto renamed{scratch.path() / "renamed"};
            expect(write_file(renamed, "data/test/block/gamma.json",
                              R"({"type":"minecraft:block"})"));
            expect(write_file(renamed, "data/test/item/beta.json",
                              R"({"type":"minecraft:item"})"));
            auto renamed_view{
                data::content_view::build(layers({directory_source(renamed)}))};
            expect(static_cast<bool>(renamed_view));
            if (renamed_view) {
                const auto change{
                    data::classify_registry_change(*current, *renamed_view)};
                expect(change.kind == data::registry_change_kind::documents);
                expect(change.entry_names_changed());
                expect(change.added.size() == 1U);
                expect(change.removed.size() == 1U);
            }
        };

    "content view/refuses a changed registry document that names no entry"_test =
        [] {
            scratch_directory scratch{};
            const auto live{scratch.path() / "live"};
            expect(write_file(live, "data/test/block/alpha.json",
                              R"({"type":"minecraft:block"})"));
            auto current{
                data::content_view::build(layers({directory_source(live)}))};
            expect(static_cast<bool>(current));
            if (!current) {
                return;
            }
            const auto stray{scratch.path() / "stray"};
            expect(write_file(stray, "data/test/block/alpha.json",
                              R"({"type":"minecraft:block"})"));
            // A non-JSON file under a registry directory is a registry input
            // but cannot define an entry; publishing it would change the input
            // fingerprint without changing the mapping, so the reload is
            // classified as restart-required instead.
            expect(write_file(stray, "data/test/block/notes", "notes"));
            auto stray_view{
                data::content_view::build(layers({directory_source(stray)}))};
            expect(static_cast<bool>(stray_view));
            if (stray_view) {
                const auto change{
                    data::classify_registry_change(*current, *stray_view)};
                expect(change.restart_required());
                expect(!change.hot_appliable());
                expect(change.description().find("data/test/block/notes") !=
                       std::string::npos);
                auto notes{data::resource_location::parse("test:notes")};
                expect(notes.has_value());
                if (notes) {
                    expect(stray_view->registries().find(
                               data::registry_kind::block, *notes) == nullptr);
                }
            }
        };
};
