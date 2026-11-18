// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "structure_nbt_internal.hpp"

#include <refrain/data/nbt.hpp>
#include <refrain/world/model.hpp>
#include <refrain/worldgen/structure/placement_types.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::world::detail {
namespace {

namespace nbt = data::nbt;
namespace structure = worldgen::structure;

using nbt::tag_type;
using nbt::value;

/// Per-column bounds. Vanilla keeps one start per structure in a column and a
/// structure start has on the order of a hundred pieces; the limits leave
/// room for a large datapack while keeping a corrupt or hostile payload from
/// allocating without bound.
constexpr std::size_t max_starts_per_column = 512;
constexpr std::size_t max_pieces_per_start = 8192;
constexpr std::size_t max_pieces_per_column = 32768;
constexpr std::size_t max_references_per_column = 65536;
constexpr std::size_t max_piece_depth = 16;
constexpr std::size_t max_nested_elements = 4096;

auto parse_failure(std::string message) -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
}

auto child(const nbt::compound &entries, std::string_view key)
    -> const value * {
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : &found->second;
}

// --- transform names ------------------------------------------------------

constexpr std::array<std::string_view, 4> rotation_names{
    "NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"};
constexpr std::array<std::string_view, 3> mirror_names{"NONE", "LEFT_RIGHT",
                                                       "FRONT_BACK"};

auto rotation_name(std::int32_t rotation) -> std::optional<std::string_view> {
    if (rotation < 0 ||
        static_cast<std::size_t>(rotation) >= rotation_names.size()) {
        return std::nullopt;
    }
    return rotation_names[static_cast<std::size_t>(rotation)];
}

auto mirror_name(std::int32_t mirror) -> std::optional<std::string_view> {
    if (mirror < 0 || static_cast<std::size_t>(mirror) >= mirror_names.size()) {
        return std::nullopt;
    }
    return mirror_names[static_cast<std::size_t>(mirror)];
}

auto parse_rotation(std::string_view text) -> std::optional<std::int32_t> {
    for (std::size_t index = 0; index < rotation_names.size(); ++index) {
        if (rotation_names[index] == text) {
            return static_cast<std::int32_t>(index);
        }
    }
    return std::nullopt;
}

auto parse_mirror(std::string_view text) -> std::optional<std::int32_t> {
    for (std::size_t index = 0; index < mirror_names.size(); ++index) {
        if (mirror_names[index] == text) {
            return static_cast<std::int32_t>(index);
        }
    }
    return std::nullopt;
}

// --- piece families -------------------------------------------------------

enum class piece_family : std::uint8_t {
    simple,
    template_piece,
    ruined_portal,
    mineshaft,
    stronghold,
    fortress,
    monument,
    jigsaw,
};

/// The vanilla piece registry id a record is stored under, and the family that
/// decides which additional keys the tag carries.
struct piece_identity {
    piece_family family{piece_family::simple};
    std::string_view vanilla_id{};
};

auto fortress_vanilla_id(structure::fortress_piece_kind kind)
    -> std::optional<std::string_view> {
    switch (kind) {
    case structure::fortress_piece_kind::bridge_crossing:
        return "minecraft:nebcr";
    case structure::fortress_piece_kind::bridge_end_filler:
        return "minecraft:nebef";
    case structure::fortress_piece_kind::bridge_straight:
        return "minecraft:nebs";
    case structure::fortress_piece_kind::castle_corridor_stairs:
        return "minecraft:neccs";
    case structure::fortress_piece_kind::castle_corridor_t_balcony:
        return "minecraft:nectb";
    case structure::fortress_piece_kind::castle_entrance:
        return "minecraft:nece";
    case structure::fortress_piece_kind::castle_small_corridor:
        return "minecraft:nesc";
    case structure::fortress_piece_kind::castle_small_corridor_crossing:
        return "minecraft:nescsc";
    case structure::fortress_piece_kind::castle_small_corridor_left_turn:
        return "minecraft:nesclt";
    case structure::fortress_piece_kind::castle_small_corridor_right_turn:
        return "minecraft:nescrt";
    case structure::fortress_piece_kind::castle_stalk_room:
        return "minecraft:necsr";
    case structure::fortress_piece_kind::monster_throne:
        return "minecraft:nemt";
    case structure::fortress_piece_kind::room_crossing:
        return "minecraft:nerc";
    case structure::fortress_piece_kind::stairs_room:
        return "minecraft:nesr";
    }
    return std::nullopt;
}

auto fortress_kind_from_id(std::string_view id)
    -> std::optional<structure::fortress_piece_kind> {
    using kind = structure::fortress_piece_kind;
    static constexpr std::array<std::pair<std::string_view, kind>, 15> table{{
        {"minecraft:nestart", kind::bridge_crossing},
        {"minecraft:nebcr", kind::bridge_crossing},
        {"minecraft:nebef", kind::bridge_end_filler},
        {"minecraft:nebs", kind::bridge_straight},
        {"minecraft:neccs", kind::castle_corridor_stairs},
        {"minecraft:nectb", kind::castle_corridor_t_balcony},
        {"minecraft:nece", kind::castle_entrance},
        {"minecraft:nesc", kind::castle_small_corridor},
        {"minecraft:nescsc", kind::castle_small_corridor_crossing},
        {"minecraft:nesclt", kind::castle_small_corridor_left_turn},
        {"minecraft:nescrt", kind::castle_small_corridor_right_turn},
        {"minecraft:necsr", kind::castle_stalk_room},
        {"minecraft:nemt", kind::monster_throne},
        {"minecraft:nerc", kind::room_crossing},
        {"minecraft:nesr", kind::stairs_room},
    }};
    for (const auto &[id_name, piece_kind] : table) {
        if (id_name == id) {
            return piece_kind;
        }
    }
    return std::nullopt;
}

auto monument_vanilla_id(structure::monument_piece_kind kind)
    -> std::optional<std::string_view> {
    using kind_t = structure::monument_piece_kind;
    switch (kind) {
    case kind_t::building:
        return "minecraft:omb";
    case kind_t::entry_room:
        return "minecraft:omentry";
    case kind_t::core_room:
        return "minecraft:omcr";
    case kind_t::double_x_room:
        return "minecraft:omdxr";
    case kind_t::double_xy_room:
        return "minecraft:omdxyr";
    case kind_t::double_y_room:
        return "minecraft:omdyr";
    case kind_t::double_yz_room:
        return "minecraft:omdyzr";
    case kind_t::double_z_room:
        return "minecraft:omdzr";
    case kind_t::simple_room:
        return "minecraft:omsimple";
    case kind_t::simple_top_room:
        return "minecraft:omsimplet";
    case kind_t::wing_room:
        return "minecraft:omwr";
    case kind_t::penthouse:
        return "minecraft:ompenthouse";
    }
    return std::nullopt;
}

auto monument_kind_from_id(std::string_view id)
    -> std::optional<structure::monument_piece_kind> {
    using kind = structure::monument_piece_kind;
    static constexpr std::array<std::pair<std::string_view, kind>, 12> table{{
        {"minecraft:omb", kind::building},
        {"minecraft:omentry", kind::entry_room},
        {"minecraft:omcr", kind::core_room},
        {"minecraft:omdxr", kind::double_x_room},
        {"minecraft:omdxyr", kind::double_xy_room},
        {"minecraft:omdyr", kind::double_y_room},
        {"minecraft:omdyzr", kind::double_yz_room},
        {"minecraft:omdzr", kind::double_z_room},
        {"minecraft:omsimple", kind::simple_room},
        {"minecraft:omsimplet", kind::simple_top_room},
        {"minecraft:omwr", kind::wing_room},
        {"minecraft:ompenthouse", kind::penthouse},
    }};
    for (const auto &[id_name, piece_kind] : table) {
        if (id_name == id) {
            return piece_kind;
        }
    }
    return std::nullopt;
}

auto stronghold_vanilla_id(structure::stronghold_piece_kind kind)
    -> std::optional<std::string_view> {
    using kind_t = structure::stronghold_piece_kind;
    switch (kind) {
    case kind_t::start:
        return "minecraft:shstart";
    case kind_t::filler_corridor:
        return "minecraft:shfc";
    case kind_t::chest_corridor:
        return "minecraft:shcc";
    case kind_t::five_crossing:
        return "minecraft:sh5c";
    case kind_t::left_turn:
        return "minecraft:shlt";
    case kind_t::right_turn:
        return "minecraft:shrt";
    case kind_t::library:
        return "minecraft:shli";
    case kind_t::portal_room:
        return "minecraft:shpr";
    case kind_t::prison_hall:
        return "minecraft:shph";
    case kind_t::room_crossing:
        return "minecraft:shrc";
    case kind_t::stairs_down:
        return "minecraft:shsd";
    case kind_t::straight:
        return "minecraft:shs";
    case kind_t::straight_stairs_down:
        return "minecraft:shssd";
    }
    return std::nullopt;
}

auto stronghold_kind_from_id(std::string_view id)
    -> std::optional<structure::stronghold_piece_kind> {
    using kind = structure::stronghold_piece_kind;
    static constexpr std::array<std::pair<std::string_view, kind>, 13> table{{
        {"minecraft:shstart", kind::start},
        {"minecraft:shfc", kind::filler_corridor},
        {"minecraft:shcc", kind::chest_corridor},
        {"minecraft:sh5c", kind::five_crossing},
        {"minecraft:shlt", kind::left_turn},
        {"minecraft:shrt", kind::right_turn},
        {"minecraft:shli", kind::library},
        {"minecraft:shpr", kind::portal_room},
        {"minecraft:shph", kind::prison_hall},
        {"minecraft:shrc", kind::room_crossing},
        {"minecraft:shsd", kind::stairs_down},
        {"minecraft:shs", kind::straight},
        {"minecraft:shssd", kind::straight_stairs_down},
    }};
    for (const auto &[id_name, piece_kind] : table) {
        if (id_name == id) {
            return piece_kind;
        }
    }
    return std::nullopt;
}

