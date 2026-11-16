// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/datafix.hpp>

#include "registry.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::world {
namespace {

namespace nbt = data::nbt;
namespace detail = datafix_detail;

[[nodiscard]] auto failing(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

} // namespace

auto datafix_type_name(datafix_type type) noexcept -> std::string_view {
    switch (type) {
    case datafix_type::level:
        return "level";
    case datafix_type::player:
        return "player";
    case datafix_type::chunk:
        return "chunk";
    case datafix_type::entity_chunk:
        return "entity chunk";
    case datafix_type::poi_chunk:
        return "poi chunk";
    case datafix_type::world_gen_settings:
        return "world generation settings";
    case datafix_type::saved_data:
        return "saved data";
    case datafix_type::structure:
        return "structure";
    case datafix_type::advancements:
        return "advancements";
    case datafix_type::stats:
        return "statistics";
    }
    return "unknown record";
}

auto saved_data_type(std::string_view file_name) noexcept -> datafix_type {
    if (file_name == "world_gen_settings.dat" ||
        file_name.ends_with("/world_gen_settings.dat")) {
        return datafix_type::world_gen_settings;
    }
    return datafix_type::saved_data;
}

auto datafix_step_count(std::int32_t from_version,
                        std::int32_t to_version) noexcept -> std::uint32_t {
    std::uint32_t count{0};
    for (const auto &step : detail::fix_steps()) {
        if (step.schema_version > from_version &&
            step.schema_version <= to_version) {
            ++count;
        }
    }
    return count;
}

auto plan_datafix(std::int32_t from_version, std::int32_t to_version)
    -> result<datafix_plan> {
    if (from_version > to_version) {
        return failing(error_code::invalid_argument,
                       fmt::format("cannot downgrade a data fix request from "
                                   "data version {} to {}",
                                   from_version, to_version));
    }
    if (to_version != datafix_target_version) {
        return failing(
            error_code::unsupported,
            fmt::format("data fix target version {} is not supported; this "
                        "build applies fixes up to {}",
                        to_version, datafix_target_version));
    }
    if (from_version < oldest_migratable_data_version) {
        return failing(
            error_code::unsupported,
            fmt::format("data version {} is older than the supported data fix "
                        "window [{}, {}]; upgrade the record in Minecraft 26.3 "
                        "before reading it here",
                        from_version, oldest_migratable_data_version,
                        datafix_target_version));
    }
    return datafix_plan{
        .from_version = from_version,
        .to_version = to_version,
        .steps = datafix_step_count(from_version, to_version),
    };
}

auto fix_document(nbt::value &document, datafix_type type,
                  std::int32_t from_version, std::int32_t to_version)
    -> result<datafix_result> {
    if (document.as_compound() == nullptr) {
        return failing(error_code::parse_error,
                       fmt::format("cannot fix a {} record: the document root "
                                   "is not a compound",
                                   datafix_type_name(type)));
    }
    auto plan{plan_datafix(from_version, to_version)};
    if (!plan) {
        return failing(plan.error().code,
                       fmt::format("cannot fix a {} record: {}",
                                   datafix_type_name(type),
                                   plan.error().message));
    }
    const auto types{detail::type_bit(type)};
    datafix_result result{};
    for (const auto &step : detail::fix_steps()) {
        if (step.schema_version <= from_version ||
            step.schema_version > to_version) {
            continue;
        }
        if ((step.types & types) == 0U) {
            continue;
        }
        if (step.apply(document)) {
            result.changed = true;
            ++result.fixes_applied;
        }
    }
    return result;
}

auto stamp_data_version(nbt::value &document, std::int32_t data_version)
    -> status {
    if (document.as_compound() == nullptr) {
        return failing(error_code::invalid_argument,
                       "cannot stamp a data version on a record that is not a "
                       "compound");
    }
    return document.set("DataVersion", nbt::make_int(data_version));
}

} // namespace refrain::world
