// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The `structures` compound stores a jigsaw pool element's processor list in
// the reference's shape: a named processor list as its id string, and an
// inline list as the direct codec's object form `{"processors": [...]}`.
// The generation record keeps the datapack document of an inline list, so the
// encoder converts it. A mixed numeric array becomes a TAG_Double list, which
// every numeric reader in the reference accepts; a mixed array containing a
// non-numeric value has no tag form in this model and fails. A `processors`
// field on a kind whose codec has no such field is ignored.

#include "../../src/world/structure_nbt_internal.hpp"

#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/jigsaw.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
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

auto inline_processors(std::string_view document)
    -> std::shared_ptr<const json::value> {
    auto parsed = json::value::parse(document);
    if (!parsed) {
        return nullptr;
    }
    return std::make_shared<const json::value>(std::move(*parsed));
}

auto make_element(structure::pool_element_kind kind)
    -> structure::jigsaw_element_info {
    structure::jigsaw_element_info element{};
    element.kind = kind;
    element.projection = structure::pool_projection::rigid;
    element.template_id = "minecraft:test/template";
    return element;
}

auto make_piece(structure::jigsaw_element_info element)
    -> worldgen::structure_piece_record {
    worldgen::structure_piece_record piece{};
    piece.type = "minecraft:jigsaw";
    piece.min_x = -4;
    piece.min_y = 0;
    piece.min_z = -4;
    piece.max_x = 4;
    piece.max_y = 4;
    piece.max_z = 4;
    piece.orientation = 0;
    piece.template_rotation = 0;
    piece.template_mirror = 0;
    piece.jigsaw = structure::jigsaw_piece_data{
        .element = std::move(element),
        .ground_level_delta = 1,
        .liquid = structure::liquid_settings::apply_waterlogging,
    };
    return piece;
}

auto make_start(std::vector<worldgen::structure_piece_record> pieces)
    -> worldgen::structure_start {
    worldgen::structure_start start{};
    start.structure = "minecraft:test_structure";
    start.step = "surface_structures";
    start.chunk_x = 0;
    start.chunk_z = 0;
    start.min_x = -4;
    start.min_y = 0;
    start.min_z = -4;
    start.max_x = 4;
    start.max_y = 4;
    start.max_z = 4;
    start.pieces = std::move(pieces);
    return start;
}

auto encode_one(const worldgen::structure_start &start)
    -> std::optional<nbt::value> {
    const std::vector<worldgen::structure_reference> references{};
    auto bytes = world::detail::encode_structures({start}, references, 0, 0);
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed = nbt::read(*bytes);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

auto child(const nbt::compound &entries, std::string_view key)
    -> const nbt::value * {
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : &found->second;
}

/// The first piece tag of the encoded start.
auto first_piece(const nbt::value &root) -> const nbt::compound * {
    const auto *starts = root.as_compound();
    if (starts == nullptr) {
        return nullptr;
    }
    const auto *start = child(*starts, "starts");
    if (start == nullptr || start->as_compound() == nullptr) {
        return nullptr;
    }
    const auto *structure =
        child(*start->as_compound(), "minecraft:test_structure");
    if (structure == nullptr || structure->as_compound() == nullptr) {
        return nullptr;
    }
    const auto *children = child(*structure->as_compound(), "Children");
    if (children == nullptr || children->as_list() == nullptr ||
        children->as_list()->items.empty()) {
        return nullptr;
    }
    return children->as_list()->items.front().as_compound();
}

auto pool_element(const nbt::compound &piece) -> const nbt::compound * {
    const auto *element = child(piece, "pool_element");
    return element == nullptr ? nullptr : element->as_compound();
}

/// The processors child of a pool element, whichever shape it carries.
auto processors_field(const nbt::compound &element) -> const nbt::value * {
    return child(element, "processors");
}

/// The first processor entry of an encoded inline list.
auto first_processor(const nbt::value &root) -> const nbt::compound * {
    const auto *piece = first_piece(root);
    if (piece == nullptr) {
        return nullptr;
    }
    const auto *element = pool_element(*piece);
    if (element == nullptr) {
        return nullptr;
    }
    const auto *processors = processors_field(*element);
    if (processors == nullptr || processors->as_compound() == nullptr) {
        return nullptr;
    }
    const auto *list = child(*processors->as_compound(), "processors");
    if (list == nullptr || list->as_list() == nullptr ||
        list->as_list()->items.empty()) {
        return nullptr;
    }
    return list->as_list()->items.front().as_compound();
}

} // namespace