auto small_door_name(structure::stronghold_door door)
    -> std::optional<std::string_view> {
    switch (door) {
    case structure::stronghold_door::opening:
        return "OPENING";
    case structure::stronghold_door::wood_door:
        return "WOOD_DOOR";
    case structure::stronghold_door::grates:
        return "GRATES";
    case structure::stronghold_door::iron_door:
        return "IRON_DOOR";
    }
    return std::nullopt;
}

auto small_door_from_name(std::string_view name)
    -> std::optional<structure::stronghold_door> {
    if (name == "OPENING") {
        return structure::stronghold_door::opening;
    }
    if (name == "WOOD_DOOR") {
        return structure::stronghold_door::wood_door;
    }
    if (name == "GRATES") {
        return structure::stronghold_door::grates;
    }
    if (name == "IRON_DOOR") {
        return structure::stronghold_door::iron_door;
    }
    return std::nullopt;
}

auto mineshaft_vanilla_id(std::string_view internal)
    -> std::optional<std::string_view> {
    static constexpr std::array<std::string_view, 4> known{
        "minecraft:msroom", "minecraft:mscorridor", "minecraft:mscrossing",
        "minecraft:msstairs"};
    for (const auto id : known) {
        if (internal == id) {
            return id;
        }
    }
    return std::nullopt;
}

/// Classifies a generated piece record. The record type names the vanilla
/// piece for most families; the fortress and monument records carry the
/// per-kind id in their payload instead.
auto identify_piece(const worldgen::structure_piece_record &piece)
    -> std::optional<piece_identity> {
    if (piece.type == "minecraft:btp" || piece.type == "minecraft:tedp" ||
        piece.type == "minecraft:tejp" || piece.type == "minecraft:tesh") {
        return piece_identity{.family = piece_family::simple,
                              .vanilla_id = piece.type};
    }
    if (piece.type == "minecraft:igl") {
        return piece_identity{.family = piece_family::template_piece,
                              .vanilla_id = "minecraft:iglu"};
    }
    if (piece.type == "minecraft:shipwreck" ||
        piece.type == "minecraft:ocean_ruin" ||
        piece.type == "minecraft:woodland_mansion" ||
        piece.type == "minecraft:end_city" ||
        piece.type == "minecraft:nether_fossil") {
        static constexpr std::array<
            std::pair<std::string_view, std::string_view>, 5>
            ids{{
                {"minecraft:shipwreck", "minecraft:shipwreck"},
                {"minecraft:ocean_ruin", "minecraft:orp"},
                {"minecraft:woodland_mansion", "minecraft:wmp"},
                {"minecraft:end_city", "minecraft:ecp"},
                {"minecraft:nether_fossil", "minecraft:nefos"},
            }};
        for (const auto &[internal, vanilla] : ids) {
            if (internal == piece.type) {
                return piece_identity{.family = piece_family::template_piece,
                                      .vanilla_id = vanilla};
            }
        }
    }
    if (piece.type == "minecraft:ruined_portal") {
        return piece_identity{.family = piece_family::ruined_portal,
                              .vanilla_id = "minecraft:rupo"};
    }
    if (const auto id = mineshaft_vanilla_id(piece.type); id) {
        return piece_identity{.family = piece_family::mineshaft,
                              .vanilla_id = *id};
    }
    if (piece.type.starts_with("minecraft:sh") &&
        piece.type != "minecraft:shipwreck") {
        if (piece.stronghold) {
            if (const auto id = stronghold_vanilla_id(piece.stronghold->kind);
                id) {
                return piece_identity{.family = piece_family::stronghold,
                                      .vanilla_id = *id};
            }
        }
        return std::nullopt;
    }
    if (piece.type == "minecraft:fortress") {
        if (!piece.fortress) {
            return std::nullopt;
        }
        if (const auto id = fortress_vanilla_id(piece.fortress->kind); id) {
            return piece_identity{.family = piece_family::fortress,
                                  .vanilla_id = *id};
        }
        return std::nullopt;
    }
    if (piece.type == "minecraft:omb" ||
        piece.type.starts_with("minecraft:om")) {
        if (!piece.monument) {
            return std::nullopt;
        }
        if (const auto id = monument_vanilla_id(piece.monument->kind); id) {
            return piece_identity{.family = piece_family::monument,
                                  .vanilla_id = *id};
        }
        return std::nullopt;
    }
    if (piece.type == "minecraft:jigsaw") {
        if (!piece.jigsaw) {
            return std::nullopt;
        }
        return piece_identity{.family = piece_family::jigsaw,
                              .vanilla_id = "minecraft:jigsaw"};
    }
    return std::nullopt;
}

/// Bounded integer field of a compound; any tag of the wrong type is a
/// malformed record rather than a default.
auto int_field(const nbt::compound &entries, std::string_view key,
               std::optional<std::int32_t> fallback, std::string_view what)
    -> result<std::int32_t> {
    const auto *item = child(entries, key);
    if (item == nullptr) {
        if (fallback) {
            return *fallback;
        }
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("{} has no '{}' integer", what, key))};
    }
    if (const auto number = item->as_int(); number) {
        return *number;
    }
    return std::unexpected{
        make_error(error_code::parse_error,
                   fmt::format("{} '{}' is not an integer", what, key))};
}

auto bool_field(const nbt::compound &entries, std::string_view key,
                bool fallback, std::string_view what) -> result<bool> {
    const auto *item = child(entries, key);
    if (item == nullptr) {
        return fallback;
    }
    if (const auto number = item->as_byte(); number) {
        return *number != 0;
    }
    return std::unexpected{
        make_error(error_code::parse_error,
                   fmt::format("{} '{}' is not a byte", what, key))};
}

auto float_field(const nbt::compound &entries, std::string_view key,
                 float fallback, std::string_view what) -> result<float> {
    const auto *item = child(entries, key);
    if (item == nullptr) {
        return fallback;
    }
    if (const auto number = item->as_float(); number) {
        return *number;
    }
    return std::unexpected{
        make_error(error_code::parse_error,
                   fmt::format("{} '{}' is not a float", what, key))};
}

auto string_field(const nbt::compound &entries, std::string_view key,
                  std::optional<std::string_view> fallback,
                  std::string_view what) -> result<std::string> {
    const auto *item = child(entries, key);
    if (item == nullptr) {
        if (fallback) {
            return std::string{*fallback};
        }
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("{} has no '{}' string", what, key))};
    }
    if (const auto text = item->as_string(); text) {
        return std::string{*text};
    }
    return std::unexpected{
        make_error(error_code::parse_error,
                   fmt::format("{} '{}' is not a string", what, key))};
}

// --- encoding -------------------------------------------------------------

struct encode_state {
    std::size_t pieces{0};
};

auto common_piece_tag(const worldgen::structure_piece_record &piece,
                      piece_identity identity, std::int32_t gen_depth)
    -> result<value> {
    if (piece.min_x > piece.max_x || piece.min_y > piece.max_y ||
        piece.min_z > piece.max_z) {
        return parse_failure(
            fmt::format("{} piece {} has an inverted bounding box",
                        identity.vanilla_id, piece.type));
    }
    if (piece.orientation < -1 || piece.orientation > 3) {
        return parse_failure(fmt::format("{} piece {} has orientation {}",
                                         identity.vanilla_id, piece.type,
                                         piece.orientation));
    }
    nbt::compound tag{};
    tag["id"] = nbt::make_string(std::string{identity.vanilla_id});
    tag["BB"] = nbt::make_int_array(nbt::int_array{piece.min_x, piece.min_y,
                                                   piece.min_z, piece.max_x,
                                                   piece.max_y, piece.max_z});
    tag["O"] = nbt::make_int(piece.orientation);
    tag["GD"] = nbt::make_int(gen_depth);
    return nbt::value{std::move(tag)};
}

auto template_name(const worldgen::structure_piece_record &piece,
                   piece_identity identity) -> result<std::string> {
    const std::string_view prefix =
        identity.vanilla_id == "minecraft:wmp"   ? "minecraft:woodland_mansion/"
        : identity.vanilla_id == "minecraft:ecp" ? "minecraft:end_city/"
                                                 : "";
    if (prefix.empty()) {
        if (piece.template_id.empty()) {
            return parse_failure(fmt::format("{} piece has no template id",
                                             identity.vanilla_id));
        }
        return piece.template_id;
    }
    if (!piece.template_id.starts_with(prefix)) {
        return parse_failure(
            fmt::format("{} piece template id '{}' is outside '{}'",
                        identity.vanilla_id, piece.template_id, prefix));
    }
    return piece.template_id.substr(prefix.size());
}

