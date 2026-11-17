// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/vanilla.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/json.hpp>

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::data {

namespace {

namespace io = refrain::core::io;

auto fail(std::string_view message) -> error {
    return make_error(error_code::invalid_argument, message);
}

auto parse_fail(std::string_view file, std::string_view detail) -> error {
    std::string message{file};
    message.append(": ");
    message.append(detail);
    return make_error(error_code::parse_error, message);
}

/// Quantizes a climate coordinate exactly as the game does:
/// `(long)(coordinate * 10000.0F)` with a float multiply and a truncating cast.
auto quantize(double value) -> std::int32_t {
    return static_cast<std::int32_t>(static_cast<float>(value) * 10000.0F);
}

auto as_double(const simdjson::dom::element &element) -> std::optional<double> {
    if (element.is_double()) {
        double value = 0.0;
        if (element.get_double().get(value) == simdjson::SUCCESS) {
            return value;
        }
        return std::nullopt;
    }
    if (element.is_int64()) {
        std::int64_t value = 0;
        if (element.get_int64().get(value) == simdjson::SUCCESS) {
            return static_cast<double>(value);
        }
        return std::nullopt;
    }
    if (element.is_uint64()) {
        std::uint64_t value = 0;
        if (element.get_uint64().get(value) == simdjson::SUCCESS) {
            return static_cast<double>(value);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

auto member(const simdjson::dom::object &object, std::string_view key,
            simdjson::dom::element &out) -> bool {
    simdjson::dom::element found;
    if (object[key].get(found) != simdjson::SUCCESS) {
        return false;
    }
    out = found;
    return true;
}

auto parse_range(const simdjson::dom::element &element, std::int32_t &min,
                 std::int32_t &max) -> bool {
    if (const auto point = as_double(element); point.has_value()) {
        min = quantize(*point);
        max = min;
        return true;
    }
    if (element.is_array()) {
        simdjson::dom::array array;
        if (element.get_array().get(array) != simdjson::SUCCESS ||
            array.size() != 2) {
            return false;
        }
        auto iterator = array.begin();
        const auto first = as_double(*iterator);
        ++iterator;
        const auto second = as_double(*iterator);
        if (!first.has_value() || !second.has_value()) {
            return false;
        }
        min = quantize(*first);
        max = quantize(*second);
        return true;
    }
    if (element.is_object()) {
        simdjson::dom::object object;
        if (element.get_object().get(object) != simdjson::SUCCESS) {
            return false;
        }
        simdjson::dom::element low_element;
        simdjson::dom::element high_element;
        if (!member(object, "min", low_element) ||
            !member(object, "max", high_element)) {
            return false;
        }
        const auto low = as_double(low_element);
        const auto high = as_double(high_element);
        if (!low.has_value() || !high.has_value()) {
            return false;
        }
        min = quantize(*low);
        max = quantize(*high);
        return true;
    }
    return false;
}

auto parse_blocks(std::string_view json, std::vector<block_spec> &blocks)
    -> result<void> {
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (const auto error = parser.parse(json).get(document);
        error != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("blocks.json", simdjson::error_message(error))};
    }
    simdjson::dom::object root;
    if (document.get_object().get(root) != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("blocks.json", "root is not an object")};
    }

    for (const auto block_field : root) {
        block_spec block;
        block.name = std::string{block_field.key};
        simdjson::dom::object entry;
        if (block_field.value.get_object().get(entry) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(
                "blocks.json", "block entry is not an object: " + block.name)};
        }

        simdjson::dom::element properties_element;
        if (member(entry, "properties", properties_element)) {
            simdjson::dom::object property_object;
            if (properties_element.get_object().get(property_object) !=
                simdjson::SUCCESS) {
                return std::unexpected{
                    parse_fail("blocks.json",
                               "properties is not an object: " + block.name)};
            }
            for (const auto property_field : property_object) {
                simdjson::dom::array values;
                if (property_field.value.get_array().get(values) !=
                    simdjson::SUCCESS) {
                    return std::unexpected{parse_fail(
                        "blocks.json",
                        "property values is not an array: " + block.name)};
                }
                std::vector<std::string> texts;
                for (const auto value : values) {
                    std::string_view text;
                    if (value.get_string().get(text) != simdjson::SUCCESS) {
                        return std::unexpected{parse_fail(
                            "blocks.json",
                            "property value is not a string: " + block.name)};
                    }
                    texts.emplace_back(text);
                }
                block.property_names.emplace_back(property_field.key);
                block.property_values.push_back(std::move(texts));
            }
        }

        simdjson::dom::element states_element;
        simdjson::dom::array states;
        if (!member(entry, "states", states_element) ||
            states_element.get_array().get(states) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(
                "blocks.json", "states is not an array: " + block.name)};
        }
        for (const auto state_element : states) {
            simdjson::dom::object state_object;
            if (state_element.get_object().get(state_object) !=
                simdjson::SUCCESS) {
                return std::unexpected{parse_fail(
                    "blocks.json", "state is not an object: " + block.name)};
            }
            state_spec state;
            simdjson::dom::element id_element;
            std::int64_t id = 0;
            if (!member(state_object, "id", id_element) ||
                id_element.get_int64().get(id) != simdjson::SUCCESS || id < 0 ||
                id > 0xFFFF) {
                return std::unexpected{parse_fail(
                    "blocks.json", "state id out of range: " + block.name)};
            }
            state.vanilla_id = static_cast<std::uint16_t>(id);

            simdjson::dom::element default_element;
            if (member(state_object, "default", default_element)) {
                bool is_default = false;
                if (default_element.get_bool().get(is_default) !=
                    simdjson::SUCCESS) {
                    return std::unexpected{parse_fail(
                        "blocks.json",
                        "default marker is not a bool: " + block.name)};
                }
                state.is_default = is_default;
            }

            state.values.resize(block.property_names.size());
            simdjson::dom::element state_properties_element;
            if (member(state_object, "properties", state_properties_element)) {
                simdjson::dom::object state_properties;
                if (state_properties_element.get_object().get(
                        state_properties) != simdjson::SUCCESS) {
                    return std::unexpected{parse_fail(
                        "blocks.json",
                        "state properties is not an object: " + block.name)};
                }
                for (const auto value_field : state_properties) {
                    const auto position =
                        std::ranges::find(block.property_names,
                                          std::string_view{value_field.key});
                    if (position == block.property_names.end()) {
                        return std::unexpected{
                            parse_fail("blocks.json",
                                       "state uses an undeclared property: " +
                                           block.name)};
                    }
                    std::string_view text;
                    if (value_field.value.get_string().get(text) !=
                        simdjson::SUCCESS) {
                        return std::unexpected{parse_fail(
                            "blocks.json",
                            "state value is not a string: " + block.name)};
                    }
                    state.values[static_cast<std::size_t>(
                        position - block.property_names.begin())] =
                        std::string{text};
                }
            }
            block.states.push_back(std::move(state));
        }
        blocks.push_back(std::move(block));
    }
    return status_ok();
}

auto parse_biomes(std::string_view name, std::string_view json,
                  biome_preset_spec &preset) -> result<void> {
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (const auto error = parser.parse(json).get(document);
        error != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail(name, simdjson::error_message(error))};
    }
    simdjson::dom::object root;
    if (document.get_object().get(root) != simdjson::SUCCESS) {
        return std::unexpected{parse_fail(name, "root is not an object")};
    }
    simdjson::dom::element entries_element;
    simdjson::dom::array entries;
    if (!member(root, "biomes", entries_element) ||
        entries_element.get_array().get(entries) != simdjson::SUCCESS) {
        return std::unexpected{parse_fail(name, "biomes is not an array")};
    }
    preset.name = std::string{name};
    for (const auto entry_element : entries) {
        simdjson::dom::object entry;
        if (entry_element.get_object().get(entry) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(name, "entry is not an object")};
        }
        biome_parameter_spec spec;
        simdjson::dom::element biome_element;
        std::string_view biome;
        if (!member(entry, "biome", biome_element) ||
            biome_element.get_string().get(biome) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(name, "biome is not a string")};
        }
        spec.biome = std::string{biome};
        simdjson::dom::element parameters_element;
        simdjson::dom::object parameters;
        if (!member(entry, "parameters", parameters_element) ||
            parameters_element.get_object().get(parameters) !=
                simdjson::SUCCESS) {
            return std::unexpected{
                parse_fail(name, "parameters is not an object")};
        }
        static constexpr std::array<std::string_view, 6> channels{
            "temperature", "humidity", "continentalness",
            "erosion",     "depth",    "weirdness"};
        for (std::size_t i = 0; i < channels.size(); ++i) {
            simdjson::dom::element range_element;
            if (!member(parameters, channels[i], range_element) ||
                !parse_range(range_element, spec.min[i], spec.max[i])) {
                return std::unexpected{parse_fail(
                    name, "bad climate range: " + std::string{channels[i]})};
            }
        }
        simdjson::dom::element offset_element;
        const auto offset = member(parameters, "offset", offset_element)
                                ? as_double(offset_element)
                                : std::nullopt;
        if (!offset.has_value()) {
            return std::unexpected{parse_fail(name, "missing offset")};
        }
        spec.offset = quantize(*offset);
        preset.entries.push_back(std::move(spec));
    }
    return status_ok();
}

