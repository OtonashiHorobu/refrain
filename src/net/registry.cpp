// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/registry.hpp>

#include <refrain/data/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::net {
namespace {

namespace json = refrain::data::json;

[[nodiscard]] auto field_string(const json::value &object, std::string_view key,
                                std::string_view where) -> result<std::string> {
    const auto *value{object.find(key)};
    if (value == nullptr || !value->is_string()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("{}: '{}' is missing or not a string", where, key))};
    }
    const auto text{value->as_string()};
    if (!text) {
        return std::unexpected{std::move(text.error())};
    }
    return std::string{*text};
}

[[nodiscard]] auto field_id(const json::value &object, std::string_view key,
                            std::string_view where) -> result<std::uint32_t> {
    const auto *value{object.find(key)};
    if (value == nullptr || !value->is_integer()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("{}: '{}' is missing or not an integer", where, key))};
    }
    const auto number{value->as_int64()};
    if (!number) {
        return std::unexpected{std::move(number.error())};
    }
    if (*number < 0 || static_cast<std::uint64_t>(*number) >
                           std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("{}: '{}' is outside the u32 range", where, key))};
    }
    return static_cast<std::uint32_t>(*number);
}

} // namespace

auto registry_catalog::load(std::string_view path) -> result<registry_catalog> {
    auto document{json::value::parse_file(path)};
    if (!document) {
        return std::unexpected{std::move(document.error())};
    }
    if (!document->is_object()) {
        return std::unexpected{make_error(
            error_code::parse_error, "registry catalog root is not an object")};
    }

    registry_catalog catalog{};
    const auto *registries{document->find("registries")};
    if (registries == nullptr || !registries->is_array()) {
        return std::unexpected{make_error(
            error_code::parse_error, "registry catalog has no registry list")};
    }
    const auto registry_array{registries->as_array()};
    if (!registry_array) {
        return std::unexpected{std::move(registry_array.error())};
    }
    for (const auto &registry_value : *registry_array) {
        if (!registry_value.is_object()) {
            return std::unexpected{make_error(
                error_code::parse_error, "registry entry is not an object")};
        }
        auto key{field_string(registry_value, "registry", "registry")};
        if (!key) {
            return std::unexpected{std::move(key.error())};
        }
        registry_definition definition{};
        definition.key = std::move(*key);
        const auto *entries{registry_value.find("entries")};
        if (entries == nullptr || !entries->is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("registry '{}' has no entries", definition.key))};
        }
        const auto entry_array{entries->as_array()};
        if (!entry_array) {
            return std::unexpected{std::move(entry_array.error())};
        }
        definition.entries.reserve(entry_array->size());
        for (const auto &entry_value : *entry_array) {
            if (!entry_value.is_object()) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    fmt::format("registry '{}' entry is not an object",
                                definition.key))};
            }
            auto name{field_string(entry_value, "name", definition.key)};
            auto id{field_id(entry_value, "id", definition.key)};
            if (!name) {
                return std::unexpected{std::move(name.error())};
            }
            if (!id) {
                return std::unexpected{std::move(id.error())};
            }
            definition.entries.push_back(
                registry_entry{.name = std::move(*name), .id = *id});
        }
        catalog.registries_.push_back(std::move(definition));
    }

    const auto *tags{document->find("tags")};
    if (tags != nullptr) {
        if (!tags->is_array()) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "registry catalog tags is not a list")};
        }
        const auto tag_array{tags->as_array()};
        if (!tag_array) {
            return std::unexpected{std::move(tag_array.error())};
        }
        for (const auto &tag_value : *tag_array) {
            auto registry{field_string(tag_value, "registry", "tag")};
            auto tag{field_string(tag_value, "tag", "tag")};
            if (!registry) {
                return std::unexpected{std::move(registry.error())};
            }
            if (!tag) {
                return std::unexpected{std::move(tag.error())};
            }
            tag_definition definition{.registry = std::move(*registry),
                                      .tag = std::move(*tag)};
            const auto *entries{tag_value.find("entries")};
            if (entries != nullptr && entries->is_array()) {
                const auto entry_array{entries->as_array()};
                if (!entry_array) {
                    return std::unexpected{std::move(entry_array.error())};
                }
                definition.entries.reserve(entry_array->size());
                for (const auto &id_value : *entry_array) {
                    if (!id_value.is_integer()) {
                        return std::unexpected{make_error(
                            error_code::parse_error,
                            fmt::format("tag '{}' has a non-integer id",
                                        definition.tag))};
                    }
                    const auto id{id_value.as_int64()};
                    if (!id || *id < 0 ||
                        static_cast<std::uint64_t>(*id) >
                            std::numeric_limits<std::uint32_t>::max()) {
                        return std::unexpected{make_error(
                            error_code::parse_error,
                            fmt::format("tag '{}' has an out-of-range id",
                                        definition.tag))};
                    }
                    definition.entries.push_back(
                        static_cast<std::uint32_t>(*id));
                }
            }
            catalog.tags_.push_back(std::move(definition));
        }
    }
    return catalog;
}

} // namespace refrain::net