auto encode_template_piece(nbt::compound &tag,
                           const worldgen::structure_piece_record &piece,
                           piece_identity identity) -> status {
    auto name = template_name(piece, identity);
    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    const auto rotation = rotation_name(piece.template_rotation);
    if (!rotation) {
        return parse_failure(fmt::format("{} piece has rotation {}",
                                         identity.vanilla_id,
                                         piece.template_rotation));
    }
    tag["Template"] = nbt::make_string(std::move(*name));
    tag["TPX"] = nbt::make_int(piece.template_x);
    tag["TPY"] = nbt::make_int(piece.template_y);
    tag["TPZ"] = nbt::make_int(piece.template_z);
    tag["Rot"] = nbt::make_string(std::string{*rotation});
    if (identity.vanilla_id == "minecraft:wmp") {
        const auto mirror = mirror_name(piece.template_mirror);
        if (!mirror) {
            return parse_failure(fmt::format(
                "woodland mansion piece has mirror {}", piece.template_mirror));
        }
        tag["Mi"] = nbt::make_string(std::string{*mirror});
    }
    if (identity.vanilla_id == "minecraft:ecp") {
        tag["OW"] = nbt::make_byte(piece.overwrite ? 1 : 0);
    }
    if (identity.vanilla_id == "minecraft:orp") {
        tag["Integrity"] = nbt::make_float(piece.integrity);
        tag["IsLarge"] = nbt::make_byte(piece.large ? 1 : 0);
        // The ruin keeps the material family it was generated with. The
        // reference stores it as an enum name rather than a path, and the
        // generated template path is the only record of it.
        tag["BiomeType"] = nbt::make_string(
            piece.template_id.find("warm") != std::string::npos ? "WARM"
                                                                : "COLD");
    }
    if (identity.vanilla_id == "minecraft:shipwreck") {
        tag["isBeached"] = nbt::make_byte(piece.beached ? 1 : 0);
        tag["height_adjusted"] = nbt::make_byte(0);
    }
    return status_ok();
}

auto encode_ruined_portal(nbt::compound &tag,
                          const worldgen::structure_piece_record &piece)
    -> status {
    if (!piece.portal_valid) {
        return parse_failure(
            "ruined portal piece has no portal properties to store");
    }
    static constexpr std::array<std::string_view, 6> placements{
        "on_land_surface", "partly_buried", "on_ocean_floor",
        "in_mountain",     "underground",   "in_nether"};
    if (std::ranges::find(placements, piece.portal_placement) ==
        placements.end()) {
        return parse_failure(
            fmt::format("ruined portal has vertical placement '{}'",
                        piece.portal_placement));
    }
    auto name = template_name(
        piece, piece_identity{.family = piece_family::template_piece,
                              .vanilla_id = "minecraft:rupo"});
    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    const auto rotation = rotation_name(piece.template_rotation);
    if (!rotation) {
        return parse_failure(fmt::format("ruined portal has rotation {}",
                                         piece.template_rotation));
    }
    const auto mirror = mirror_name(piece.template_mirror);
    if (!mirror) {
        return parse_failure(
            fmt::format("ruined portal has mirror {}", piece.template_mirror));
    }
    tag["Template"] = nbt::make_string(std::move(*name));
    tag["TPX"] = nbt::make_int(piece.template_x);
    tag["TPY"] = nbt::make_int(piece.template_y);
    tag["TPZ"] = nbt::make_int(piece.template_z);
    tag["Rotation"] = nbt::make_string(std::string{*rotation});
    tag["Mirror"] = nbt::make_string(std::string{*mirror});
    tag["VerticalPlacement"] = nbt::make_string(piece.portal_placement);
    nbt::compound properties{};
    properties["cold"] = nbt::make_byte(piece.portal_cold ? 1 : 0);
    properties["mossiness"] = nbt::make_float(piece.portal_mossiness);
    properties["air_pocket"] = nbt::make_byte(piece.portal_air_pocket ? 1 : 0);
    properties["overgrown"] = nbt::make_byte(piece.portal_overgrown ? 1 : 0);
    properties["vines"] = nbt::make_byte(piece.portal_vines ? 1 : 0);
    properties["replace_with_blackstone"] =
        nbt::make_byte(piece.portal_blackstone ? 1 : 0);
    tag["Properties"] = nbt::make_compound(std::move(properties));
    return status_ok();
}

auto entrances_value(const worldgen::structure_piece_record &piece)
    -> result<value> {
    auto list = nbt::make_list(tag_type::int_array);
    for (const auto &entrance : piece.entrances) {
        (void)list.push(nbt::make_int_array(
            nbt::int_array{entrance.min_x, entrance.min_y, entrance.min_z,
                           entrance.max_x, entrance.max_y, entrance.max_z}));
    }
    return list;
}

auto encode_mineshaft(nbt::compound &tag,
                      const worldgen::structure_piece_record &piece,
                      piece_identity identity) -> status {
    tag["MST"] = nbt::make_int(piece.mesa ? 1 : 0);
    if (identity.vanilla_id == "minecraft:msroom") {
        auto entrances = entrances_value(piece);
        if (!entrances) {
            return std::unexpected{std::move(entrances.error())};
        }
        tag["Entrances"] = std::move(*entrances);
    } else if (identity.vanilla_id == "minecraft:mscorridor") {
        tag["hr"] = nbt::make_byte(piece.rails ? 1 : 0);
        tag["sc"] = nbt::make_byte(piece.spider ? 1 : 0);
        tag["hps"] = nbt::make_byte(0);
        tag["Num"] = nbt::make_int(piece.sections);
    } else if (identity.vanilla_id == "minecraft:mscrossing") {
        tag["tf"] = nbt::make_byte(piece.two_floored ? 1 : 0);
        // The reference records the crossing direction separately from its
        // bounding-box orientation and the generated record does not keep it;
        // the loader falls back to south, which the placement never reads.
    }
    return status_ok();
}

auto encode_stronghold(nbt::compound &tag,
                       const worldgen::structure_piece_record &piece,
                       piece_identity identity) -> status {
    if (!piece.stronghold) {
        return parse_failure(fmt::format("{} piece has no stronghold payload",
                                         identity.vanilla_id));
    }
    const auto &data = *piece.stronghold;
    const auto door = small_door_name(data.door);
    if (!door) {
        return parse_failure("stronghold piece has an unknown door kind");
    }
    const auto id = identity.vanilla_id;
    tag["EntryDoor"] = nbt::make_string(std::string{*door});
    if (id == "minecraft:shfc") {
        tag["Steps"] = nbt::make_int(data.steps);
    } else if (id == "minecraft:sh5c") {
        tag["leftLow"] = nbt::make_byte(data.left_low ? 1 : 0);
        tag["leftHigh"] = nbt::make_byte(data.left_high ? 1 : 0);
        tag["rightLow"] = nbt::make_byte(data.right_low ? 1 : 0);
        tag["rightHigh"] = nbt::make_byte(data.right_high ? 1 : 0);
    } else if (id == "minecraft:shli") {
        tag["Tall"] = nbt::make_byte(data.tall ? 1 : 0);
    } else if (id == "minecraft:shpr") {
        tag["Mob"] = nbt::make_byte(0);
    } else if (id == "minecraft:shrc") {
        tag["Type"] = nbt::make_int(data.variant);
    } else if (id == "minecraft:shsd" || id == "minecraft:shstart") {
        tag["Source"] = nbt::make_byte(data.source ? 1 : 0);
    } else if (id == "minecraft:shs") {
        tag["Left"] = nbt::make_byte(data.left ? 1 : 0);
        tag["Right"] = nbt::make_byte(data.right ? 1 : 0);
    }
    return status_ok();
}

auto encode_fortress(nbt::compound &tag,
                     const worldgen::structure_piece_record &piece,
                     piece_identity identity) -> status {
    if (!piece.fortress) {
        return parse_failure(fmt::format("{} piece has no fortress payload",
                                         identity.vanilla_id));
    }
    const auto &data = *piece.fortress;
    if (identity.vanilla_id == "minecraft:nebef") {
        tag["Seed"] = nbt::make_int(static_cast<std::int32_t>(data.self_seed));
    } else if (identity.vanilla_id == "minecraft:nesclt" ||
               identity.vanilla_id == "minecraft:nescrt") {
        tag["Chest"] = nbt::make_byte(data.chest ? 1 : 0);
    }
    return status_ok();
}

auto encode_pool_element(const structure::jigsaw_element_info &element,
                         std::size_t depth, std::size_t &budget)
    -> result<value> {
    if (depth > max_piece_depth || budget == 0) {
        return parse_failure("jigsaw pool element nesting exceeds its bound");
    }
    --budget;
    if (element.inline_processors != nullptr) {
        return parse_failure(
            "jigsaw pool element carries an inline processor list, which the "
            "chunk structure compound cannot represent");
    }
    static constexpr std::array<
        std::pair<structure::pool_element_kind, std::string_view>, 5>
        kinds{{
            {structure::pool_element_kind::empty,
             "minecraft:empty_pool_element"},
            {structure::pool_element_kind::single,
             "minecraft:single_pool_element"},
            {structure::pool_element_kind::legacy_single,
             "minecraft:legacy_single_pool_element"},
            {structure::pool_element_kind::list, "minecraft:list_pool_element"},
            {structure::pool_element_kind::feature,
             "minecraft:feature_pool_element"},
        }};
    std::string_view element_type{};
    for (const auto &[kind, name] : kinds) {
        if (kind == element.kind) {
            element_type = name;
            break;
        }
    }
    if (element_type.empty()) {
        return parse_failure("jigsaw pool element has an unknown kind");
    }
    nbt::compound out{};
    out["element_type"] = nbt::make_string(std::string{element_type});
    const auto projection =
        element.projection == structure::pool_projection::terrain_matching
            ? "terrain_matching"
            : "rigid";
    const auto with_liquid =
        [](nbt::compound &entries,
           const std::optional<structure::liquid_settings> &liquid) {
            if (liquid.has_value()) {
                entries["override_liquid_settings"] = nbt::make_string(
                    *liquid == structure::liquid_settings::ignore_waterlogging
                        ? "ignore_waterlogging"
                        : "apply_waterlogging");
            }
        };
    switch (element.kind) {
    case structure::pool_element_kind::empty:
        return nbt::value{std::move(out)};
    case structure::pool_element_kind::single:
    case structure::pool_element_kind::legacy_single:
        out["location"] = nbt::make_string(element.template_id);
        out["processors"] = nbt::make_string(element.processor_list.empty()
                                                 ? "minecraft:empty"
                                                 : element.processor_list);
        out["projection"] = nbt::make_string(projection);
        with_liquid(out, element.liquid_override);
        return nbt::value{std::move(out)};
    case structure::pool_element_kind::feature:
        out["feature"] = nbt::make_string(element.feature);
        out["projection"] = nbt::make_string(projection);
        return nbt::value{std::move(out)};
    case structure::pool_element_kind::list: {
        auto children = nbt::make_list(tag_type::compound);
        for (const auto &child_element : element.elements) {
            auto encoded =
                encode_pool_element(child_element, depth + 1, budget);
            if (!encoded) {
                return std::unexpected{std::move(encoded.error())};
            }
            (void)children.push(std::move(*encoded));
        }
        out["elements"] = std::move(children);
        out["projection"] = nbt::make_string(projection);
        return nbt::value{std::move(out)};
    }
    }
    return parse_failure("jigsaw pool element has an unknown kind");
}