auto parse_registries(std::string_view json, std::vector<registry_spec> &out)
    -> result<void> {
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (const auto error = parser.parse(json).get(document);
        error != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("registries.json", simdjson::error_message(error))};
    }
    simdjson::dom::object root;
    if (document.get_object().get(root) != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("registries.json", "root is not an object")};
    }
    for (const auto registry_field : root) {
        registry_spec registry;
        registry.name = std::string{registry_field.key};
        simdjson::dom::element entries_element;
        simdjson::dom::object entries;
        if (registry_field.value.is_object()) {
            simdjson::dom::object wrapper;
            if (registry_field.value.get_object().get(wrapper) !=
                    simdjson::SUCCESS ||
                !member(wrapper, "entries", entries_element) ||
                entries_element.get_object().get(entries) !=
                    simdjson::SUCCESS) {
                return std::unexpected{
                    parse_fail("registries.json",
                               "entries is not an object: " + registry.name)};
            }
        } else {
            return std::unexpected{
                parse_fail("registries.json",
                           "registry is not an object: " + registry.name)};
        }
        for (const auto entry_field : entries) {
            registry_entry_spec entry;
            entry.name = std::string{entry_field.key};
            simdjson::dom::element protocol_element;
            std::int64_t protocol_id = 0;
            if (!entry_field.value.is_object()) {
                return std::unexpected{
                    parse_fail("registries.json",
                               "entry is not an object: " + entry.name)};
            }
            simdjson::dom::object entry_object;
            if (entry_field.value.get_object().get(entry_object) !=
                    simdjson::SUCCESS ||
                !member(entry_object, "protocol_id", protocol_element) ||
                protocol_element.get_int64().get(protocol_id) !=
                    simdjson::SUCCESS) {
                return std::unexpected{parse_fail(
                    "registries.json", "protocol_id missing: " + entry.name)};
            }
            entry.protocol_id = static_cast<std::int32_t>(protocol_id);
            registry.entries.push_back(std::move(entry));
        }
        out.push_back(std::move(registry));
    }
    return status_ok();
}

auto parse_item_defaults(std::string_view json, item_spec &out) -> status {
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (const auto error = parser.parse(json).get(document);
        error != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("item components", simdjson::error_message(error))};
    }
    simdjson::dom::object root;
    if (document.get_object().get(root) != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("item components", "root is not an object")};
    }
    simdjson::dom::element components_element;
    if (!member(root, "components", components_element)) {
        return status_ok();
    }
    simdjson::dom::object components;
    if (components_element.get_object().get(components) != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("item components", "components is not an object")};
    }
    simdjson::dom::element value;
    if (member(components, "minecraft:max_stack_size", value)) {
        std::int64_t stack_size = 0;
        if (value.get_int64().get(stack_size) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(
                "item components", "max_stack_size is not an integer")};
        }
        if (stack_size < 1 || stack_size > 99) {
            return std::unexpected{parse_fail(
                "item components", "max_stack_size is out of range")};
        }
        out.max_stack_size = static_cast<std::uint16_t>(stack_size);
    }
    if (member(components, "minecraft:max_damage", value)) {
        std::int64_t max_damage = 0;
        if (value.get_int64().get(max_damage) != simdjson::SUCCESS ||
            max_damage < 0) {
            return std::unexpected{parse_fail(
                "item components", "max_damage is not a non-negative integer")};
        }
        out.max_damage = static_cast<std::uint32_t>(max_damage);
    }
    return status_ok();
}

