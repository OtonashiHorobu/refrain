// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/entity_nbt.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

[[nodiscard]] auto parse_failure(std::string message)
    -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
}

/// Reads one numeric NBT scalar as a double.
[[nodiscard]] auto number_at(const data::nbt::value &value)
    -> std::optional<double> {
    if (const auto real{value.as_double()}; real.has_value()) {
        return *real;
    }
    if (const auto real{value.as_float()}; real.has_value()) {
        return static_cast<double>(*real);
    }
    if (const auto integer{value.as_int()}; integer.has_value()) {
        return static_cast<double>(*integer);
    }
    if (const auto integer{value.as_long()}; integer.has_value()) {
        return static_cast<double>(*integer);
    }
    if (const auto integer{value.as_short()}; integer.has_value()) {
        return static_cast<double>(*integer);
    }
    if (const auto integer{value.as_byte()}; integer.has_value()) {
        return static_cast<double>(*integer);
    }
    return std::nullopt;
}

/// Reads one numeric NBT scalar as an int32.
[[nodiscard]] auto integer_at(const data::nbt::value &value)
    -> std::optional<std::int32_t> {
    if (const auto integer{value.as_int()}; integer.has_value()) {
        return *integer;
    }
    if (const auto integer{value.as_short()}; integer.has_value()) {
        return static_cast<std::int32_t>(*integer);
    }
    if (const auto integer{value.as_byte()}; integer.has_value()) {
        return static_cast<std::int32_t>(*integer);
    }
    if (const auto integer{value.as_long()}; integer.has_value()) {
        if (*integer >= std::numeric_limits<std::int32_t>::min() &&
            *integer <= std::numeric_limits<std::int32_t>::max()) {
            return static_cast<std::int32_t>(*integer);
        }
    }
    if (const auto real{value.as_double()}; real.has_value()) {
        if (std::isfinite(*real) && *real >= -2147483648.0 &&
            *real <= 2147483647.0) {
            return static_cast<std::int32_t>(*real);
        }
    }
    return std::nullopt;
}

} // namespace