auto encode_jigsaw(nbt::compound &tag,
                   const worldgen::structure_piece_record &piece) -> status {
    if (!piece.jigsaw) {
        return parse_failure("jigsaw piece has no pool element payload");
    }
    const auto &data = *piece.jigsaw;
    const auto rotation = rotation_name(piece.template_rotation);
    if (!rotation) {
        return parse_failure(fmt::format("jigsaw piece has rotation {}",
                                         piece.template_rotation));
    }
    std::size_t budget = max_nested_elements;
    auto element = encode_pool_element(data.element, 0, budget);
    if (!element) {
        return std::unexpected{std::move(element.error())};
    }
    tag["PosX"] = nbt::make_int(piece.template_x);
    tag["PosY"] = nbt::make_int(piece.template_y);
    tag["PosZ"] = nbt::make_int(piece.template_z);
    tag["rotation"] = nbt::make_string(std::string{*rotation});
    tag["ground_level_delta"] = nbt::make_int(data.ground_level_delta);
    tag["pool_element"] = std::move(*element);
    auto junctions = nbt::make_list(tag_type::compound);
    for (const auto &junction : data.junctions) {
        nbt::compound entry{};
        entry["source_x"] = nbt::make_int(junction.source_x);
        entry["source_ground_y"] = nbt::make_int(junction.source_ground_y);
        entry["source_z"] = nbt::make_int(junction.source_z);
        entry["delta_y"] = nbt::make_int(junction.delta_y);
        entry["dest_proj"] = nbt::make_string(
            junction.dest_projection ==
                    structure::pool_projection::terrain_matching
                ? "terrain_matching"
                : "rigid");
        (void)junctions.push(nbt::make_compound(std::move(entry)));
    }
    tag["junctions"] = std::move(junctions);
    if (data.liquid != structure::liquid_settings::apply_waterlogging) {
        tag["liquid_settings"] = nbt::make_string("ignore_waterlogging");
    }
    return status_ok();
}

auto encode_piece_tag(const worldgen::structure_piece_record &piece,
                      bool first_of_start) -> result<value> {
    auto identity = identify_piece(piece);
    if (!identity) {
        return parse_failure(
            fmt::format("generated piece of type '{}' has no vanilla piece id",
                        piece.type));
    }
    if (first_of_start && identity->family == piece_family::fortress &&
        piece.fortress &&
        piece.fortress->kind ==
            structure::fortress_piece_kind::bridge_crossing) {
        // The first fortress piece is the reference's start piece, which has
        // its own registry id and the same behaviour as a bridge crossing.
        identity->vanilla_id = "minecraft:nestart";
    }
    const std::int32_t gen_depth =
        piece.stronghold ? piece.stronghold->gen_depth : 0;
    auto encoded = common_piece_tag(piece, *identity, gen_depth);
    if (!encoded) {
        return std::unexpected{std::move(encoded.error())};
    }
    auto &tag = *encoded->as_compound();
    // Both template-piece constructors force NORTH orientation; the reference
    // tag always carries O = 2 and the loader overwrites it. The effective
    // rotation is the subclass rotation key.
    if (identity->family == piece_family::template_piece ||
        identity->family == piece_family::ruined_portal) {
        tag["O"] = nbt::make_int(2);
    }
    switch (identity->family) {
    case piece_family::simple:
        if (piece.type == "minecraft:tedp" || piece.type == "minecraft:tejp" ||
            piece.type == "minecraft:tesh") {
            // The scattered-feature classes rebuild their bounding box from
            // these cached dimensions when they run again.
            const bool z_axis =
                piece.orientation == 0 || piece.orientation == 2;
            tag["Width"] =
                nbt::make_int(z_axis ? piece.max_x - piece.min_x + 1
                                     : piece.max_z - piece.min_z + 1);
            tag["Height"] = nbt::make_int(piece.max_y - piece.min_y + 1);
            tag["Depth"] =
                nbt::make_int(z_axis ? piece.max_z - piece.min_z + 1
                                     : piece.max_x - piece.min_x + 1);
            // A resolved height keeps the reference from moving the box when
            // the generated piece is placed again.
            tag["HPos"] =
                nbt::make_int(piece.height_position >= 0 ? piece.height_position
                                                         : piece.min_y);
        }
        break;
    case piece_family::template_piece:
        if (auto status = encode_template_piece(tag, piece, *identity);
            !status) {
            return std::unexpected{std::move(status.error())};
        }
        break;
    case piece_family::ruined_portal:
        if (auto status = encode_ruined_portal(tag, piece); !status) {
            return std::unexpected{std::move(status.error())};
        }
        break;
    case piece_family::mineshaft:
        if (auto status = encode_mineshaft(tag, piece, *identity); !status) {
            return std::unexpected{std::move(status.error())};
        }
        break;
    case piece_family::stronghold:
        if (auto status = encode_stronghold(tag, piece, *identity); !status) {
            return std::unexpected{std::move(status.error())};
        }
        break;
    case piece_family::fortress:
        if (auto status = encode_fortress(tag, piece, *identity); !status) {
            return std::unexpected{std::move(status.error())};
        }
        break;
    case piece_family::monument:
        // The monument piece tag carries only the common fields; the room
        // definition travels in the saved piece list order.
        if (!piece.monument) {
            return parse_failure("monument piece has no room payload");
        }
        break;
    case piece_family::jigsaw:
        if (auto status = encode_jigsaw(tag, piece); !status) {
            return std::unexpected{std::move(status.error())};
        }
        break;
    }
    // The reference saves monument children as the following entries of the
    // same piece list, so the child records are flattened into the list the
    // caller is building rather than nested in the parent tag.
    return encoded;
}

auto append_piece(const worldgen::structure_piece_record &piece,
                  std::size_t depth, bool first_of_start, encode_state &state,
                  nbt::value &children) -> status {
    if (depth > max_piece_depth) {
        return parse_failure("structure piece nesting exceeds its bound");
    }
    ++state.pieces;
    if (state.pieces > max_pieces_per_column) {
        return parse_failure(fmt::format(
            "structure data has more than {} pieces", max_pieces_per_column));
    }
    auto encoded = encode_piece_tag(piece, first_of_start);
    if (!encoded) {
        return std::unexpected{std::move(encoded.error())};
    }
    if (auto pushed = children.push(std::move(*encoded)); !pushed) {
        return std::unexpected{std::move(pushed.error())};
    }
    bool child_first = false;
    for (const auto &child : piece.children) {
        if (auto status =
                append_piece(child, depth + 1, child_first, state, children);
            !status) {
            return std::unexpected{std::move(status.error())};
        }
    }
    return status_ok();
}

auto encode_start(const worldgen::structure_start &start, std::int32_t chunk_x,
                  std::int32_t chunk_z, encode_state &state)
    -> result<std::pair<std::string, value>> {
    if (start.structure.empty()) {
        return parse_failure("structure start has an empty structure id");
    }
    const auto colon = start.structure.find(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 == start.structure.size()) {
        return parse_failure(fmt::format(
            "structure id '{}' is not a namespaced id", start.structure));
    }
    if (start.chunk_x != chunk_x || start.chunk_z != chunk_z) {
        return parse_failure(fmt::format(
            "structure '{}' names chunk ({}, {}) but is stored in ({}, {})",
            start.structure, start.chunk_x, start.chunk_z, chunk_x, chunk_z));
    }
    if (start.pieces.size() > max_pieces_per_start) {
        return parse_failure(fmt::format(
            "structure '{}' has {} pieces (limit {})", start.structure,
            start.pieces.size(), max_pieces_per_start));
    }
    auto children = nbt::make_list(tag_type::compound);
    bool first = true;
    for (const auto &piece : start.pieces) {
        if (auto status = append_piece(piece, 0, first, state, children);
            !status) {
            return std::unexpected{std::move(status.error())};
        }
        first = false;
    }
    nbt::compound tag{};
    tag["id"] = nbt::make_string(start.structure);
    // The reference writes the chunk being saved, which for an owned start is
    // also the start chunk.
    tag["ChunkX"] = nbt::make_int(chunk_x);
    tag["ChunkZ"] = nbt::make_int(chunk_z);
    // The generated record does not carry the reference counter; the stored
    // compound keeps the key so the tag stays shaped like the reference one.
    tag["references"] = nbt::make_int(0);
    tag["Children"] = std::move(children);
    return std::pair<std::string, value>{start.structure,
                                         nbt::value{std::move(tag)}};
}