/// Hash of one report document; the importer records every input it read.
auto hash_text(std::string_view text) -> std::string {
    return to_hex(
        blake3(std::as_bytes(std::span<const char>{text.data(), text.size()})));
}

auto manifest_json(const vanilla_import_options &options,
                   const vanilla_import_result &result,
                   std::span<const std::pair<std::string, std::string>> inputs)
    -> std::string {
    json::value::array_type files;
    json::value::object_type table_file;
    table_file.emplace("path", json::value{"tables.rvtb"});
    table_file.emplace(
        "bytes", json::value{static_cast<std::int64_t>(result.tables_bytes)});
    table_file.emplace("blake3", json::value{result.tables_digest});
    files.emplace_back(json::value{std::move(table_file)});

    json::value::array_type input_array;
    for (const auto &[name, digest] : inputs) {
        json::value::object_type entry;
        entry.emplace("path", json::value{name});
        entry.emplace("blake3", json::value{digest});
        input_array.emplace_back(json::value{std::move(entry)});
    }

    json::value::object_type root;
    root.emplace("format", json::value{"refrain.vanilla-tables/1"});
    root.emplace("format_version",
                 json::value{static_cast<std::int64_t>(vanilla_table_version)});
    root.emplace("game_version", json::value{options.game_version});
    root.emplace("data_version",
                 json::value{static_cast<std::int64_t>(options.data_version)});
    root.emplace("generator", json::value{"refrain-tools vanilla import"});
    root.emplace("tables", json::value{std::move(files)});
    root.emplace("inputs", json::value{std::move(input_array)});
    return json::value{std::move(root)}.dump_pretty();
}

/// Parses the tracked block light report: per-state emission, dampening and
/// occlusion shape references, plus the six face box lists of each shape.
///
/// The report is generated from the user's own jar by the light probe and is
/// game-derived data. Every count and id is validated here; the table builder
/// then checks that the records cover every state of the block list report.
auto parse_block_light(std::string_view json, std::int32_t data_version,
                       std::vector<light_shape_spec> &shapes,
                       std::vector<block_light_spec> &lights) -> result<void> {
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (const auto error = parser.parse(json).get(document);
        error != simdjson::SUCCESS) {
        return std::unexpected{parse_fail("block_light/light.json",
                                          simdjson::error_message(error))};
    }
    simdjson::dom::object root;
    if (document.get_object().get(root) != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("block_light/light.json", "root is not an object")};
    }
    simdjson::dom::element field;
    std::string_view format;
    if (!member(root, "format", field) ||
        field.get_string().get(format) != simdjson::SUCCESS ||
        format != "refrain.block-light/1") {
        return std::unexpected{
            parse_fail("block_light/light.json", "unexpected report format")};
    }
    std::int64_t version = 0;
    if (!member(root, "data_version", field) ||
        field.get_int64().get(version) != simdjson::SUCCESS ||
        static_cast<std::int32_t>(version) != data_version) {
        return std::unexpected{
            parse_fail("block_light/light.json", "data version mismatch")};
    }
    std::int64_t state_count = 0;
    std::int64_t shape_count = 0;
    if (!member(root, "state_count", field) ||
        field.get_int64().get(state_count) != simdjson::SUCCESS ||
        state_count < 0 || state_count > 0xFFFFFFFFLL) {
        return std::unexpected{
            parse_fail("block_light/light.json", "state count invalid")};
    }
    if (!member(root, "shape_count", field) ||
        field.get_int64().get(shape_count) != simdjson::SUCCESS ||
        shape_count < 0 || shape_count > 0xFFFFLL) {
        return std::unexpected{
            parse_fail("block_light/light.json", "shape count invalid")};
    }

    simdjson::dom::array shape_array;
    if (!member(root, "shapes", field) ||
        field.get_array().get(shape_array) != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("block_light/light.json", "shapes is not an array")};
    }
    if (static_cast<std::int64_t>(shape_array.size()) != shape_count) {
        return std::unexpected{
            parse_fail("block_light/light.json", "shape count does not match")};
    }
    shapes.clear();
    shapes.reserve(static_cast<std::size_t>(shape_count));
    for (const auto shape_element : shape_array) {
        simdjson::dom::array face_array;
        if (shape_element.get_array().get(face_array) != simdjson::SUCCESS ||
            face_array.size() != light_shape_faces) {
            return std::unexpected{parse_fail(
                "block_light/light.json", "shape does not carry six faces")};
        }
        light_shape_spec shape;
        std::size_t face_index = 0;
        for (const auto face_element : face_array) {
            simdjson::dom::array box_array;
            if (face_element.get_array().get(box_array) != simdjson::SUCCESS) {
                return std::unexpected{parse_fail("block_light/light.json",
                                                  "face is not a box array")};
            }
            if (box_array.size() > max_light_boxes_per_face) {
                return std::unexpected{parse_fail(
                    "block_light/light.json", "face carries too many boxes")};
            }
            for (const auto box_element : box_array) {
                simdjson::dom::array coordinate_array;
                if (box_element.get_array().get(coordinate_array) !=
                        simdjson::SUCCESS ||
                    coordinate_array.size() != 6) {
                    return std::unexpected{
                        parse_fail("block_light/light.json",
                                   "box is not six coordinates")};
                }
                std::array<double, 6> bounds{};
                std::size_t coordinate = 0;
                for (const auto value : coordinate_array) {
                    const auto parsed = as_double(value);
                    if (!parsed.has_value() || !std::isfinite(*parsed)) {
                        return std::unexpected{parse_fail(
                            "block_light/light.json",
                            "box coordinate is not a finite number")};
                    }
                    bounds[coordinate++] = *parsed;
                }
                if (bounds[0] > bounds[3] || bounds[1] > bounds[4] ||
                    bounds[2] > bounds[5]) {
                    return std::unexpected{parse_fail("block_light/light.json",
                                                      "box has min above max")};
                }
                shape.faces[face_index].push_back(bounds);
            }
            ++face_index;
        }
        shapes.push_back(std::move(shape));
    }

    simdjson::dom::array state_array;
    if (!member(root, "states", field) ||
        field.get_array().get(state_array) != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail("block_light/light.json", "states is not an array")};
    }
    if (static_cast<std::int64_t>(state_array.size()) != state_count) {
        return std::unexpected{
            parse_fail("block_light/light.json", "state count does not match")};
    }
    lights.clear();
    lights.reserve(static_cast<std::size_t>(state_count));
    std::int64_t previous_id = -1;
    for (const auto state_element : state_array) {
        simdjson::dom::array record;
        if (state_element.get_array().get(record) != simdjson::SUCCESS ||
            record.size() != 5) {
            return std::unexpected{parse_fail(
                "block_light/light.json", "state record is not five values")};
        }
        std::array<std::int64_t, 5> values{};
        std::size_t value_index = 0;
        for (const auto value : record) {
            if (value.get_int64().get(values[value_index]) !=
                simdjson::SUCCESS) {
                return std::unexpected{
                    parse_fail("block_light/light.json",
                               "state record value is not an integer")};
            }
            ++value_index;
        }
        const auto id = values[0];
        if (id <= previous_id || id > 0xFFFF) {
            return std::unexpected{
                parse_fail("block_light/light.json",
                           "state ids are not dense and ascending")};
        }
        previous_id = id;
        if (values[1] < 0 || values[1] > max_light_level || values[2] < 0 ||
            values[2] > max_light_level) {
            return std::unexpected{parse_fail("block_light/light.json",
                                              "light level out of range")};
        }
        if (values[3] < 0 || (values[3] & ~3LL) != 0 || values[4] < -1 ||
            values[4] >= shape_count) {
            return std::unexpected{parse_fail("block_light/light.json",
                                              "state record flags or shape "
                                              "out of range")};
        }
        block_light_spec light;
        light.vanilla_id = static_cast<std::uint16_t>(id);
        light.emission = static_cast<std::uint8_t>(values[1]);
        light.dampening = static_cast<std::uint8_t>(values[2]);
        light.can_occlude = (values[3] & 1) != 0;
        light.use_shape_for_light_occlusion = (values[3] & 2) != 0;
        light.shape = values[4] < 0 ? no_light_shape
                                    : static_cast<std::uint32_t>(values[4]);
        lights.push_back(light);
    }
    return status_ok();
}

