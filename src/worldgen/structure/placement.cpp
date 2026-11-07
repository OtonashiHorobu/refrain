// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/placement.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <memory>

namespace refrain::worldgen::structure {

namespace {

auto floor_div(jint value, jint divisor) -> jint {
    const jint quotient = value / divisor;
    const jint remainder = value % divisor;
    return remainder != 0 && ((remainder < 0) != (divisor < 0)) ? quotient - 1
                                                                : quotient;
}

} // namespace

auto random_spread_placement::potential_chunk(jlong seed, jint chunk_x,
                                              jint chunk_z) const -> chunk_pos {
    const jint grid_x = floor_div(chunk_x, spacing);
    const jint grid_z = floor_div(chunk_z, spacing);
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_with_salt(seed, grid_x, grid_z, salt);
    const jint limit = spacing - separation;
    const auto spread_coordinate = [&]() {
        if (spread == spread_type::linear) {
            return random.next_int(limit);
        }
        return (random.next_int(limit) + random.next_int(limit)) / 2;
    };
    const jint spread_x = spread_coordinate();
    const jint spread_z = spread_coordinate();
    return chunk_pos{.x = grid_x * spacing + spread_x,
                     .z = grid_z * spacing + spread_z};
}

auto random_spread_placement::is_structure_chunk(jlong seed, jint chunk_x,
                                                 jint chunk_z) const -> bool {
    const auto chunk = potential_chunk(seed, chunk_x, chunk_z);
    if (chunk.x != chunk_x || chunk.z != chunk_z) {
        return false;
    }
    return passes_frequency(seed, chunk_x, chunk_z);
}

auto random_spread_placement::passes_frequency(jlong seed, jint chunk_x,
                                               jint chunk_z) const -> bool {
    if (frequency >= 1.0F) {
        return true;
    }
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    switch (frequency_method) {
    case frequency_reduction_method::default_method:
        random.set_large_feature_with_salt(seed, salt, chunk_x, chunk_z);
        return random.next_float() < frequency;
    case frequency_reduction_method::legacy_type_1:
        random.set_large_feature_seed(seed, chunk_x, chunk_z);
        return random.next_double() < static_cast<double>(frequency);
    case frequency_reduction_method::legacy_type_2:
        random.set_large_feature_with_salt(seed, chunk_x, chunk_z, 10387320);
        return random.next_float() < frequency;
    case frequency_reduction_method::legacy_type_3: {
        const jint cx = chunk_x >> 4;
        const jint cz = chunk_z >> 4;
        random.set_seed(static_cast<jlong>(cx ^ (cz << 4) ^ seed));
        (void)random.next_int();
        return random.next_int(static_cast<jint>(1.0F / frequency)) == 0;
    }
    }
    return false;
}

auto decode_random_spread(const data::json::value &document)
    -> result<random_spread_placement> {
    const auto *spacing_value = document.find("spacing");
    const auto *separation_value = document.find("separation");
    const auto *salt_value = document.find("salt");
    if (spacing_value == nullptr || separation_value == nullptr ||
        salt_value == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "random spread is incomplete")};
    }
    const auto spacing = spacing_value->as_int64();
    const auto separation = separation_value->as_int64();
    const auto salt = salt_value->as_int64();
    if (!spacing || !separation || !salt) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "random spread numbers are invalid")};
    }
    if (*spacing <= *separation) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "spacing must exceed separation")};
    }
    random_spread_placement placement{.spacing = static_cast<jint>(*spacing),
                                      .separation =
                                          static_cast<jint>(*separation),
                                      .salt = static_cast<jint>(*salt)};
    if (const auto *frequency = document.find("frequency");
        frequency != nullptr) {
        const auto value = frequency->as_double();
        if (!value) {
            return std::unexpected{value.error()};
        }
        placement.frequency = static_cast<float>(*value);
    }
    if (const auto *method = document.find("frequency_reduction_method");
        method != nullptr) {
        const auto name = method->as_string();
        if (!name) {
            return std::unexpected{name.error()};
        }
        if (*name == "minecraft:legacy_type_1" || *name == "legacy_type_1") {
            placement.frequency_method =
                frequency_reduction_method::legacy_type_1;
        } else if (*name == "minecraft:legacy_type_2" ||
                   *name == "legacy_type_2") {
            placement.frequency_method =
                frequency_reduction_method::legacy_type_2;
        } else if (*name == "minecraft:legacy_type_3" ||
                   *name == "legacy_type_3") {
            placement.frequency_method =
                frequency_reduction_method::legacy_type_3;
        }
    }
    if (const auto *type = document.find("spread_type"); type != nullptr) {
        const auto name = type->as_string();
        if (!name) {
            return std::unexpected{name.error()};
        }
        if (*name == "minecraft:triangular" || *name == "triangular") {
            placement.spread = spread_type::triangular;
        } else if (*name != "minecraft:linear" && *name != "linear") {
            return std::unexpected{
                make_error(error_code::unsupported, "unknown spread type")};
        }
    }
    return placement;
}

} // namespace refrain::worldgen::structure
