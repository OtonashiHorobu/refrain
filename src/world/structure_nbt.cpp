// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "structure_nbt_internal.hpp"

#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/model.hpp>
#include <refrain/worldgen/structure/placement_types.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
/// The inline processor list is the one part of a piece tag that carries a
/// datapack document rather than a decoded record, so its conversion walks the
/// document and is bounded separately. A processor entry nests a handful of
/// levels; the node bound is generous for a rule list of a few thousand rules.
constexpr std::size_t max_processor_depth = 64;
constexpr std::size_t max_processor_nodes = 65536;

auto parse_failure(std::string message) -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
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
} // --- piece families -------------------------------------------------------

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
        // TODO: the generation record does not carry whether the corridor
        // already placed its spider spawner, so the tag always writes 0. The
        // correct behaviour is to carry the flag on the piece record.
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
    } else if (id == "minecraft:shcc") {
        // TODO: the generation record does not carry whether the chest
        // corridor already placed its chest, so the tag always writes false.
        // The correct behaviour is to carry the flag on the piece record.
        tag["Chest"] = nbt::make_byte(0);
    } else if (id == "minecraft:shpr") {
        // TODO: the generation record does not carry whether the portal room
        // already placed its spawner, so the tag always writes false. The
        // correct behaviour is to carry the flag on the piece record.
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
    } else if (identity.vanilla_id == "minecraft:nemt") {
        // TODO: the generation record does not carry whether the monster
        // throne already placed its spawner, so the tag always writes false.
        // The correct behaviour is to carry the flag on the piece record.
        tag["Mob"] = nbt::make_byte(0);
    }
    return status_ok();
}

/// The numeric tag kinds that the reference's `NumericTag.asNumber` accepts.
[[nodiscard]] auto is_numeric_tag(tag_type kind) noexcept -> bool {
    switch (kind) {
    case tag_type::byte:
    case tag_type::short_:
    case tag_type::int_:
    case tag_type::long_:
    case tag_type::float_:
    case tag_type::double_:
        return true;
    default:
        return false;
    }
}

/// Widens any numeric tag to a double; the caller only passes numeric tags.
[[nodiscard]] auto numeric_as_double(const value &stored) noexcept
    -> std::optional<double> {
    if (const auto number = stored.as_byte()) {
        return static_cast<double>(*number);
    }
    if (const auto number = stored.as_short()) {
        return static_cast<double>(*number);
    }
    if (const auto number = stored.as_int()) {
        return static_cast<double>(*number);
    }
    if (const auto number = stored.as_long()) {
        return static_cast<double>(*number);
    }
    if (const auto number = stored.as_float()) {
        return static_cast<double>(*number);
    }
    return stored.as_double();
}

/// Converts one JSON node of an inline processor list into the NBT value the
/// reference codec writes for it. The document is the datapack's codec form,
/// so its JSON kinds map onto the codec's value kinds: booleans are bytes,
/// integers are ints while they fit and longs beyond, reals are doubles, and
/// arrays become lists of one element type. An array mixing numeric kinds is
/// widened to a TAG_Double list: the reference reads every numeric tag through
/// `NumericTag.asNumber`, so all of its numeric decoders accept a double. An
/// array mixing numbers with non-numeric values fails instead: this NBT DOM
/// has only homogeneous lists and does not carry the reference's empty-name
/// compound wrappers for heterogeneous lists. A null also fails.
auto processor_value(const data::json::value &node, std::size_t depth,
                     std::size_t &budget) -> result<value> {
    if (depth > max_processor_depth) {
        return parse_failure("inline processor list nesting exceeds its bound");
    }
    if (budget == 0) {
        return parse_failure(
            fmt::format("inline processor list has more than {} values",
                        max_processor_nodes));
    }
    --budget;
    switch (node.kind()) {
    case data::json::type::boolean: {
        const auto flag = node.as_bool();
        if (!flag) {
            return parse_failure(
                "inline processor value has an invalid boolean");
        }
        return nbt::make_byte(*flag ? 1 : 0);
    }
    case data::json::type::integer: {
        const auto number = node.as_int64();
        if (!number) {
            return parse_failure(
                "inline processor value has an invalid integer");
        }
        if (*number < std::numeric_limits<std::int32_t>::min() ||
            *number > std::numeric_limits<std::int32_t>::max()) {
            return nbt::make_long(*number);
        }
        return nbt::make_int(static_cast<std::int32_t>(*number));
    }
    case data::json::type::real: {
        const auto number = node.as_double();
        if (!number) {
            return parse_failure("inline processor value has an invalid real");
        }
        return nbt::make_double(*number);
    }
    case data::json::type::string: {
        const auto text = node.as_string();
        if (!text) {
            return parse_failure(
                "inline processor value has an invalid string");
        }
        return nbt::make_string(std::string{*text});
    }
    case data::json::type::array: {
        const auto items = node.as_array();
        if (!items) {
            return parse_failure("inline processor value is not an array");
        }
        if (items->empty()) {
            return nbt::make_list(tag_type::end);
        }
        std::vector<value> converted;
        converted.reserve(items->size());
        for (const auto &item : *items) {
            auto encoded = processor_value(item, depth + 1, budget);
            if (!encoded) {
                return std::unexpected{std::move(encoded.error())};
            }
            converted.push_back(std::move(*encoded));
        }
        const auto element_type = converted.front().type();
        const auto first_different =
            std::find_if(converted.begin(), converted.end(),
                         [element_type](const value &item) {
                             return item.type() != element_type;
                         });
        if (first_different == converted.end()) {
            auto list = nbt::make_list(element_type);
            for (auto &item : converted) {
                if (auto pushed = list.push(std::move(item)); !pushed) {
                    return std::unexpected{std::move(pushed.error())};
                }
            }
            return list;
        }
        if (!std::all_of(converted.begin(), converted.end(),
                         [](const value &item) {
                             return is_numeric_tag(item.type());
                         })) {
            return parse_failure(fmt::format(
                "inline processor array mixes {} and {} values; this NBT list "
                "model is homogeneous and has no empty-name wrappers",
                nbt::tag_type_name(element_type),
                nbt::tag_type_name(first_different->type())));
        }
        // The reference persists a heterogeneous numeric list as a compound
        // list with empty-name wrappers. Widening to TAG_Double keeps the same
        // value for every numeric reader without those wrappers.
        auto list = nbt::make_list(tag_type::double_);
        for (const auto &item : converted) {
            const auto number = numeric_as_double(item);
            if (!number) {
                return parse_failure(
                    "inline processor array has an unreadable number");
            }
            if (auto pushed = list.push(nbt::make_double(*number)); !pushed) {
                return std::unexpected{std::move(pushed.error())};
            }
        }
        return list;
    }
    case data::json::type::object: {
        const auto members = node.as_object();
        if (!members) {
            return parse_failure("inline processor value is not a compound");
        }
        nbt::compound entries{};
        for (const auto &[key, child] : members->get()) {
            auto encoded = processor_value(child, depth + 1, budget);
            if (!encoded) {
                return std::unexpected{std::move(encoded.error())};
            }
            entries.emplace(key, std::move(*encoded));
        }
        return nbt::make_compound(std::move(entries));
    }
    case data::json::type::null:
        break;
    }
    return parse_failure("inline processor list contains a null value");
}