/// Parses one string member of a report document.
auto parse_text(const simdjson::dom::element &element, std::string_view file,
                std::string_view what, std::string &out) -> result<void> {
    std::string_view text;
    if (element.get_string().get(text) != simdjson::SUCCESS || text.empty()) {
        return std::unexpected{parse_fail(file, std::string{what})};
    }
    out = std::string{text};
    return status_ok();
}

/// Parses one integer member of a report document into `[lower, upper]`.
auto parse_integer(const simdjson::dom::element &element, std::string_view file,
                   std::string_view what, std::int64_t lower,
                   std::int64_t upper, std::int64_t &out) -> result<void> {
    if (element.get_int64().get(out) != simdjson::SUCCESS || out < lower ||
        out > upper) {
        return std::unexpected{parse_fail(file, std::string{what})};
    }
    return status_ok();
}

/// Parses one finite number member of a report document.
auto parse_number(const simdjson::dom::element &element, std::string_view file,
                  std::string_view what, double &out) -> result<void> {
    const auto value = as_double(element);
    if (!value.has_value() || !std::isfinite(*value)) {
        return std::unexpected{parse_fail(file, std::string{what})};
    }
    out = *value;
    return status_ok();
}

auto tool_type_from_name(std::string_view name) -> std::optional<tool_type> {
    if (name == "pickaxe") {
        return tool_type::pickaxe;
    }
    if (name == "axe") {
        return tool_type::axe;
    }
    if (name == "shovel") {
        return tool_type::shovel;
    }
    if (name == "hoe") {
        return tool_type::hoe;
    }
    if (name == "shears") {
        return tool_type::shears;
    }
    if (name == "sword") {
        return tool_type::sword;
    }
    if (name == "none") {
        return tool_type::none;
    }
    return std::nullopt;
}

auto tool_material_from_name(std::string_view name)
    -> std::optional<tool_material> {
    if (name == "wood") {
        return tool_material::wood;
    }
    if (name == "stone") {
        return tool_material::stone;
    }
    if (name == "copper") {
        return tool_material::copper;
    }
    if (name == "iron") {
        return tool_material::iron;
    }
    if (name == "diamond") {
        return tool_material::diamond;
    }
    if (name == "netherite") {
        return tool_material::netherite;
    }
    if (name == "gold") {
        return tool_material::gold;
    }
    return std::nullopt;
}