suite<"world/structure-nbt"> structure_nbt_tests = [] {
    "an empty inline processor list is stored as the direct codec object"_test =
        [] {
            auto element =
                make_element(structure::pool_element_kind::legacy_single);
            element.inline_processors =
                inline_processors(R"({"processors":[]})");
            expect(element.inline_processors != nullptr);
            const auto encoded = encode_one(make_start({make_piece(element)}));
            expect(encoded.has_value());
            if (!encoded) {
                return;
            }
            const auto *piece = first_piece(*encoded);
            expect(piece != nullptr);
            if (piece == nullptr) {
                return;
            }
            const auto *element_tag = pool_element(*piece);
            expect(element_tag != nullptr);
            if (element_tag == nullptr) {
                return;
            }
            const auto *processors = processors_field(*element_tag);
            expect(processors != nullptr);
            if (processors == nullptr) {
                return;
            }
            // The reference writes the direct codec's object form here, not a
            // bare list and not a processor-list id.
            expect(processors->is(nbt::tag_type::compound));
            const auto *entries = processors->as_compound();
            expect(entries != nullptr);
            if (entries == nullptr) {
                return;
            }
            expect(entries->size() == 1U);
            const auto *list = child(*entries, "processors");
            expect(list != nullptr);
            if (list == nullptr) {
                return;
            }
            expect(list->is(nbt::tag_type::list));
            expect(list->as_list() != nullptr);
            if (list->as_list() == nullptr) {
                return;
            }
            expect(list->as_list()->items.empty());
            expect(list->as_list()->element_type == nbt::tag_type::end);
        };

    "an inline processor list keeps its entries and value kinds"_test = [] {
        const std::string document = R"({"processors":[
            {"processor_type":"minecraft:rule","rules":[
                {"input_predicate":{"predicate_type":"minecraft:random_block_match",
                                    "block":"minecraft:cobblestone",
                                    "probability":0.25},
                 "location_predicate":{"predicate_type":"minecraft:always_true"},
                 "output_state":"minecraft:mossy_cobblestone",
                 "seed":1234,"large":5000000000,"active":true}]},
            {"processor_type":"minecraft:capped",
             "delegate":{"processor_type":"minecraft:block_age","mossiness":0.5},
             "limit":7}]})";
        auto element = make_element(structure::pool_element_kind::single);
        element.inline_processors = inline_processors(document);
        expect(element.inline_processors != nullptr);
        const auto encoded = encode_one(make_start({make_piece(element)}));
        expect(encoded.has_value());
        if (!encoded) {
            return;
        }
        const auto *piece = first_piece(*encoded);
        expect(piece != nullptr);
        if (piece == nullptr) {
            return;
        }
        const auto *element_tag = pool_element(*piece);
        expect(element_tag != nullptr);
        if (element_tag == nullptr) {
            return;
        }
        const auto *processors = processors_field(*element_tag);
        expect(processors != nullptr && processors->as_compound() != nullptr);
        if (processors == nullptr || processors->as_compound() == nullptr) {
            return;
        }
        const auto *list = child(*processors->as_compound(), "processors");
        expect(list != nullptr && list->as_list() != nullptr);
        if (list == nullptr || list->as_list() == nullptr) {
            return;
        }
        expect(list->as_list()->element_type == nbt::tag_type::compound);
        const auto &entries = list->as_list()->items;
        expect(entries.size() == 2U);
        if (entries.size() != 2U) {
            return;
        }
        const auto *rule_processor = entries[0].as_compound();
        expect(rule_processor != nullptr);
        if (rule_processor == nullptr) {
            return;
        }
        const auto *type = child(*rule_processor, "processor_type");
        expect(type != nullptr && type->as_string() == "minecraft:rule");
        const auto *rules = child(*rule_processor, "rules");
        expect(rules != nullptr && rules->as_list() != nullptr);
        if (rules == nullptr || rules->as_list() == nullptr) {
            return;
        }
        expect(rules->as_list()->items.size() == 1U);
        const auto *rule = rules->as_list()->items.front().as_compound();
        expect(rule != nullptr);
        if (rule == nullptr) {
            return;
        }
        const auto *predicate = child(*rule, "input_predicate");
        expect(predicate != nullptr && predicate->as_compound() != nullptr);
        if (predicate == nullptr || predicate->as_compound() == nullptr) {
            return;
        }
        expect(child(*predicate->as_compound(), "block")->as_string() ==
               "minecraft:cobblestone");
        // A JSON real is stored as a double; the reference's float codecs read
        // any numeric tag, so the value survives the round trip.
        const auto *probability =
            child(*predicate->as_compound(), "probability");
        expect(probability != nullptr &&
               probability->is(nbt::tag_type::double_));
        expect(probability->as_double() == 0.25);
        const auto *seed = child(*rule, "seed");
        expect(seed != nullptr && seed->is(nbt::tag_type::int_));
        expect(seed->as_int() == 1234);
        const auto *large = child(*rule, "large");
        expect(large != nullptr && large->is(nbt::tag_type::long_));
        expect(large->as_long() == 5000000000LL);
        const auto *active = child(*rule, "active");
        expect(active != nullptr && active->is(nbt::tag_type::byte));
        expect(active->as_byte() == 1);
        const auto *capped = entries[1].as_compound();
        expect(capped != nullptr);
        if (capped == nullptr) {
            return;
        }
        const auto *limit = child(*capped, "limit");
        expect(limit != nullptr && limit->as_int() == 7);
        const auto *delegate = child(*capped, "delegate");
        expect(delegate != nullptr && delegate->as_compound() != nullptr);
        if (delegate != nullptr && delegate->as_compound() != nullptr) {
            const auto *mossiness =
                child(*delegate->as_compound(), "mossiness");
            expect(mossiness != nullptr &&
                   mossiness->is(nbt::tag_type::double_));
            expect(mossiness->as_double() == 0.5);
        }
    };

    "a bare inline processor array is stored as the processors object"_test =
        [] {
            auto element = make_element(structure::pool_element_kind::single);
            // The direct codec's second alternative accepts a bare list; the
            // stored tag normalises it to the object form the reference
            // writes.
            element.inline_processors =
                inline_processors(R"([{"processor_type":"minecraft:nop"}])");
            expect(element.inline_processors != nullptr);
            const auto encoded = encode_one(make_start({make_piece(element)}));
            expect(encoded.has_value());
            if (!encoded) {
                return;
            }
            const auto *piece = first_piece(*encoded);
            expect(piece != nullptr);
            if (piece == nullptr) {
                return;
            }
            const auto *element_tag = pool_element(*piece);
            expect(element_tag != nullptr);
            if (element_tag == nullptr) {
                return;
            }
            const auto *processors = processors_field(*element_tag);
            expect(processors != nullptr &&
                   processors->as_compound() != nullptr);
            if (processors == nullptr || processors->as_compound() == nullptr) {
                return;
            }
            const auto *list = child(*processors->as_compound(), "processors");
            expect(list != nullptr && list->as_list() != nullptr);
            if (list != nullptr && list->as_list() != nullptr) {
                expect(list->as_list()->items.size() == 1U);
            }
        };

    "a nested list element keeps a child's inline processors"_test = [] {
        auto parent = make_element(structure::pool_element_kind::list);
        auto inner = make_element(structure::pool_element_kind::legacy_single);
        inner.inline_processors = inline_processors(R"({"processors":[]})");
        parent.elements.push_back(std::move(inner));
        const auto encoded = encode_one(make_start({make_piece(parent)}));
        expect(encoded.has_value());
        if (!encoded) {
            return;
        }
        const auto *piece = first_piece(*encoded);
        expect(piece != nullptr);
        if (piece == nullptr) {
            return;
        }
        const auto *element_tag = pool_element(*piece);
        expect(element_tag != nullptr);
        if (element_tag == nullptr) {
            return;
        }
        const auto *elements = child(*element_tag, "elements");
        expect(elements != nullptr && elements->as_list() != nullptr);
        if (elements == nullptr || elements->as_list() == nullptr) {
            return;
        }
        expect(elements->as_list()->element_type == nbt::tag_type::compound);
        expect(elements->as_list()->items.size() == 1U);
        const auto *nested = elements->as_list()->items.front().as_compound();
        expect(nested != nullptr);
        if (nested == nullptr) {
            return;
        }
        const auto *processors = processors_field(*nested);
        expect(processors != nullptr && processors->as_compound() != nullptr);
        if (processors != nullptr && processors->as_compound() != nullptr) {
            const auto *list = child(*processors->as_compound(), "processors");
            expect(list != nullptr && list->as_list() != nullptr);
        }
    };

    "a mixed numeric array in inline processor data becomes a double list"_test =
        [] {
            const std::string document = R"({"processors":[
                {"processor_type":"minecraft:nop",
                 "mixed":[1,2.5,3,true]}]})";
            auto element = make_element(structure::pool_element_kind::single);
            element.inline_processors = inline_processors(document);
            expect(element.inline_processors != nullptr);
            const auto encoded = encode_one(make_start({make_piece(element)}));
            expect(encoded.has_value());
            if (!encoded) {
                return;
            }
            const auto *entry = first_processor(*encoded);
            expect(entry != nullptr);
            if (entry == nullptr) {
                return;
            }
            const auto *mixed = child(*entry, "mixed");
            expect(mixed != nullptr && mixed->as_list() != nullptr);
            if (mixed == nullptr || mixed->as_list() == nullptr) {
                return;
            }
            expect(mixed->as_list()->element_type == nbt::tag_type::double_);
            const auto &items = mixed->as_list()->items;
            expect(items.size() == 4U);
            if (items.size() != 4U) {
                return;
            }
            expect(items[0].as_double() == 1.0);
            expect(items[1].as_double() == 2.5);
            expect(items[2].as_double() == 3.0);
            expect(items[3].as_double() == 1.0);
        };

    "a homogeneous numeric array keeps its tag type"_test = [] {
        const std::string document = R"({"processors":[
            {"processor_type":"minecraft:nop","values":[1,2,3]}]})";
        auto element = make_element(structure::pool_element_kind::single);
        element.inline_processors = inline_processors(document);
        expect(element.inline_processors != nullptr);
        const auto encoded = encode_one(make_start({make_piece(element)}));
        expect(encoded.has_value());
        if (!encoded) {
            return;
        }
        const auto *entry = first_processor(*encoded);
        expect(entry != nullptr);
        if (entry == nullptr) {
            return;
        }
        const auto *values = child(*entry, "values");
        expect(values != nullptr && values->as_list() != nullptr);
        if (values == nullptr || values->as_list() == nullptr) {
            return;
        }
        expect(values->as_list()->element_type == nbt::tag_type::int_);
        expect(values->as_list()->items.size() == 3U);
    };

    "an inline processor document the tag model cannot carry fails closed"_test =
        [] {
            const auto encode = [](std::string_view document) {
                auto element =
                    make_element(structure::pool_element_kind::single);
                element.inline_processors = inline_processors(document);
                return encode_one(make_start({make_piece(element)}));
            };
            // A non-object, non-list value, an object without the processors
            // field, a processor entry that is not an object and a null all
            // have no reference shape.
            expect(!encode("5").has_value());
            expect(!encode(R"({"limit":6})").has_value());
            expect(!encode(R"({"processors":[1,2]})").has_value());
            expect(!encode(R"({"processors":[{"processor_type":"minecraft:nop"},
                                             null]})")
                        .has_value());
            // This NBT model has only homogeneous lists and does not carry the
            // reference's empty-name wrappers, so an array mixing a number
            // with a string has no representable tag here.
            expect(!encode(
                        R"({"processors":[{"processor_type":"minecraft:nop",
                                           "mixed":[1,"two"]}]})")
                        .has_value());
        };

    "a processors field on an element without a processor codec is ignored"_test =
        [] {
            const auto encode = [](structure::pool_element_kind kind,
                                   bool inline_list) {
                auto element = make_element(kind);
                if (kind == structure::pool_element_kind::feature) {
                    element.feature = "minecraft:test_feature";
                }
                if (kind == structure::pool_element_kind::list) {
                    element.elements.push_back(
                        make_element(structure::pool_element_kind::empty));
                }
                if (inline_list) {
                    element.inline_processors =
                        inline_processors(R"({"processors":[]})");
                } else {
                    element.processor_list = "minecraft:test_processors";
                }
                return encode_one(make_start({make_piece(element)}));
            };
            for (const auto kind : {structure::pool_element_kind::feature,
                                    structure::pool_element_kind::empty,
                                    structure::pool_element_kind::list}) {
                for (const bool inline_list : {false, true}) {
                    const auto encoded = encode(kind, inline_list);
                    expect(encoded.has_value());
                    if (!encoded) {
                        continue;
                    }
                    const auto *piece = first_piece(*encoded);
                    expect(piece != nullptr);
                    if (piece == nullptr) {
                        continue;
                    }
                    const auto *element = pool_element(*piece);
                    expect(element != nullptr);
                    if (element == nullptr) {
                        continue;
                    }
                    expect(processors_field(*element) == nullptr);
                }
            }
        };

    "a named processor list stays a processor-list id"_test = [] {
        auto element = make_element(structure::pool_element_kind::single);
        element.processor_list = "minecraft:test_processors";
        const auto encoded = encode_one(make_start({make_piece(element)}));
        expect(encoded.has_value());
        if (!encoded) {
            return;
        }
        const auto *piece = first_piece(*encoded);
        expect(piece != nullptr);
        if (piece == nullptr) {
            return;
        }
        const auto *element_tag = pool_element(*piece);
        expect(element_tag != nullptr);
        if (element_tag == nullptr) {
            return;
        }
        const auto *processors = processors_field(*element_tag);
        expect(processors != nullptr &&
               processors->as_string() == "minecraft:test_processors");
    };
};
