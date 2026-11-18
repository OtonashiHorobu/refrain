// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Persistence of a generated jigsaw start whose pool elements carry an inline
// processor list. The official datapack expresses village animal and decor
// pieces with an inline list instead of a named processor-list id; the
// reference stores that list inline in the chunk `structures` compound, so a
// generated column holding such a start must persist through the storage
// bridge, the RWF record and the Anvil export instead of failing the
// conversion.
//
// The case generates column (3, 3) of seed 368560065, the last spawn column of
// that seed and the column whose village start carries the inline lists.
//
// Inputs: REFRAIN_VANILLA_WORLDGEN and REFRAIN_VANILLA_STRUCTURES. The case
// skips with one line when either is unset.

#include <refrain/data/nbt.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/world/store.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

namespace nbt = data::nbt;
namespace json = data::json;
namespace structure = worldgen::structure;

auto child(const nbt::compound &entries, std::string_view key)
    -> const nbt::value * {
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : &found->second;
}

/// The inline processor array of a decoded pool element, which is either the
/// direct codec's object form or its bare-list alternative.
auto inline_processor_list(const json::value &document) -> const json::value * {
    if (document.is_array()) {
        return &document;
    }
    if (!document.is_object()) {
        return nullptr;
    }
    const auto *processors = document.find("processors");
    return processors != nullptr && processors->is_array() ? processors
                                                           : nullptr;
}

/// Sizes of the inline processor lists one element carries, in the order the
/// encoder writes them: the element's own list first, then its list children.
void collect_record_sizes(const structure::jigsaw_element_info &element,
                          std::vector<std::size_t> &sizes) {
    if (element.inline_processors != nullptr) {
        const auto *list = inline_processor_list(*element.inline_processors);
        sizes.push_back(list == nullptr ? 0U : list->size());
    }
    for (const auto &nested : element.elements) {
        collect_record_sizes(nested, sizes);
    }
}

void collect_nbt_sizes(const nbt::compound &element,
                       std::vector<std::size_t> &sizes) {
    // A named processor list is a string; an inline one is the object form.
    if (const auto *processors = child(element, "processors");
        processors != nullptr && processors->as_compound() != nullptr) {
        const auto *list = child(*processors->as_compound(), "processors");
        expect(list != nullptr && list->as_list() != nullptr);
        if (list != nullptr && list->as_list() != nullptr) {
            sizes.push_back(list->as_list()->items.size());
        }
    }
    const auto *elements = child(element, "elements");
    if (elements == nullptr || elements->as_list() == nullptr) {
        return;
    }
    for (const auto &nested : elements->as_list()->items) {
        if (const auto *compound = nested.as_compound()) {
            collect_nbt_sizes(*compound, sizes);
        }
    }
}

/// Per-structure inline processor list sizes of the stored `structures`
/// compound.
auto stored_processor_sizes(const std::vector<std::byte> &payload)
    -> std::optional<std::map<std::string, std::vector<std::size_t>>> {
    auto document = nbt::read(payload);
    if (!document) {
        return std::nullopt;
    }
    const auto *root = document->as_compound();
    const auto *starts = root == nullptr ? nullptr : child(*root, "starts");
    if (starts == nullptr || starts->as_compound() == nullptr) {
        return std::nullopt;
    }
    std::map<std::string, std::vector<std::size_t>> sizes;
    for (const auto &[id, tag] : *starts->as_compound()) {
        const auto *start = tag.as_compound();
        const auto *children =
            start == nullptr ? nullptr : child(*start, "Children");
        if (children == nullptr || children->as_list() == nullptr) {
            continue;
        }
        std::vector<std::size_t> &entry = sizes[id];
        for (const auto &piece : children->as_list()->items) {
            const auto *compound = piece.as_compound();
            const auto *element = compound == nullptr
                                      ? nullptr
                                      : child(*compound, "pool_element");
            if (element == nullptr || element->as_compound() == nullptr) {
                continue;
            }
            collect_nbt_sizes(*element->as_compound(), entry);
        }
    }
    return sizes;
}

} // namespace