auto entity_from_nbt(const data::nbt::compound &tag) -> result<entity_record> {
    entity_record record{};
    const auto type{tag.find("id")};
    if (type == tag.end()) {
        return parse_failure("entity record has no 'id'");
    }
    const auto key{type->second.as_string()};
    if (!key.has_value() || key->empty()) {
        return parse_failure("entity record has a non-string or empty 'id'");
    }
    record.id = std::string{*key};

    const auto pos{tag.find("Pos")};
    if (pos == tag.end()) {
        return parse_failure(
            fmt::format("entity '{}' has no 'Pos'", record.id));
    }
    const auto *position{pos->second.as_list()};
    if (position == nullptr || position->items.size() != 3) {
        return parse_failure(
            fmt::format("entity '{}' has a malformed 'Pos'", record.id));
    }
    std::array<double, 3> coordinates{};
    for (std::size_t index{0}; index < coordinates.size(); ++index) {
        const auto number{number_at(position->items[index])};
        if (!number.has_value()) {
            return parse_failure(
                fmt::format("entity '{}' has a non-numeric 'Pos'", record.id));
        }
        coordinates[index] = *number;
    }
    record.x = coordinates[0];
    record.y = coordinates[1];
    record.z = coordinates[2];

    if (const auto rotation{tag.find("Rotation")}; rotation != tag.end()) {
        const auto *list{rotation->second.as_list()};
        if (list == nullptr || list->items.size() < 2) {
            return parse_failure(fmt::format(
                "entity '{}' has a malformed 'Rotation'", record.id));
        }
        const auto yaw{number_at(list->items[0])};
        const auto pitch{number_at(list->items[1])};
        if (!yaw.has_value() || !pitch.has_value()) {
            return parse_failure(fmt::format(
                "entity '{}' has a non-numeric 'Rotation'", record.id));
        }
        record.yaw = static_cast<float>(*yaw);
        record.pitch = static_cast<float>(*pitch);
    }

    if (const auto block{tag.find("block_pos")}; block != tag.end()) {
        // The reference writes `block_pos` with `BlockPos.CODEC`, which is an
        // int array; a template-independent producer may write an int list.
        std::optional<std::array<std::int32_t, 3>> corners{};
        if (const auto *list{block->second.as_list()}; list != nullptr) {
            if (list->items.size() == 3) {
                const auto x{integer_at(list->items[0])};
                const auto y{integer_at(list->items[1])};
                const auto z{integer_at(list->items[2])};
                if (x.has_value() && y.has_value() && z.has_value()) {
                    corners = std::array<std::int32_t, 3>{*x, *y, *z};
                }
            }
        } else if (const auto array{block->second.as_int_array()};
                   array.has_value() && array->size() == 3) {
            corners = std::array<std::int32_t, 3>{(*array)[0], (*array)[1],
                                                  (*array)[2]};
        }
        if (!corners.has_value() ||
            (*corners)[1] < std::numeric_limits<std::int16_t>::min() ||
            (*corners)[1] > std::numeric_limits<std::int16_t>::max()) {
            return parse_failure(fmt::format(
                "entity '{}' has a malformed 'block_pos'", record.id));
        }
        record.pos = block_pos{.x = (*corners)[0],
                               .y = static_cast<std::int16_t>((*corners)[1]),
                               .z = (*corners)[2]};
    } else {
        // The reference derives an entity's block position from its position
        // when the tag carries none.
        if (!std::isfinite(record.x) || !std::isfinite(record.y) ||
            !std::isfinite(record.z) || std::fabs(record.x) > 3.0e7 ||
            std::fabs(record.y) > 3.0e7 || std::fabs(record.z) > 3.0e7) {
            return parse_failure(fmt::format(
                "entity '{}' has an unrepresentable position", record.id));
        }
        const auto floor_y{static_cast<std::int64_t>(std::floor(record.y))};
        record.pos =
            block_pos{.x = static_cast<std::int32_t>(std::floor(record.x)),
                      .y = static_cast<std::int16_t>(std::clamp<std::int64_t>(
                          floor_y, std::numeric_limits<std::int16_t>::min(),
                          std::numeric_limits<std::int16_t>::max())),
                      .z = static_cast<std::int32_t>(std::floor(record.z))};
    }

    data::nbt::compound components{tag};
    components.erase("id");
    components.erase("Pos");
    components.erase("Rotation");
    components.erase("block_pos");
    auto bytes{data::nbt::write(data::nbt::value{std::move(components)}, "",
                                data::nbt::compression::none)};
    if (!bytes) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("entity '{}' components are not encodable: {}",
                        record.id, bytes.error().message))};
    }
    record.data = std::move(*bytes);
    return record;
}

auto entity_to_nbt(const entity_record &record) -> result<data::nbt::compound> {
    data::nbt::compound components{};
    if (!record.data.empty()) {
        auto document{data::nbt::read(record.data)};
        if (!document) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("entity '{}' components are not valid NBT: {}",
                            record.id, document.error().message))};
        }
        auto *parsed{document->as_compound()};
        if (parsed == nullptr) {
            return parse_failure(fmt::format(
                "entity '{}' components are not a compound", record.id));
        }
        components = std::move(*parsed);
    }
    components["id"] = data::nbt::make_string(record.id);
    components["Pos"] = data::nbt::make_list(
        data::nbt::tag_type::double_,
        {data::nbt::make_double(record.x), data::nbt::make_double(record.y),
         data::nbt::make_double(record.z)});
    components["Rotation"] = data::nbt::make_list(
        data::nbt::tag_type::float_, {data::nbt::make_float(record.yaw),
                                      data::nbt::make_float(record.pitch)});
    components["block_pos"] =
        data::nbt::make_int_array({record.pos.x, record.pos.y, record.pos.z});
    return components;
}

} // namespace refrain::world
