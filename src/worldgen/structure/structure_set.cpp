// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/structure_set.hpp>

#include <refrain/data/json.hpp>

#include <utility>

namespace refrain::worldgen::structure {

auto decode_structure_set(std::string_view id,
                          const data::json::value &document)
    -> result<structure_set> {
    const auto *structures = document.find("structures");
    const auto *placement = document.find("placement");
    if (structures == nullptr || !structures->is_array() ||
        placement == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "structure set is incomplete")};
    }
    structure_set decoded{.id = std::string{id}};
    for (const auto &entry : *structures->as_array()) {
        const auto *structure = entry.find("structure");
        const auto *weight = entry.find("weight");
        if (structure == nullptr || weight == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "structure entry is incomplete")};
        }
        const auto structure_id = structure->as_string();
        const auto weight_value = weight->as_int64();
        if (!structure_id || !weight_value) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "structure entry is invalid")};
        }
        decoded.structures.push_back(
            weighted_structure{.structure = std::string{*structure_id},
                               .weight = static_cast<jint>(*weight_value)});
    }
    const auto *type = placement->find("type");
    if (type == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "structure placement has no type")};
    }
    const auto type_name = type->as_string();
    if (!type_name) {
        return std::unexpected{type_name.error()};
    }
    if (*type_name == "minecraft:random_spread") {
        auto spread = decode_random_spread(*placement);
        if (!spread) {
            return std::unexpected{spread.error()};
        }
        decoded.random_spread = std::move(*spread);
    } else if (*type_name != "minecraft:concentric_rings") {
        return std::unexpected{make_error(error_code::unsupported,
                                          "unsupported structure placement")};
    }
    return decoded;
}

} // namespace refrain::worldgen::structure