suite<"worldgen/structure-persist"> structure_persist_tests = [] {
    "a generated village start with inline processors persists"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (data_root == nullptr || templates_root == nullptr) {
            std::clog << "worldgen: inline-processor persistence skipped; set "
                         "REFRAIN_VANILLA_WORLDGEN and "
                         "REFRAIN_VANILLA_STRUCTURES to run it\n";
            return;
        }
        auto generator = chunk_generator::create(chunk_generator_config{
            .data_root = data_root,
            .templates_root = templates_root,
            .seed = 368560065,
            .kind = dimension_kind::overworld,
            .target = generation_target::full,
        });
        expect(generator.has_value())
            << (generator ? "" : generator.error().message);
        if (!generator) {
            return;
        }
        const auto chunk = (*generator)->generate(chunk_pos{.x = 3, .z = 3});
        expect(chunk.has_value()) << (chunk ? "" : chunk.error().message);
        if (!chunk) {
            return;
        }

        // The generation record itself carries the inline lists; without them
        // this case would not exercise the encoding gap.
        std::map<std::string, std::vector<std::size_t>> expected;
        for (const auto &start : (*chunk)->structure_starts()) {
            std::vector<std::size_t> &sizes = expected[start.structure];
            for (const auto &piece : start.pieces) {
                if (piece.jigsaw) {
                    collect_record_sizes(piece.jigsaw->element, sizes);
                }
            }
        }
        std::size_t inline_elements = 0;
        for (const auto &[id, sizes] : expected) {
            inline_elements += sizes.size();
        }
        expect(inline_elements > 0U)
            << "seed 368560065 no longer generates a start with inline "
               "processors";
        if (inline_elements == 0U) {
            return;
        }

        const auto storage =
            world::from_proto_chunk(**chunk, (*generator)->biome_names());
        expect(storage.has_value()) << (storage ? "" : storage.error().message);
        if (!storage) {
            return;
        }
        const auto stored_sizes = stored_processor_sizes(storage->structures);
        expect(stored_sizes.has_value());
        if (!stored_sizes) {
            return;
        }
        expect(*stored_sizes == expected);

        // The RWF chunk record keeps the opaque compound byte for byte.
        const auto payload = world::encode_chunk(
            *storage, nullptr, true, world::chunk_palette_mode::inline_keys);
        expect(payload.has_value()) << (payload ? "" : payload.error().message);
        if (!payload) {
            return;
        }
        const auto decoded = world::decode_chunk(*payload);
        expect(decoded.has_value()) << (decoded ? "" : decoded.error().message);
        if (!decoded) {
            return;
        }
        expect(decoded->structures == storage->structures);

        // An Anvil export carries the same compound, and importing it back
        // keeps it.
        const test_support::scratch_directory scratch;
        const auto root = scratch.path() / "anvil";
        auto store = world::world_store::create(root.string());
        expect(store.has_value());
        if (!store) {
            return;
        }
        nbt::compound level{};
        level["DataVersion"] = nbt::value{std::int32_t{5023}};
        nbt::compound meta{};
        meta["level"] = nbt::value{std::move(level)};
        const auto meta_bytes =
            nbt::write(nbt::value{std::move(meta)}, "", nbt::compression::none);
        expect(meta_bytes.has_value());
        if (!meta_bytes) {
            return;
        }
        expect(store->write_meta(*meta_bytes).has_value());
        expect(store->write_chunk({.x = 3, .z = 3}, *storage).has_value());
        expect(store->commit().has_value());
        const auto exported = scratch.path() / "export";
        const auto export_result =
            world::anvil::export_world(root.string(), exported.string(), true);
        expect(export_result.has_value());
        if (!export_result) {
            return;
        }
        const auto imported = scratch.path() / "import";
        const auto import_result = world::anvil::import_world(
            exported.string(), imported.string(), true);
        expect(import_result.has_value());
        if (!import_result) {
            return;
        }
        auto reopened = world::world_store::open(imported.string());
        expect(reopened.has_value());
        if (!reopened) {
            return;
        }
        const auto back = reopened->read_chunk({.x = 3, .z = 3});
        expect(back.has_value()) << (back ? "" : back.error().message);
        if (!back) {
            return;
        }
        const auto reloaded_sizes = stored_processor_sizes(back->structures);
        expect(reloaded_sizes.has_value());
        if (!reloaded_sizes) {
            return;
        }
        expect(*reloaded_sizes == expected);
    };
};