auto packed_chunk_pos(std::int32_t x, std::int32_t z) -> std::int64_t {
    const auto low = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x));
    const auto high = static_cast<std::uint64_t>(static_cast<std::uint32_t>(z))
                      << 32U;
    return static_cast<std::int64_t>(low | high);
}

} // namespace

auto encode_structures(
    const std::vector<worldgen::structure_start> &starts,
    const std::vector<worldgen::structure_reference> &references,
    std::int32_t chunk_x, std::int32_t chunk_z)
    -> result<std::vector<std::byte>> {
    if (starts.empty() && references.empty()) {
        return std::vector<std::byte>{};
    }
    if (starts.size() > max_starts_per_column) {
        return parse_failure(fmt::format("column has {} structure starts "
                                         "(limit {})",
                                         starts.size(), max_starts_per_column));
    }
    encode_state state{};
    nbt::compound start_entries{};
    for (const auto &start : starts) {
        auto encoded = encode_start(start, chunk_x, chunk_z, state);
        if (!encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
        if (start_entries.contains(encoded->first)) {
            // The reference chunk model stores one start per structure and the
            // compound is keyed by structure id, so two generated starts for
            // the same structure cannot both be represented.
            return parse_failure(fmt::format(
                "two starts of structure '{}' in one column cannot be stored",
                encoded->first));
        }
        start_entries.emplace(encoded->first, std::move(encoded->second));
    }

    std::map<std::string, std::map<std::int64_t, std::int64_t>, std::less<>>
        reference_entries{};
    for (const auto &reference : references) {
        if (reference.structure.empty()) {
            return parse_failure("structure reference has an empty id");
        }
        if (reference.count <= 0) {
            return parse_failure(
                fmt::format("structure reference '{}' has count {}",
                            reference.structure, reference.count));
        }
        // The reference drops a reference farther than eight chunks away when
        // it loads the compound; written data must respect the same bound.
        const auto distance_x =
            std::abs(static_cast<std::int64_t>(reference.chunk_x) - chunk_x);
        const auto distance_z =
            std::abs(static_cast<std::int64_t>(reference.chunk_z) - chunk_z);
        if (distance_x > 8 || distance_z > 8) {
            return parse_failure(fmt::format(
                "structure '{}' references chunk ({}, {}), more than eight "
                "chunks from ({}, {})",
                reference.structure, reference.chunk_x, reference.chunk_z,
                chunk_x, chunk_z));
        }
        reference_entries[reference.structure][packed_chunk_pos(
            reference.chunk_x, reference.chunk_z)] += reference.count;
    }
    nbt::compound reference_compounds{};
    std::size_t total = 0;
    for (const auto &[structure_id, positions] : reference_entries) {
        nbt::long_array packed{};
        for (const auto &[position, count] : positions) {
            total += static_cast<std::size_t>(count);
            for (std::int64_t copy = 0; copy < count; ++copy) {
                packed.push_back(position);
            }
        }
        reference_compounds.emplace(structure_id,
                                    nbt::make_long_array(std::move(packed)));
    }
    if (total > max_references_per_column) {
        return parse_failure(
            fmt::format("column has {} structure references (limit {})", total,
                        max_references_per_column));
    }

    nbt::compound root{};
    root["starts"] = nbt::make_compound(std::move(start_entries));
    root["References"] = nbt::make_compound(std::move(reference_compounds));
    auto bytes =
        nbt::write(nbt::value{std::move(root)}, "", nbt::compression::none);
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    if (bytes->size() > world::max_payload_bytes) {
        return parse_failure(
            fmt::format("structure payload is {} bytes (limit {})",
                        bytes->size(), world::max_payload_bytes));
    }
    return std::move(*bytes);
}

// --- decoding -------------------------------------------------------------

namespace {

struct decode_state {
    std::size_t pieces{0};
};

/// Vanilla ids of the template-backed structure pieces and the generated
/// record type each one restores to.
constexpr std::array<std::pair<std::string_view, std::string_view>, 6>
    template_piece_ids{{
        {"minecraft:iglu", "minecraft:igl"},
        {"minecraft:shipwreck", "minecraft:shipwreck"},
        {"minecraft:orp", "minecraft:ocean_ruin"},
        {"minecraft:wmp", "minecraft:woodland_mansion"},
        {"minecraft:ecp", "minecraft:end_city"},
        {"minecraft:nefos", "minecraft:nether_fossil"},
    }};

auto classify_vanilla_piece(std::string_view id, piece_family &family)
    -> std::optional<std::string> {
    if (id == "minecraft:btp" || id == "minecraft:tedp" ||
        id == "minecraft:tejp" || id == "minecraft:tesh") {
        family = piece_family::simple;
        return std::string{id};
    }
    for (const auto &[vanilla, internal] : template_piece_ids) {
        if (vanilla == id) {
            family = piece_family::template_piece;
            return std::string{internal};
        }
    }
    if (id == "minecraft:rupo") {
        family = piece_family::ruined_portal;
        return std::string{"minecraft:ruined_portal"};
    }
    if (mineshaft_vanilla_id(id)) {
        family = piece_family::mineshaft;
        return std::string{id};
    }
    if (stronghold_kind_from_id(id)) {
        family = piece_family::stronghold;
        return std::string{id};
    }
    if (fortress_kind_from_id(id)) {
        family = piece_family::fortress;
        return std::string{"minecraft:fortress"};
    }
    if (monument_kind_from_id(id)) {
        family = piece_family::monument;
        return std::string{id};
    }
    if (id == "minecraft:jigsaw") {
        family = piece_family::jigsaw;
        return std::string{"minecraft:jigsaw"};
    }
    return std::nullopt;
}

auto bounds_of(const nbt::compound &entries, std::string_view what)
    -> result<structure::structure_bbox> {
    const auto *box = child(entries, "BB");
    if (box == nullptr) {
        return parse_failure(fmt::format("{} has no bounding box", what));
    }
    const auto values = box->as_int_array();
    if (!values || values->size() != 6U) {
        return parse_failure(
            fmt::format("{} bounding box is not six integers", what));
    }
    return structure::structure_bbox{.min_x = (*values)[0],
                                     .min_y = (*values)[1],
                                     .min_z = (*values)[2],
                                     .max_x = (*values)[3],
                                     .max_y = (*values)[4],
                                     .max_z = (*values)[5]};
}

auto decode_bounds_list(const nbt::compound &entries, std::string_view key,
                        std::string_view what)
    -> result<std::vector<worldgen::structure::structure_bbox>> {
    std::vector<structure::structure_bbox> boxes{};
    const auto *list_value = child(entries, key);
    if (list_value == nullptr) {
        return boxes;
    }
    const auto *list = list_value->as_list();
    if (list == nullptr) {
        return parse_failure(fmt::format("{} '{}' is not a list", what, key));
    }
    boxes.reserve(list->items.size());
    for (const auto &item : list->items) {
        const auto values = item.as_int_array();
        if (!values || values->size() != 6U) {
            return parse_failure(fmt::format(
                "{} '{}' holds an entry that is not six integers", what, key));
        }
        boxes.push_back(structure::structure_bbox{.min_x = (*values)[0],
                                                  .min_y = (*values)[1],
                                                  .min_z = (*values)[2],
                                                  .max_x = (*values)[3],
                                                  .max_y = (*values)[4],
                                                  .max_z = (*values)[5]});
    }
    return boxes;
}

auto decode_template_piece(const nbt::compound &tag,
                           const worldgen::structure_piece_record &piece,
                           std::string_view vanilla_id, std::string_view what)
    -> result<worldgen::structure_piece_record> {
    auto record = piece;
    const auto name = string_field(tag, "Template", std::nullopt, what);
    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    if (name->empty()) {
        return parse_failure(fmt::format("{} has an empty template id", what));
    }
    if (vanilla_id == "minecraft:wmp") {
        record.template_id = "minecraft:woodland_mansion/" + *name;
    } else if (vanilla_id == "minecraft:ecp") {
        record.template_id = "minecraft:end_city/" + *name;
    } else {
        record.template_id = *name;
    }
    const auto rotation = string_field(tag, "Rot", std::nullopt, what);
    if (!rotation) {
        return std::unexpected{std::move(rotation.error())};
    }
    const auto parsed_rotation = parse_rotation(*rotation);
    if (!parsed_rotation) {
        return parse_failure(
            fmt::format("{} has rotation '{}'", what, *rotation));
    }
    record.template_rotation = *parsed_rotation;
    const auto x = int_field(tag, "TPX", 0, what);
    const auto y = int_field(tag, "TPY", 0, what);
    const auto z = int_field(tag, "TPZ", 0, what);
    if (!x || !y || !z) {
        return parse_failure(fmt::format("{} has no template position", what));
    }
    record.template_x = *x;
    record.template_y = *y;
    record.template_z = *z;
    record.template_mirror = 0;
    if (vanilla_id == "minecraft:wmp") {
        const auto mirror = string_field(tag, "Mi", std::nullopt, what);
        if (!mirror) {
            return std::unexpected{std::move(mirror.error())};
        }
        const auto parsed_mirror = parse_mirror(*mirror);
        if (!parsed_mirror) {
            return parse_failure(
                fmt::format("{} has mirror '{}'", what, *mirror));
        }
        record.template_mirror = *parsed_mirror;
    }
    if (vanilla_id == "minecraft:ecp") {
        auto overwrite = bool_field(tag, "OW", false, what);
        if (!overwrite) {
            return std::unexpected{std::move(overwrite.error())};
        }
        record.overwrite = *overwrite;
    }
    if (vanilla_id == "minecraft:orp") {
        auto integrity = float_field(tag, "Integrity", 0.0F, what);
        if (!integrity) {
            return std::unexpected{std::move(integrity.error())};
        }
        record.integrity = *integrity;
        auto large = bool_field(tag, "IsLarge", false, what);
        if (!large) {
            return std::unexpected{std::move(large.error())};
        }
        record.large = *large;
        const auto biome = string_field(tag, "BiomeType", std::nullopt, what);
        if (!biome) {
            return std::unexpected{std::move(biome.error())};
        }
        if (*biome != "WARM" && *biome != "COLD") {
            return parse_failure(
                fmt::format("{} has biome type '{}'", what, *biome));
        }
    }
    if (vanilla_id == "minecraft:shipwreck") {
        auto beached = bool_field(tag, "isBeached", false, what);
        if (!beached) {
            return std::unexpected{std::move(beached.error())};
        }
        record.beached = *beached;
    }
    // The reference restores a template piece's rotation pivot from the
    // template the piece names; the chunk compound does not store it.
    if (vanilla_id == "minecraft:iglu") {
        if (record.template_id.ends_with("igloo/top")) {
            record.template_pivot_x = 3;
            record.template_pivot_y = 5;
            record.template_pivot_z = 5;
        } else if (record.template_id.ends_with("igloo/middle")) {
            record.template_pivot_x = 1;
            record.template_pivot_y = 3;
            record.template_pivot_z = 1;
        } else if (record.template_id.ends_with("igloo/bottom")) {
            record.template_pivot_x = 3;
            record.template_pivot_y = 6;
            record.template_pivot_z = 7;
        } else {
            return parse_failure(fmt::format(
                "igloo piece names unknown template '{}'", record.template_id));
        }
    } else if (vanilla_id == "minecraft:shipwreck") {
        record.template_pivot_x = 4;
        record.template_pivot_y = 0;
        record.template_pivot_z = 15;
    }
    return record;
}

auto decode_ruined_portal(const nbt::compound &tag,
                          const worldgen::structure_piece_record &piece,
                          std::string_view what)
    -> result<worldgen::structure_piece_record> {
    auto record = piece;
    const auto name = string_field(tag, "Template", std::nullopt, what);
    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    record.template_id = *name;
    const auto rotation = string_field(tag, "Rotation", std::nullopt, what);
    const auto mirror = string_field(tag, "Mirror", std::nullopt, what);
    const auto placement =
        string_field(tag, "VerticalPlacement", std::nullopt, what);
    if (!rotation || !mirror || !placement) {
        return parse_failure(
            fmt::format("{} has no portal placement transform", what));
    }
    const auto parsed_rotation = parse_rotation(*rotation);
    const auto parsed_mirror = parse_mirror(*mirror);
    if (!parsed_rotation || !parsed_mirror) {
        return parse_failure(
            fmt::format("{} has an invalid rotation or mirror", what));
    }
    record.template_rotation = *parsed_rotation;
    record.template_mirror = *parsed_mirror;
    const auto x = int_field(tag, "TPX", 0, what);
    const auto y = int_field(tag, "TPY", 0, what);
    const auto z = int_field(tag, "TPZ", 0, what);
    if (!x || !y || !z) {
        return parse_failure(fmt::format("{} has no template position", what));
    }
    record.template_x = *x;
    record.template_y = *y;
    record.template_z = *z;
    static constexpr std::array<std::string_view, 6> placements{
        "on_land_surface", "partly_buried", "on_ocean_floor",
        "in_mountain",     "underground",   "in_nether"};
    if (std::ranges::find(placements, *placement) == placements.end()) {
        return parse_failure(
            fmt::format("{} has vertical placement '{}'", what, *placement));
    }
    const auto *properties = child(tag, "Properties");
    const auto *entries =
        properties == nullptr ? nullptr : properties->as_compound();
    if (entries == nullptr) {
        return parse_failure(fmt::format("{} has no portal properties", what));
    }
    auto cold = bool_field(*entries, "cold", false, what);
    auto mossiness = float_field(*entries, "mossiness", 0.0F, what);
    auto air_pocket = bool_field(*entries, "air_pocket", false, what);
    auto overgrown = bool_field(*entries, "overgrown", false, what);
    auto vines = bool_field(*entries, "vines", false, what);
    auto blackstone =
        bool_field(*entries, "replace_with_blackstone", false, what);
    if (!cold || !mossiness || !air_pocket || !overgrown || !vines ||
        !blackstone) {
        return parse_failure(
            fmt::format("{} portal properties are malformed", what));
    }
    record.portal_valid = true;
    record.portal_placement = *placement;
    record.portal_mossiness = *mossiness;
    record.portal_cold = *cold;
    record.portal_air_pocket = *air_pocket;
    record.portal_overgrown = *overgrown;
    record.portal_vines = *vines;
    record.portal_blackstone = *blackstone;
    return record;
}

auto decode_mineshaft(const nbt::compound &tag,
                      const worldgen::structure_piece_record &piece,
                      std::string_view vanilla_id, std::string_view what)
    -> result<worldgen::structure_piece_record> {
    auto record = piece;
    auto mesa = int_field(tag, "MST", 0, what);
    if (!mesa) {
        return std::unexpected{std::move(mesa.error())};
    }
    record.mesa = *mesa != 0;
    if (vanilla_id == "minecraft:msroom") {
        auto entrances = decode_bounds_list(tag, "Entrances", what);
        if (!entrances) {
            return std::unexpected{std::move(entrances.error())};
        }
        record.entrances = std::move(*entrances);
    } else if (vanilla_id == "minecraft:mscorridor") {
        auto rails = bool_field(tag, "hr", false, what);
        auto spider = bool_field(tag, "sc", false, what);
        auto sections = int_field(tag, "Num", 0, what);
        if (!rails || !spider || !sections) {
            return parse_failure(
                fmt::format("{} corridor fields are malformed", what));
        }
        record.rails = *rails;
        record.spider = *spider;
        record.sections = *sections;
    } else if (vanilla_id == "minecraft:mscrossing") {
        auto two_floored = bool_field(tag, "tf", false, what);
        if (!two_floored) {
            return std::unexpected{std::move(two_floored.error())};
        }
        record.two_floored = *two_floored;
    }
    return record;
}

auto decode_stronghold(const nbt::compound &tag,
                       const worldgen::structure_piece_record &piece,
                       std::string_view vanilla_id, std::string_view what)
    -> result<worldgen::structure_piece_record> {
    const auto kind = stronghold_kind_from_id(vanilla_id);
    if (!kind) {
        return parse_failure(fmt::format("{} is not a stronghold piece", what));
    }
    structure::stronghold_piece_data data{};
    data.kind = *kind;
    const auto door = string_field(tag, "EntryDoor", std::nullopt, what);
    if (!door) {
        return std::unexpected{std::move(door.error())};
    }
    const auto parsed_door = small_door_from_name(*door);
    if (!parsed_door) {
        return parse_failure(fmt::format("{} has door '{}'", what, *door));
    }
    data.door = *parsed_door;
    auto record = piece;
    record.stronghold = data;
    if (vanilla_id == "minecraft:shfc") {
        auto steps = int_field(tag, "Steps", 0, what);
        if (!steps) {
            return std::unexpected{std::move(steps.error())};
        }
        record.stronghold->steps = *steps;
    } else if (vanilla_id == "minecraft:sh5c") {
        auto left_low = bool_field(tag, "leftLow", false, what);
        auto left_high = bool_field(tag, "leftHigh", false, what);
        auto right_low = bool_field(tag, "rightLow", false, what);
        auto right_high = bool_field(tag, "rightHigh", false, what);
        if (!left_low || !left_high || !right_low || !right_high) {
            return parse_failure(
                fmt::format("{} crossing fields are malformed", what));
        }
        record.stronghold->left_low = *left_low;
        record.stronghold->left_high = *left_high;
        record.stronghold->right_low = *right_low;
        record.stronghold->right_high = *right_high;
    } else if (vanilla_id == "minecraft:shli") {
        auto tall = bool_field(tag, "Tall", false, what);
        if (!tall) {
            return std::unexpected{std::move(tall.error())};
        }
        record.stronghold->tall = *tall;
    } else if (vanilla_id == "minecraft:shrc") {
        auto variant = int_field(tag, "Type", 0, what);
        if (!variant) {
            return std::unexpected{std::move(variant.error())};
        }
        record.stronghold->variant = *variant;
    } else if (vanilla_id == "minecraft:shsd" ||
               vanilla_id == "minecraft:shssd") {
        auto source = bool_field(tag, "Source", false, what);
        if (!source) {
            return std::unexpected{std::move(source.error())};
        }
        record.stronghold->source = *source;
    } else if (vanilla_id == "minecraft:shs") {
        auto left = bool_field(tag, "Left", false, what);
        auto right = bool_field(tag, "Right", false, what);
        if (!left || !right) {
            return parse_failure(
                fmt::format("{} straight fields are malformed", what));
        }
        record.stronghold->left = *left;
        record.stronghold->right = *right;
    }
    return record;
}

auto decode_fortress(const nbt::compound &tag,
                     const worldgen::structure_piece_record &piece,
                     std::string_view vanilla_id, std::string_view what)
    -> result<worldgen::structure_piece_record> {
    const auto kind = fortress_kind_from_id(vanilla_id);
    if (!kind) {
        return parse_failure(fmt::format("{} is not a fortress piece", what));
    }
    structure::fortress_piece_data data{};
    data.kind = *kind;
    auto record = piece;
    if (vanilla_id == "minecraft:nebef") {
        auto seed = int_field(tag, "Seed", 0, what);
        if (!seed) {
            return std::unexpected{std::move(seed.error())};
        }
        data.self_seed = *seed;
    } else if (vanilla_id == "minecraft:nesclt" ||
               vanilla_id == "minecraft:nescrt") {
        auto chest = bool_field(tag, "Chest", false, what);
        if (!chest) {
            return std::unexpected{std::move(chest.error())};
        }
        data.chest = *chest;
    }
    record.fortress = data;
    return record;
}

auto decode_monument(const nbt::compound & /*tag*/,
                     const worldgen::structure_piece_record &piece,
                     std::string_view vanilla_id, std::string_view what)
    -> result<worldgen::structure_piece_record> {
    const auto kind = monument_kind_from_id(vanilla_id);
    if (!kind) {
        return parse_failure(fmt::format("{} is not a monument piece", what));
    }
    auto record = piece;
    structure::monument_piece_data data{};
    data.kind = *kind;
    // The monument compound carries only the common piece fields; the room
    // definition is rebuilt from the start seed by the reference, so the
    // restored record holds the geometry without its reconstruction payload.
    record.monument = data;
    return record;
}

auto decode_pool_element(const nbt::compound &entries, std::size_t depth,
                         std::size_t &budget)
    -> result<structure::jigsaw_element_info> {
    if (depth > max_piece_depth || budget == 0) {
        return parse_failure("jigsaw pool element nesting exceeds its bound");
    }
    --budget;
    const auto type =
        string_field(entries, "element_type", std::nullopt, "pool element");
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    structure::jigsaw_element_info element{};
    if (*type == "minecraft:empty_pool_element") {
        element.kind = structure::pool_element_kind::empty;
        element.projection = structure::pool_projection::terrain_matching;
        return element;
    }
    if (*type == "minecraft:single_pool_element") {
        element.kind = structure::pool_element_kind::single;
    } else if (*type == "minecraft:legacy_single_pool_element") {
        element.kind = structure::pool_element_kind::legacy_single;
    } else if (*type == "minecraft:feature_pool_element") {
        element.kind = structure::pool_element_kind::feature;
    } else if (*type == "minecraft:list_pool_element") {
        element.kind = structure::pool_element_kind::list;
    } else {
        return parse_failure(fmt::format("pool element has type '{}'", *type));
    }
    const auto projection =
        string_field(entries, "projection", "rigid", "pool element");
    if (!projection) {
        return std::unexpected{std::move(projection.error())};
    }
    if (*projection == "terrain_matching") {
        element.projection = structure::pool_projection::terrain_matching;
    } else if (*projection == "rigid") {
        element.projection = structure::pool_projection::rigid;
    } else {
        return parse_failure(
            fmt::format("pool element has projection '{}'", *projection));
    }
    if (element.kind == structure::pool_element_kind::feature) {
        const auto feature =
            string_field(entries, "feature", std::nullopt, "pool element");
        if (!feature) {
            return std::unexpected{std::move(feature.error())};
        }
        element.feature = *feature;
        return element;
    }
    if (element.kind == structure::pool_element_kind::list) {
        const auto *elements_value = child(entries, "elements");
        const auto *elements =
            elements_value == nullptr ? nullptr : elements_value->as_list();
        if (elements == nullptr) {
            return parse_failure("list pool element has no element list");
        }
        for (const auto &item : elements->items) {
            const auto *child_entries = item.as_compound();
            if (child_entries == nullptr) {
                return parse_failure(
                    "list pool element holds a non-compound entry");
            }
            auto decoded =
                decode_pool_element(*child_entries, depth + 1, budget);
            if (!decoded) {
                return std::unexpected{std::move(decoded.error())};
            }
            element.elements.push_back(std::move(*decoded));
        }
        return element;
    }
    const auto location =
        string_field(entries, "location", std::nullopt, "pool element");
    if (!location) {
        return std::unexpected{std::move(location.error())};
    }
    element.template_id = *location;
    const auto processors =
        string_field(entries, "processors", "minecraft:empty", "pool element");
    if (!processors) {
        return std::unexpected{std::move(processors.error())};
    }
    if (*processors != "minecraft:empty") {
        element.processor_list = *processors;
    }
    const auto *liquid = child(entries, "override_liquid_settings");
    if (liquid != nullptr) {
        const auto text = liquid->as_string();
        if (!text) {
            return parse_failure(
                "pool element liquid override is not a string");
        }
        if (*text == "ignore_waterlogging") {
            element.liquid_override =
                structure::liquid_settings::ignore_waterlogging;
        } else if (*text == "apply_waterlogging") {
            element.liquid_override =
                structure::liquid_settings::apply_waterlogging;
        } else {
            return parse_failure(
                fmt::format("pool element has liquid setting '{}'", *text));
        }
    }
    return element;
}

auto decode_jigsaw(const nbt::compound &tag,
                   const worldgen::structure_piece_record &piece,
                   std::string_view what)
    -> result<worldgen::structure_piece_record> {
    auto record = piece;
    record.type = "minecraft:jigsaw";
    const auto x = int_field(tag, "PosX", 0, what);
    const auto y = int_field(tag, "PosY", 0, what);
    const auto z = int_field(tag, "PosZ", 0, what);
    if (!x || !y || !z) {
        return parse_failure(fmt::format("{} has no piece position", what));
    }
    record.template_x = *x;
    record.template_y = *y;
    record.template_z = *z;
    const auto rotation = string_field(tag, "rotation", std::nullopt, what);
    if (!rotation) {
        return std::unexpected{std::move(rotation.error())};
    }
    const auto parsed_rotation = parse_rotation(*rotation);
    if (!parsed_rotation) {
        return parse_failure(
            fmt::format("{} has rotation '{}'", what, *rotation));
    }
    record.template_rotation = *parsed_rotation;
    record.template_mirror = 0;
    record.template_pivot_x = 0;
    record.template_pivot_y = 0;
    record.template_pivot_z = 0;
    const auto *pool = child(tag, "pool_element");
    const auto *pool_entries = pool == nullptr ? nullptr : pool->as_compound();
    if (pool_entries == nullptr) {
        return parse_failure(fmt::format("{} has no pool element", what));
    }
    std::size_t budget = max_nested_elements;
    auto element = decode_pool_element(*pool_entries, 0, budget);
    if (!element) {
        return std::unexpected{std::move(element.error())};
    }
    structure::jigsaw_piece_data data{};
    data.element = std::move(*element);
    const auto delta = int_field(tag, "ground_level_delta", 0, what);
    if (!delta) {
        return std::unexpected{std::move(delta.error())};
    }
    data.ground_level_delta = *delta;
    const auto *liquid = child(tag, "liquid_settings");
    if (liquid != nullptr) {
        const auto text = liquid->as_string();
        if (!text) {
            return parse_failure(
                fmt::format("{} liquid setting is not a string", what));
        }
        if (*text == "ignore_waterlogging") {
            data.liquid = structure::liquid_settings::ignore_waterlogging;
        } else if (*text != "apply_waterlogging") {
            return parse_failure(
                fmt::format("{} has liquid setting '{}'", what, *text));
        }
    }
    const auto *junctions_value = child(tag, "junctions");
    if (junctions_value != nullptr) {
        const auto *junctions = junctions_value->as_list();
        if (junctions == nullptr) {
            return parse_failure(
                fmt::format("{} junctions are not a list", what));
        }
        data.junctions.reserve(junctions->items.size());
        for (const auto &item : junctions->items) {
            const auto *entry = item.as_compound();
            if (entry == nullptr) {
                return parse_failure(
                    fmt::format("{} junction is not a compound", what));
            }
            structure::jigsaw_junction_info junction{};
            const auto source_x = int_field(*entry, "source_x", 0, what);
            const auto source_y = int_field(*entry, "source_ground_y", 0, what);
            const auto source_z = int_field(*entry, "source_z", 0, what);
            const auto delta_y = int_field(*entry, "delta_y", 0, what);
            if (!source_x || !source_y || !source_z || !delta_y) {
                return parse_failure(
                    fmt::format("{} junction fields are malformed", what));
            }
            junction.source_x = *source_x;
            junction.source_ground_y = *source_y;
            junction.source_z = *source_z;
            junction.delta_y = *delta_y;
            const auto projection =
                string_field(*entry, "dest_proj", "rigid", what);
            if (!projection) {
                return std::unexpected{std::move(projection.error())};
            }
            if (*projection == "terrain_matching") {
                junction.dest_projection =
                    structure::pool_projection::terrain_matching;
            } else if (*projection != "rigid") {
                return parse_failure(fmt::format(
                    "{} junction has projection '{}'", what, *projection));
            }
            data.junctions.push_back(junction);
        }
    }
    record.jigsaw = std::move(data);
    return record;
}

auto decode_piece(const value &item, std::size_t depth, decode_state &state)
    -> result<worldgen::structure_piece_record> {
    if (depth > max_piece_depth) {
        return parse_failure("structure piece nesting exceeds its bound");
    }
    ++state.pieces;
    if (state.pieces > max_pieces_per_column) {
        return parse_failure(fmt::format(
            "structure data has more than {} pieces", max_pieces_per_column));
    }
    const auto *entries = item.as_compound();
    if (entries == nullptr) {
        return parse_failure("structure piece is not a compound");
    }
    const auto id = string_field(*entries, "id", std::nullopt, "piece");
    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    piece_family family{piece_family::simple};
    auto type = classify_vanilla_piece(*id, family);
    if (!type) {
        return parse_failure(
            fmt::format("structure piece id '{}' has no record type", *id));
    }
    const auto what = fmt::format("piece '{}'", *id);
    auto bounds = bounds_of(*entries, what);
    if (!bounds) {
        return std::unexpected{std::move(bounds.error())};
    }
    if (bounds->min_x > bounds->max_x || bounds->min_y > bounds->max_y ||
        bounds->min_z > bounds->max_z) {
        return parse_failure(
            fmt::format("{} has an inverted bounding box", what));
    }
    auto orientation = int_field(*entries, "O", -1, what);
    if (!orientation) {
        return std::unexpected{std::move(orientation.error())};
    }
    if (*orientation < -1 || *orientation > 3) {
        return parse_failure(
            fmt::format("{} has orientation {}", what, *orientation));
    }
    auto gen_depth = int_field(*entries, "GD", 0, what);
    if (!gen_depth) {
        return std::unexpected{std::move(gen_depth.error())};
    }
    worldgen::structure_piece_record record{};
    record.type = std::move(*type);
    record.min_x = bounds->min_x;
    record.min_y = bounds->min_y;
    record.min_z = bounds->min_z;
    record.max_x = bounds->max_x;
    record.max_y = bounds->max_y;
    record.max_z = bounds->max_z;
    record.orientation = *orientation;
    record.template_rotation = 0;
    record.template_mirror = 0;
    record.height_position = -1;
    record.height_offset = 0;
    record.integrity = 1.0F;
    record.overwrite = true;
    switch (family) {
    case piece_family::simple:
        return record;
    case piece_family::template_piece: {
        auto decoded = decode_template_piece(*entries, record, *id, what);
        if (decoded) {
            decoded->orientation = 2;
        }
        return decoded;
    }
    case piece_family::ruined_portal: {
        auto decoded = decode_ruined_portal(*entries, record, what);
        if (decoded) {
            decoded->orientation = 2;
        }
        return decoded;
    }
    case piece_family::mineshaft:
        return decode_mineshaft(*entries, record, *id, what);
    case piece_family::stronghold: {
        auto decoded = decode_stronghold(*entries, record, *id, what);
        if (decoded) {
            decoded->stronghold->gen_depth = *gen_depth;
        }
        return decoded;
    }
    case piece_family::fortress:
        return decode_fortress(*entries, record, *id, what);
    case piece_family::monument:
        return decode_monument(*entries, record, *id, what);
    case piece_family::jigsaw:
        return decode_jigsaw(*entries, record, what);
    }
    return parse_failure(fmt::format("{} has an unknown family", what));
}

auto decoded_piece_bounds_union(
    const std::vector<worldgen::structure_piece_record> &pieces)
    -> structure::structure_bbox {
    structure::structure_bbox bounds{};
    if (pieces.empty()) {
        return bounds;
    }
    bounds = structure::structure_bbox{.min_x = pieces.front().min_x,
                                       .min_y = pieces.front().min_y,
                                       .min_z = pieces.front().min_z,
                                       .max_x = pieces.front().max_x,
                                       .max_y = pieces.front().max_y,
                                       .max_z = pieces.front().max_z};
    for (const auto &piece : pieces) {
        bounds.min_x = std::min(bounds.min_x, piece.min_x);
        bounds.min_y = std::min(bounds.min_y, piece.min_y);
        bounds.min_z = std::min(bounds.min_z, piece.min_z);
        bounds.max_x = std::max(bounds.max_x, piece.max_x);
        bounds.max_y = std::max(bounds.max_y, piece.max_y);
        bounds.max_z = std::max(bounds.max_z, piece.max_z);
    }
    return bounds;
}

/// The reference saves a monument building first and its rooms after it in
/// the same flat piece list; the generated record nests the rooms inside the
/// building so the placement runs them with the building pass.
auto nest_monument_children(
    std::vector<worldgen::structure_piece_record> &pieces) -> void {
    if (pieces.size() < 2) {
        return;
    }
    auto &first = pieces.front();
    if (!first.monument.has_value() ||
        first.monument->kind != structure::monument_piece_kind::building) {
        return;
    }
    first.children.reserve(pieces.size() - 1);
    for (std::size_t index = 1; index < pieces.size(); ++index) {
        first.children.push_back(std::move(pieces[index]));
    }
    pieces.resize(1);
}

} // namespace

auto decode_structures(
    std::span<const std::byte> payload, worldgen::proto_chunk &target,
    const std::function<std::string(std::string_view)> &step_for) -> status {
    if (payload.empty()) {
        return status_ok();
    }
    if (payload.size() > world::max_payload_bytes) {
        return parse_failure(
            fmt::format("stored structures payload is {} bytes (limit {})",
                        payload.size(), world::max_payload_bytes));
    }
    auto document = nbt::read(payload, nbt::compression::none);
    if (!document) {
        return parse_failure(
            fmt::format("stored structures payload is not valid NBT: {}",
                        document.error().message));
    }
    const auto *root = document->as_compound();
    if (root == nullptr) {
        return parse_failure("stored structures payload is not a compound");
    }
    decode_state state{};

    if (const auto *starts_value = child(*root, "starts");
        starts_value != nullptr) {
        const auto *starts = starts_value->as_compound();
        if (starts == nullptr) {
            return parse_failure(
                "stored structures 'starts' is not a compound");
        }
        if (starts->size() > max_starts_per_column) {
            return parse_failure(
                fmt::format("stored structures carry {} starts (limit {})",
                            starts->size(), max_starts_per_column));
        }
        for (const auto &[structure_id, item] : *starts) {
            const auto *entry = item.as_compound();
            if (entry == nullptr) {
                return parse_failure(fmt::format(
                    "stored start '{}' is not a compound", structure_id));
            }
            const auto id =
                string_field(*entry, "id", std::nullopt,
                             fmt::format("start '{}'", structure_id));
            if (!id) {
                return std::unexpected{std::move(id.error())};
            }
            if (*id != structure_id) {
                return parse_failure(fmt::format(
                    "stored start key '{}' names id '{}'", structure_id, *id));
            }
            // An invalidated start carries no pieces and marks a structure the
            // reference removed; it takes no part in continuation.
            if (structure_id == "INVALID") {
                continue;
            }
            if (structure_id.empty()) {
                return parse_failure("stored start has an empty structure id");
            }
            auto chunk_x = int_field(*entry, "ChunkX", 0,
                                     fmt::format("start '{}'", structure_id));
            auto chunk_z = int_field(*entry, "ChunkZ", 0,
                                     fmt::format("start '{}'", structure_id));
            if (!chunk_x || !chunk_z) {
                return parse_failure(fmt::format(
                    "stored start '{}' has no chunk position", structure_id));
            }
            std::vector<worldgen::structure_piece_record> pieces{};
            if (const auto *children_value = child(*entry, "Children");
                children_value != nullptr) {
                const auto *children = children_value->as_list();
                if (children == nullptr) {
                    return parse_failure(
                        fmt::format("stored start '{}' children are not a list",
                                    structure_id));
                }
                if (children->items.size() > max_pieces_per_start) {
                    return parse_failure(fmt::format(
                        "stored start '{}' has {} pieces (limit {})",
                        structure_id, children->items.size(),
                        max_pieces_per_start));
                }
                pieces.reserve(children->items.size());
                for (const auto &piece_item : children->items) {
                    auto decoded = decode_piece(piece_item, 0, state);
                    if (!decoded) {
                        return std::unexpected{std::move(decoded.error())};
                    }
                    pieces.push_back(std::move(*decoded));
                }
            }
            nest_monument_children(pieces);
            auto bounds = decoded_piece_bounds_union(pieces);
            for (auto &piece : pieces) {
                if (piece.type == "minecraft:woodland_mansion") {
                    piece.height_position = bounds.min_y;
                }
            }
            worldgen::structure_start start{};
            start.structure = structure_id;
            start.step = step_for ? step_for(structure_id) : std::string{};
            start.chunk_x = *chunk_x;
            start.chunk_z = *chunk_z;
            start.min_x = bounds.min_x;
            start.min_y = bounds.min_y;
            start.min_z = bounds.min_z;
            start.max_x = bounds.max_x;
            start.max_y = bounds.max_y;
            start.max_z = bounds.max_z;
            start.pieces = std::move(pieces);
            target.add_structure_start(std::move(start));
        }
    }

    if (const auto *references_value = child(*root, "References");
        references_value != nullptr) {
        const auto *references = references_value->as_compound();
        if (references == nullptr) {
            return parse_failure(
                "stored structures 'References' is not a compound");
        }
        std::map<std::pair<std::string, std::pair<std::int32_t, std::int32_t>>,
                 std::int32_t>
            restored{};
        std::size_t total = 0;
        for (const auto &[structure_id, item] : *references) {
            if (structure_id.empty()) {
                return parse_failure(
                    "stored structure reference has an empty structure id");
            }
            const auto packed = item.as_long_array();
            if (!packed) {
                return parse_failure(
                    fmt::format("stored references '{}' are not a long array",
                                structure_id));
            }
            for (const auto entry : *packed) {
                const auto bits = static_cast<std::uint64_t>(entry);
                const auto chunk_x =
                    static_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
                const auto chunk_z = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(bits >> 32U));
                total += 1;
                if (total > max_references_per_column) {
                    return parse_failure(fmt::format(
                        "stored structures carry more than {} references",
                        max_references_per_column));
                }
                auto &count = restored[{structure_id, {chunk_x, chunk_z}}];
                if (count == std::numeric_limits<std::int32_t>::max()) {
                    return parse_failure(fmt::format(
                        "stored reference '{}' at ({}, {}) overflows its count",
                        structure_id, chunk_x, chunk_z));
                }
                ++count;
            }
        }
        for (const auto &[key, count] : restored) {
            target.add_structure_reference(key.first, key.second.first,
                                           key.second.second, count);
        }
    }
    return status_ok();
}

} // namespace refrain::world::detail