/// Parses the tracked block mining report: per-block mining records, the tool
/// table and the tool rules and block tags they were derived from.
///
/// The report is generated from the user's own jar by the mining probe and is
/// game-derived data. The importer uses the per-block records and the tool
/// table; the rules and tags are parsed so a corrupt report cannot reach the
/// container, and every count, id and enum is validated here.
auto parse_block_mining(
    std::string_view json, std::int32_t data_version,
    std::vector<std::pair<std::string, block_mining_spec>> &minings,
    std::vector<tool_mining_spec> &tools, std::int64_t &state_count)
    -> result<void> {
    constexpr std::string_view file = "block_mining/mining.json";
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (const auto error = parser.parse(json).get(document);
        error != simdjson::SUCCESS) {
        return std::unexpected{
            parse_fail(file, simdjson::error_message(error))};
    }
    simdjson::dom::object root;
    if (document.get_object().get(root) != simdjson::SUCCESS) {
        return std::unexpected{parse_fail(file, "root is not an object")};
    }
    simdjson::dom::element field;
    std::string format;
    if (!member(root, "format", field) ||
        !parse_text(field, file, "report format", format) ||
        format != "refrain.block-mining/1") {
        return std::unexpected{parse_fail(file, "unexpected report format")};
    }
    std::int64_t version = 0;
    if (!member(root, "data_version", field) ||
        !parse_integer(field, file, "data version", 0, 0x7FFFFFFF, version) ||
        static_cast<std::int32_t>(version) != data_version) {
        return std::unexpected{parse_fail(file, "data version mismatch")};
    }

    std::int64_t block_count = 0;
    if (!member(root, "block_count", field) ||
        !parse_integer(field, file, "block count", 0, 0xFFFF, block_count)) {
        return std::unexpected{parse_fail(file, "block count invalid")};
    }
    std::int64_t tool_count = 0;
    if (!member(root, "tool_count", field) ||
        !parse_integer(field, file, "tool count", 0, 0xFFFFFFFFLL,
                       tool_count)) {
        return std::unexpected{parse_fail(file, "tool count invalid")};
    }
    if (!member(root, "state_count", field) ||
        !parse_integer(field, file, "state count", 0, 0xFFFFFFFFLL,
                       state_count)) {
        return std::unexpected{parse_fail(file, "state count invalid")};
    }

    // The report names the families and materials in the order the container
    // uses; a different order would silently move every bit.
    static constexpr std::array<std::string_view, 6> tool_type_names{
        "pickaxe", "axe", "shovel", "hoe", "shears", "sword"};
    simdjson::dom::array type_array;
    if (!member(root, "tool_types", field) ||
        field.get_array().get(type_array) != simdjson::SUCCESS ||
        type_array.size() != tool_type_names.size()) {
        return std::unexpected{parse_fail(file, "tool family list invalid")};
    }
    std::size_t type_index = 0;
    for (const auto element : type_array) {
        std::string name;
        if (auto parsed = parse_text(element, file, "tool family", name);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        if (name != tool_type_names[type_index]) {
            return std::unexpected{
                parse_fail(file, "tool family list out of order")};
        }
        ++type_index;
    }

    static constexpr std::array<std::string_view, 7> material_names{
        "wood", "stone", "copper", "iron", "diamond", "netherite", "gold"};
    simdjson::dom::array material_array;
    if (!member(root, "materials", field) ||
        field.get_array().get(material_array) != simdjson::SUCCESS ||
        material_array.size() != material_names.size()) {
        return std::unexpected{parse_fail(file, "material list invalid")};
    }
    std::map<std::string, std::uint8_t, std::less<>> material_levels;
    std::size_t material_index = 0;
    for (const auto element : material_array) {
        simdjson::dom::object entry;
        if (element.get_object().get(entry) != simdjson::SUCCESS) {
            return std::unexpected{
                parse_fail(file, "material is not an object")};
        }
        simdjson::dom::element name_element;
        std::string name;
        if (!member(entry, "name", name_element) ||
            !parse_text(name_element, file, "material name", name)) {
            return std::unexpected{parse_fail(file, "material name invalid")};
        }
        if (name != material_names[material_index]) {
            return std::unexpected{
                parse_fail(file, "material list out of order")};
        }
        ++material_index;
        std::int64_t level = 0;
        simdjson::dom::element level_element;
        if (!member(entry, "harvest_level", level_element) ||
            !parse_integer(level_element, file, "material harvest level", 0,
                           max_harvest_level, level)) {
            return std::unexpected{
                parse_fail(file, "material harvest level invalid")};
        }
        if (!material_levels.emplace(name, static_cast<std::uint8_t>(level))
                 .second) {
            return std::unexpected{parse_fail(file, "duplicate material")};
        }
    }

    simdjson::dom::array block_array;
    if (!member(root, "blocks", field) ||
        field.get_array().get(block_array) != simdjson::SUCCESS ||
        static_cast<std::int64_t>(block_array.size()) != block_count) {
        return std::unexpected{parse_fail(file, "block table invalid")};
    }
    std::unordered_map<std::string, std::size_t> mining_index;
    minings.clear();
    minings.reserve(static_cast<std::size_t>(block_count));
    for (const auto element : block_array) {
        simdjson::dom::array row;
        if (element.get_array().get(row) != simdjson::SUCCESS ||
            row.size() != 8) {
            return std::unexpected{parse_fail(file, "block row invalid")};
        }
        auto iterator = row.begin();
        std::string name;
        if (auto parsed = parse_text(*iterator, file, "block name", name);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        ++iterator;
        double hardness = 0.0;
        if (auto parsed =
                parse_number(*iterator, file, "block hardness", hardness);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        const auto hardness_value = static_cast<float>(hardness);
        if (!(hardness_value == -1.0F || hardness_value >= 0.0F)) {
            return std::unexpected{parse_fail(file, "block hardness invalid")};
        }
        ++iterator;
        std::int64_t flags = 0;
        if (auto parsed = parse_integer(*iterator, file, "block flags", 0,
                                        (block_mining_requires_correct_tool |
                                         block_mining_air | block_mining_fluid),
                                        flags);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        ++iterator;
        std::int64_t mask = 0;
        if (auto parsed = parse_integer(*iterator, file, "block tool mask", 0,
                                        all_tool_type_bits, mask);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        ++iterator;
        std::int64_t needed = 0;
        if (auto parsed = parse_integer(*iterator, file, "block needed tier", 0,
                                        max_harvest_level, needed);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        ++iterator;
        double shears_speed = 0.0;
        if (auto parsed = parse_number(*iterator, file, "block shears speed",
                                       shears_speed);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        if (shears_speed < 0.0) {
            return std::unexpected{
                parse_fail(file, "block shears speed invalid")};
        }
        ++iterator;
        double sword_speed = 0.0;
        if (auto parsed =
                parse_number(*iterator, file, "block sword speed", sword_speed);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        if (sword_speed < 0.0) {
            return std::unexpected{
                parse_fail(file, "block sword speed invalid")};
        }
        ++iterator;
        std::int64_t direct = 0;
        if (auto parsed =
                parse_integer(*iterator, file, "block direct-correct mask", 0,
                              all_tool_type_bits, direct);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        block_mining_spec spec;
        spec.hardness = hardness_value;
        spec.shears_speed = static_cast<float>(shears_speed);
        spec.sword_speed = static_cast<float>(sword_speed);
        spec.flags = static_cast<std::uint8_t>(flags);
        spec.tool_mask = static_cast<std::uint8_t>(mask);
        spec.needed_tier = static_cast<std::uint8_t>(needed);
        spec.direct_correct = static_cast<std::uint8_t>(direct);
        mining_index.emplace(name, minings.size());
        minings.emplace_back(std::move(name), spec);
    }
    if (minings.size() != static_cast<std::size_t>(block_count)) {
        return std::unexpected{parse_fail(file, "duplicate block record")};
    }

    // The report also carries the block tags its rules reference; they are
    // provenance for the packed values, so the importer only checks that every
    // name is a block of this content version.
    simdjson::dom::object tags;
    if (!member(root, "tags", field) ||
        field.get_object().get(tags) != simdjson::SUCCESS) {
        return std::unexpected{parse_fail(file, "tags is not an object")};
    }
    std::set<std::string> tag_names;
    for (const auto tag_field : tags) {
        if (tag_field.key.empty() || !tag_names.emplace(tag_field.key).second) {
            return std::unexpected{parse_fail(file, "tag name invalid")};
        }
        simdjson::dom::array values;
        if (tag_field.value.get_array().get(values) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(file, "tag entries invalid")};
        }
        std::string_view previous;
        for (const auto value : values) {
            std::string_view name;
            if (value.get_string().get(name) != simdjson::SUCCESS ||
                name.empty()) {
                return std::unexpected{parse_fail(file, "tag entry invalid")};
            }
            if (!previous.empty() && name <= previous) {
                return std::unexpected{
                    parse_fail(file, "tag entries are not sorted")};
            }
            previous = name;
            if (mining_index.find(std::string{name}) == mining_index.end()) {
                return std::unexpected{
                    parse_fail(file, "tag names an unknown block")};
            }
        }
    }

    simdjson::dom::array tool_array;
    if (!member(root, "tools", field) ||
        field.get_array().get(tool_array) != simdjson::SUCCESS ||
        static_cast<std::int64_t>(tool_array.size()) != tool_count) {
        return std::unexpected{parse_fail(file, "tool table invalid")};
    }
    tools.clear();
    tools.reserve(static_cast<std::size_t>(tool_count));
    std::set<std::string> tool_names;
    for (const auto element : tool_array) {
        simdjson::dom::object entry;
        if (element.get_object().get(entry) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(file, "tool is not an object")};
        }
        simdjson::dom::element id_element;
        std::string id;
        if (!member(entry, "id", id_element) ||
            !parse_text(id_element, file, "tool id", id)) {
            return std::unexpected{parse_fail(file, "tool id invalid")};
        }
        if (!tool_names.insert(id).second) {
            return std::unexpected{parse_fail(file, "duplicate tool")};
        }
        simdjson::dom::element type_element;
        std::string type_name;
        if (!member(entry, "tool_type", type_element) ||
            !parse_text(type_element, file, "tool family", type_name)) {
            return std::unexpected{parse_fail(file, "tool family invalid")};
        }
        const auto type = tool_type_from_name(type_name);
        if (!type.has_value()) {
            return std::unexpected{parse_fail(file, "unknown tool family")};
        }
        tool_mining_spec spec;
        spec.name = std::move(id);
        spec.type = *type;
        const bool tiered = static_cast<std::uint8_t>(spec.type) <=
                            static_cast<std::uint8_t>(tool_type::hoe);
        if (tiered) {
            simdjson::dom::element material_element;
            std::string material_name;
            if (!member(entry, "material", material_element) ||
                !parse_text(material_element, file, "tool material",
                            material_name)) {
                return std::unexpected{
                    parse_fail(file, "tool material invalid")};
            }
            const auto material = tool_material_from_name(material_name);
            if (!material.has_value()) {
                return std::unexpected{
                    parse_fail(file, "unknown tool material")};
            }
            spec.material = *material;
            simdjson::dom::element level_element;
            std::int64_t level = 0;
            if (!member(entry, "harvest_level", level_element) ||
                !parse_integer(level_element, file, "tool harvest level", 0,
                               max_harvest_level, level)) {
                return std::unexpected{
                    parse_fail(file, "tool harvest level invalid")};
            }
            const auto known = material_levels.find(material_name);
            if (known == material_levels.end() ||
                known->second != static_cast<std::uint8_t>(level)) {
                return std::unexpected{
                    parse_fail(file, "tool harvest level mismatch")};
            }
            spec.harvest_level = static_cast<std::uint8_t>(level);
            simdjson::dom::element speed_element;
            double speed = 0.0;
            if (!member(entry, "tier_speed", speed_element) ||
                !parse_number(speed_element, file, "tool tier speed", speed) ||
                speed <= 0.0) {
                return std::unexpected{
                    parse_fail(file, "tool tier speed invalid")};
            }
            spec.tier_speed = static_cast<float>(speed);
        } else {
            for (const auto name :
                 {"material", "harvest_level", "tier_speed"}) {
                simdjson::dom::element ignored;
                if (member(entry, name, ignored)) {
                    return std::unexpected{
                        parse_fail(file, "a non-tiered tool names a material")};
                }
            }
        }
        simdjson::dom::element value_element;
        double default_speed = 0.0;
        if (!member(entry, "default_mining_speed", value_element) ||
            !parse_number(value_element, file, "tool default speed",
                          default_speed) ||
            default_speed < 0.0) {
            return std::unexpected{
                parse_fail(file, "tool default speed invalid")};
        }
        spec.default_mining_speed = static_cast<float>(default_speed);
        std::int64_t damage = 0;
        if (!member(entry, "damage_per_block", value_element) ||
            !parse_integer(value_element, file, "tool damage per block", 0,
                           0xFFFF, damage)) {
            return std::unexpected{
                parse_fail(file, "tool damage per block invalid")};
        }
        spec.damage_per_block = static_cast<std::uint16_t>(damage);
        bool creative = false;
        if (!member(entry, "can_destroy_blocks_in_creative", value_element) ||
            value_element.get_bool().get(creative) != simdjson::SUCCESS) {
            return std::unexpected{
                parse_fail(file, "tool creative flag invalid")};
        }
        spec.can_destroy_blocks_in_creative = creative;
        std::int64_t max_damage = 0;
        if (!member(entry, "max_damage", value_element) ||
            !parse_integer(value_element, file, "tool max damage", 0,
                           0xFFFFFFFFLL, max_damage)) {
            return std::unexpected{parse_fail(file, "tool max damage invalid")};
        }
        spec.max_damage = static_cast<std::uint32_t>(max_damage);
        std::int64_t max_stack = 0;
        if (!member(entry, "max_stack_size", value_element) ||
            !parse_integer(value_element, file, "tool stack size", 1, 99,
                           max_stack)) {
            return std::unexpected{parse_fail(file, "tool stack size invalid")};
        }
        spec.max_stack_size = static_cast<std::uint16_t>(max_stack);

        // The ordered rules are the source of the compressed fields. The
        // container does not store them, but a report with a malformed rule
        // list must not be accepted.
        simdjson::dom::array rules;
        if (!member(entry, "rules", value_element) ||
            value_element.get_array().get(rules) != simdjson::SUCCESS) {
            return std::unexpected{parse_fail(file, "tool rules invalid")};
        }
        for (const auto rule_element : rules) {
            simdjson::dom::object rule;
            if (rule_element.get_object().get(rule) != simdjson::SUCCESS) {
                return std::unexpected{
                    parse_fail(file, "tool rule is not an object")};
            }
            simdjson::dom::element blocks_element;
            const bool has_tag = member(rule, "tag", blocks_element);
            const bool has_blocks = member(rule, "blocks", blocks_element);
            if (has_tag == has_blocks) {
                return std::unexpected{
                    parse_fail(file, "tool rule names no block set")};
            }
            if (has_tag) {
                std::string tag;
                if (auto parsed =
                        parse_text(blocks_element, file, "tool rule tag", tag);
                    !parsed) {
                    return std::unexpected{parsed.error()};
                }
                if (tag_names.find(tag) == tag_names.end()) {
                    return std::unexpected{
                        parse_fail(file, "tool rule names an unknown tag")};
                }
            } else {
                simdjson::dom::array blocks;
                if (blocks_element.get_array().get(blocks) !=
                    simdjson::SUCCESS) {
                    return std::unexpected{
                        parse_fail(file, "tool rule blocks invalid")};
                }
                for (const auto block_element : blocks) {
                    std::string_view block;
                    if (block_element.get_string().get(block) !=
                            simdjson::SUCCESS ||
                        block.empty() ||
                        mining_index.find(std::string{block}) ==
                            mining_index.end()) {
                        return std::unexpected{
                            parse_fail(file, "tool rule block invalid")};
                    }
                }
            }
            simdjson::dom::element speed_element;
            if (member(rule, "speed", speed_element)) {
                double speed = 0.0;
                if (auto parsed = parse_number(speed_element, file,
                                               "tool rule speed", speed);
                    !parsed) {
                    return std::unexpected{parsed.error()};
                }
                if (speed <= 0.0) {
                    return std::unexpected{
                        parse_fail(file, "tool rule speed invalid")};
                }
            }
            simdjson::dom::element correct_element;
            bool correct = false;
            if (member(rule, "correct_for_drops", correct_element) &&
                correct_element.get_bool().get(correct) != simdjson::SUCCESS) {
                return std::unexpected{
                    parse_fail(file, "tool rule correct flag invalid")};
            }
        }
        tools.push_back(std::move(spec));
    }
    return status_ok();
}

} // namespace

auto build_vanilla_from_reports(const vanilla_reports &reports,
                                std::int32_t data_version)
    -> result<std::vector<std::byte>> {
    vanilla_spec spec;
    spec.data_version = data_version;
    if (auto parsed = parse_blocks(reports.blocks_json, spec.blocks); !parsed) {
        return std::unexpected{parsed.error()};
    }
    for (const auto &[name, json] : reports.biome_presets) {
        biome_preset_spec preset;
        if (auto parsed = parse_biomes(name, json, preset); !parsed) {
            return std::unexpected{parsed.error()};
        }
        spec.biome_presets.push_back(std::move(preset));
    }
    if (auto parsed =
            parse_registries(reports.registries_json, spec.registries);
        !parsed) {
        return std::unexpected{parsed.error()};
    }
    if (!reports.block_light_json.empty()) {
        if (auto parsed =
                parse_block_light(reports.block_light_json, data_version,
                                  spec.light_shapes, spec.block_lights);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
    }
    if (!reports.block_mining_json.empty()) {
        std::vector<std::pair<std::string, block_mining_spec>> minings;
        std::int64_t state_count = 0;
        if (auto parsed =
                parse_block_mining(reports.block_mining_json, data_version,
                                   minings, spec.tools, state_count);
            !parsed) {
            return std::unexpected{parsed.error()};
        }
        std::int64_t blocks_state_count = 0;
        for (const auto &block : spec.blocks) {
            blocks_state_count +=
                static_cast<std::int64_t>(block.states.size());
        }
        if (state_count != blocks_state_count) {
            return std::unexpected{
                parse_fail("block_mining/mining.json", "state count mismatch")};
        }
        if (minings.size() != spec.blocks.size()) {
            return std::unexpected{
                parse_fail("block_mining/mining.json",
                           "the mining records do not cover every block")};
        }
        std::unordered_map<std::string_view, const block_mining_spec *> by_name;
        by_name.reserve(minings.size());
        for (const auto &[name, mining] : minings) {
            by_name.emplace(name, &mining);
        }
        spec.block_minings.reserve(spec.blocks.size());
        for (const auto &block : spec.blocks) {
            const auto found = by_name.find(block.name);
            if (found == by_name.end()) {
                return std::unexpected{
                    parse_fail("block_mining/mining.json",
                               "no mining record for block " + block.name)};
            }
            spec.block_minings.push_back(*found->second);
        }
    }
    spec.items.reserve(reports.item_components.size());
    for (const auto &[name, json] : reports.item_components) {
        item_spec item;
        item.name = name;
        if (auto parsed = parse_item_defaults(json, item); !parsed) {
            return std::unexpected{parsed.error()};
        }
        spec.items.push_back(std::move(item));
    }
    return build_vanilla_tables(spec);
}

auto import_vanilla_reports(const vanilla_import_options &options)
    -> result<vanilla_import_result> {
    if (options.reports.empty()) {
        return std::unexpected{fail("reports directory is required")};
    }
    if (options.out.empty()) {
        return std::unexpected{fail("output directory is required")};
    }
    if (!io::is_directory(options.reports)) {
        return std::unexpected{
            fail("reports directory does not exist: " + options.reports)};
    }

    auto blocks = io::read_text(options.reports + "/blocks.json");
    if (!blocks) {
        return std::unexpected{blocks.error()};
    }
    auto registries = io::read_text(options.reports + "/registries.json");
    if (!registries) {
        return std::unexpected{registries.error()};
    }

    vanilla_reports reports;
    reports.blocks_json = std::move(*blocks);
    reports.registries_json = std::move(*registries);

    std::vector<std::pair<std::string, std::string>> inputs;
    inputs.emplace_back("blocks.json", hash_text(reports.blocks_json));
    inputs.emplace_back("registries.json", hash_text(reports.registries_json));

    const auto light_path = options.reports + "/block_light/light.json";
    if (io::is_regular_file(light_path)) {
        auto light = io::read_text(light_path);
        if (!light) {
            return std::unexpected{light.error()};
        }
        reports.block_light_json = std::move(*light);
        inputs.emplace_back("block_light/light.json",
                            hash_text(reports.block_light_json));
    }

    const auto mining_path = options.reports + "/block_mining/mining.json";
    if (io::is_regular_file(mining_path)) {
        auto mining = io::read_text(mining_path);
        if (!mining) {
            return std::unexpected{mining.error()};
        }
        reports.block_mining_json = std::move(*mining);
        inputs.emplace_back("block_mining/mining.json",
                            hash_text(reports.block_mining_json));
    }

    const auto biome_root = options.reports + "/biome_parameters";
    if (!io::is_directory(biome_root)) {
        return std::unexpected{
            fail("missing biome_parameters directory: " + biome_root)};
    }
    auto namespaces = io::list_directory(biome_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    for (const auto &ns : *namespaces) {
        const auto directory = biome_root + "/" + ns;
        if (!io::is_directory(directory)) {
            continue;
        }
        auto files = io::list_directory(directory);
        if (!files) {
            return std::unexpected{files.error()};
        }
        for (const auto &file : *files) {
            if (!file.ends_with(".json")) {
                continue;
            }
            const auto path = directory + "/" + file;
            auto text = io::read_text(path);
            if (!text) {
                return std::unexpected{text.error()};
            }
            reports.biome_presets.emplace_back(
                ns + ":" + file.substr(0, file.size() - 5), std::move(*text));
            inputs.emplace_back("biome_parameters/" + ns + "/" + file,
                                hash_text(reports.biome_presets.back().second));
        }
    }
    if (reports.biome_presets.empty()) {
        return std::unexpected{fail("no biome parameter presets found")};
    }

    // Per-item default components live under <reports>/<namespace>/components/
    // item/<item>.json; the file name is the item path inside minecraft:item.
    const auto item_root = options.reports + "/minecraft/components/item";
    if (!io::is_directory(item_root)) {
        return std::unexpected{
            fail("missing item component directory: " + item_root)};
    }
    auto item_files = io::list_directory(item_root);
    if (!item_files) {
        return std::unexpected{item_files.error()};
    }
    for (const auto &file : *item_files) {
        if (!file.ends_with(".json")) {
            continue;
        }
        const auto path = item_root + "/" + file;
        auto text = io::read_text(path);
        if (!text) {
            return std::unexpected{text.error()};
        }
        reports.item_components.emplace_back(
            "minecraft:" + file.substr(0, file.size() - 5), std::move(*text));
        inputs.emplace_back("minecraft/components/item/" + file,
                            hash_text(reports.item_components.back().second));
    }
    if (reports.item_components.empty()) {
        return std::unexpected{fail("no item component reports found")};
    }

    auto bytes = build_vanilla_from_reports(reports, options.data_version);
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }

    if (auto created = io::create_directories(options.out); !created) {
        return std::unexpected{created.error()};
    }
    auto tables = vanilla_tables::open(*bytes);
    if (!tables) {
        return std::unexpected{tables.error()};
    }

    vanilla_import_result result;
    result.blocks = tables->block_count();
    result.states = tables->state_count();
    result.presets = tables->preset_count();
    result.registries = tables->registry_count();
    result.items = tables->item_count();
    result.light_shapes = tables->light_shape_count();
    result.tools = tables->tool_count();
    result.tables_path = options.out + "/tables.rvtb";
    result.manifest_path = options.out + "/MANIFEST.json";
    result.tables_bytes = bytes->size();
    result.tables_digest = to_hex(blake3(std::span<const std::byte>{*bytes}));

    if (!options.force &&
        (io::exists(result.tables_path) || io::exists(result.manifest_path))) {
        return std::unexpected{
            fail("output already exists (use --force): " + options.out)};
    }
    if (auto written = io::atomic_write_file(result.tables_path, *bytes);
        !written) {
        return std::unexpected{written.error()};
    }
    const auto manifest = manifest_json(options, result, inputs);
    if (auto written = io::atomic_write_text(result.manifest_path, manifest);
        !written) {
        return std::unexpected{written.error()};
    }
    return result;
}

} // namespace refrain::data
