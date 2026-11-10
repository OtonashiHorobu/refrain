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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
                        std::find(block.property_names.begin(),
                                  block.property_names.end(),
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