/// Encodes the reference's `processors` field for a template-backed pool
/// element. A named processor list is stored as its id; an inline list is
/// stored as the direct codec's object form `{"processors": [...]}`, which is
/// the shape the reference writes and reads back. The datapack field the
/// decoder kept is either that object or the bare list its alternative codec
/// accepts, so a bare list normalises to the object form on save.
auto inline_processors_value(const data::json::value &document)
    -> result<value> {
    const data::json::value *entries = nullptr;
    if (document.is_array()) {
        entries = &document;
    } else if (document.is_object()) {
        entries = document.find("processors");
        if (entries == nullptr) {
            return parse_failure(
                "inline processor list has no processors field");
        }
        if (!entries->is_array()) {
            return parse_failure(
                "inline processor list's processors field is not an array");
        }
    } else {
        return parse_failure(
            "inline processor list is neither a list nor a processors object");
    }
    const auto items = entries->as_array();
    if (!items) {
        return parse_failure("inline processor list is not an array");
    }
    std::vector<value> converted;
    converted.reserve(items->size());
    std::size_t budget = max_processor_nodes;
    for (const auto &item : *items) {
        if (!item.is_object()) {
            return parse_failure(
                "inline processor entry is not a processor object");
        }
        auto encoded = processor_value(item, 1, budget);
        if (!encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
        converted.push_back(std::move(*encoded));
    }
    // An empty list keeps TAG_End as its element type, the form the reference
    // writes for an empty codec list; a non-empty one is a compound list.
    auto list = converted.empty() ? nbt::make_list(tag_type::end)
                                  : nbt::make_list(tag_type::compound);
    for (auto &item : converted) {
        if (auto pushed = list.push(std::move(item)); !pushed) {
            return std::unexpected{std::move(pushed.error())};
        }
    }
    nbt::compound out{};
    out["processors"] = std::move(list);
    return nbt::value{std::move(out)};
}

auto encode_pool_element(const structure::jigsaw_element_info &element,
                         std::size_t depth, std::size_t &budget)
    -> result<value> {
    if (depth > max_piece_depth || budget == 0) {
        return parse_failure("jigsaw pool element nesting exceeds its bound");
    }
    --budget;
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
    // Only the template-backed element codecs declare a `processors` field.
    // The other reference codecs ignore unknown fields, so a list carried on
    // those kinds is accepted and dropped rather than rejecting the column.
    switch (element.kind) {
    case structure::pool_element_kind::empty:
        return nbt::value{std::move(out)};
    case structure::pool_element_kind::single:
    case structure::pool_element_kind::legacy_single:
        out["location"] = nbt::make_string(element.template_id);
        if (element.inline_processors != nullptr) {
            auto processors =
                inline_processors_value(*element.inline_processors);
            if (!processors) {
                return std::unexpected{std::move(processors.error())};
            }
            out["processors"] = std::move(*processors);
        } else {
            out["processors"] = nbt::make_string(element.processor_list.empty()
                                                     ? "minecraft:empty"
                                                     : element.processor_list);
        }
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
            // TODO: the generation record does not carry the runtime
            // "already placed" flags (the desert pyramid chest slots, the
            // jungle temple chests and traps, the swamp hut witch and cat),
            // so the tag omits them and the reference loads false. The
            // correct behaviour is to carry the flags on the piece record.
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
        // The reference stores monument pieces without the room definition and
        // rebuilds the rooms from the start seed when it loads them, so the
        // tag carries the common piece fields only.
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
    // TODO: the generation record does not carry the reference counter, so the
    // start tag always writes 0. The correct behaviour is to carry the counter
    // on the start record and write it here.
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

} // namespace refrain::world::detail
