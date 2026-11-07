// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/net/base.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace refrain::net {

/// A text component in its network-NBT form. The current codec supports the
/// string form, which covers chat, titles, tab list headers and disconnect
/// messages; compound components need the full NBT tree and are rejected.
struct text_component {
    std::string text{};

    friend auto operator==(const text_component &, const text_component &)
        -> bool = default;
};

/// A network-NBT tag in encoded form. An empty payload is the end tag, which
/// the optional compound codec uses for absence.
struct nbt_blob {
    std::vector<std::byte> bytes{};

    friend auto operator==(const nbt_blob &, const nbt_blob &)
        -> bool = default;
};

/// Writes the stored tag bytes; an empty tag is written as the end tag.
[[nodiscard]] auto encode_nbt(writer &out, const nbt_blob &value) -> status;

/// Reads one complete network-NBT tag, copying its exact bytes. Nesting depth,
/// element counts and total size are bounded.
[[nodiscard]] auto decode_nbt(reader &in, nbt_blob &value) -> status;

/// A quantized vector. The wire form stores a shared exponent and three
/// 15-bit normalized components, or a single zero byte for the zero vector.
/// Decoding reproduces the quantized value, so it is not bit-identical to the
/// value that was encoded.
struct lp_vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    friend auto operator==(const lp_vec3 &, const lp_vec3 &) -> bool = default;
};

/// Writes the quantized vector encoding, clamping components to the protocol
/// range.
[[nodiscard]] auto encode_lp_vec3(writer &out, const lp_vec3 &value) -> status;

/// Reads a quantized vector; truncated input is a parse error.
[[nodiscard]] auto decode_lp_vec3(reader &in, lp_vec3 &value) -> status;

/// A chat signature reference: either a cache id or the full 256-byte
/// signature.
struct message_signature_packed {
    std::uint32_t id{0};
    std::array<std::byte, 256> signature{};
    bool full{false};

    friend auto operator==(const message_signature_packed &,
                           const message_signature_packed &) -> bool = default;
};

/// Writes the id form (`id + 1`) or the full signature form (`0` + bytes).
[[nodiscard]] auto encode_message_signature_packed(
    writer &out, const message_signature_packed &value) -> status;

/// Reads either form; a full signature consumes exactly 256 bytes.
[[nodiscard]] auto
decode_message_signature_packed(reader &in, message_signature_packed &value)
    -> status;

/// A chat filter mask: a VarInt type (0 pass-through, 1 fully filtered,
/// 2 partially filtered) followed by a byte-set for the partial form.
struct filter_mask {
    std::uint32_t type{0};
    std::vector<std::byte> mask{};

    friend auto operator==(const filter_mask &, const filter_mask &)
        -> bool = default;
};

[[nodiscard]] auto encode_filter_mask(writer &out, const filter_mask &value)
    -> status;
[[nodiscard]] auto decode_filter_mask(reader &in, filter_mask &value) -> status;

/// A statistic identity: the stat type id and its value id, both VarInts.
struct stat_entry {
    std::uint32_t type{0};
    std::uint32_t value{0};

    friend auto operator==(const stat_entry &, const stat_entry &)
        -> bool = default;
    friend auto operator<=>(const stat_entry &, const stat_entry &)
        -> std::strong_ordering = default;
};

[[nodiscard]] auto encode_stat_entry(writer &out, const stat_entry &value)
    -> status;
[[nodiscard]] auto decode_stat_entry(reader &in, stat_entry &value) -> status;

/// One step of a stepped position path.
struct position_step {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    std::uint32_t tick_offset{0};

    friend auto operator==(const position_step &, const position_step &)
        -> bool = default;
};

/// An entity position path: a linear end position (type 0) or a stepped list
/// (type 1).
struct position_path {
    std::uint32_t type{0};
    std::array<double, 3> end{};
    std::vector<position_step> steps{};

    friend auto operator==(const position_path &, const position_path &)
        -> bool = default;
};

[[nodiscard]] auto encode_position_path(writer &out, const position_path &value)
    -> status;
[[nodiscard]] auto decode_position_path(reader &in, position_path &value)
    -> status;

/// A map colour patch. A zero width means no patch; the remaining fields are
/// present only for a non-empty patch.
struct map_patch {
    std::uint8_t width{0};
    std::uint8_t height{0};
    std::uint8_t start_x{0};
    std::uint8_t start_y{0};
    std::vector<std::byte> colors{};

    friend auto operator==(const map_patch &, const map_patch &)
        -> bool = default;
};

[[nodiscard]] auto encode_map_patch(writer &out, const map_patch &value)
    -> status;
[[nodiscard]] auto decode_map_patch(reader &in, map_patch &value) -> status;

/// Advancement screen selection: action 0 opens a tab and carries its
/// identifier; action 1 closes the screen.
struct advancement_seen {
    std::uint32_t action{1};
    data::resource_location tab{};

    friend auto operator==(const advancement_seen &, const advancement_seen &)
        -> bool = default;
};

[[nodiscard]] auto encode_advancement_seen(writer &out,
                                           const advancement_seen &value)
    -> status;
[[nodiscard]] auto decode_advancement_seen(reader &in, advancement_seen &value)
    -> status;

/// One step of a stepped entity movement delta.
struct move_step {
    std::uint32_t ticks{0};
    std::int16_t x{0};
    std::int16_t y{0};
    std::int16_t z{0};

    friend auto operator==(const move_step &, const move_step &)
        -> bool = default;
};

/// An entity movement delta: a packed `on_ground`/step-count header followed
/// by either three shorts (step count zero) or that many stepped deltas.
struct vec_delta {
    bool on_ground{false};
    std::int16_t x{0};
    std::int16_t y{0};
    std::int16_t z{0};
    std::vector<move_step> steps{};

    friend auto operator==(const vec_delta &, const vec_delta &)
        -> bool = default;
};

[[nodiscard]] auto encode_vec_delta(writer &out, const vec_delta &value)
    -> status;
[[nodiscard]] auto decode_vec_delta(reader &in, vec_delta &value) -> status;

/// Stop-sound selector: flags bit 0 carries a source id and bit 1 a name.
struct sound_selector {
    std::uint32_t flags{0};
    std::uint32_t source{0};
    data::resource_location name{};

    friend auto operator==(const sound_selector &, const sound_selector &)
        -> bool = default;
};

[[nodiscard]] auto encode_sound_selector(writer &out,
                                         const sound_selector &value) -> status;
[[nodiscard]] auto decode_sound_selector(reader &in, sound_selector &value)
    -> status;

/// Scoreboard objective add/change/remove. The optional number format is not
/// yet supported and is rejected when present.
struct objective_update {
    std::string name{};
    std::uint8_t method{0};
    text_component display{};
    std::uint32_t render_type{0};

    friend auto operator==(const objective_update &, const objective_update &)
        -> bool = default;
};

[[nodiscard]] auto encode_objective_update(writer &out,
                                           const objective_update &value)
    -> status;
[[nodiscard]] auto decode_objective_update(reader &in, objective_update &value)
    -> status;

/// Scoreboard team parameters, present for add/change.
struct team_parameters {
    text_component display{};
    text_component prefix{};
    text_component suffix{};
    std::uint32_t visibility{0};
    std::uint32_t collision{0};
    std::optional<std::uint32_t> color{};
    std::uint8_t options{0};

    friend auto operator==(const team_parameters &, const team_parameters &)
        -> bool = default;
};

/// Scoreboard team add/remove/change/join/leave payload.
struct team_update {
    std::string name{};
    std::uint8_t method{0};
    std::optional<team_parameters> parameters{};
    std::vector<std::string> players{};

    friend auto operator==(const team_update &, const team_update &)
        -> bool = default;
};

[[nodiscard]] auto encode_team_update(writer &out, const team_update &value)
    -> status;
[[nodiscard]] auto decode_team_update(reader &in, team_update &value) -> status;

/// One entry of a game profile's property list.
struct profile_property {
    std::string name{};
    std::string value{};
    std::optional<std::string> signature{};

    friend auto operator==(const profile_property &, const profile_property &)
        -> bool = default;
};

/// A remote chat session attached to a player-list entry.
struct chat_session_data {
    bool present{false};
    uuid session_id{};
    std::int64_t expires_at{0};
    std::vector<std::byte> key{};
    std::vector<std::byte> key_signature{};

    friend auto operator==(const chat_session_data &, const chat_session_data &)
        -> bool = default;
};

/// One player-list entry; fields are present according to the action bits.
struct player_info_entry {
    uuid id{};
    std::string name{};
    std::vector<profile_property> properties{};
    chat_session_data chat{};
    std::optional<std::uint32_t> game_mode{};
    std::optional<bool> listed{};
    std::optional<std::uint32_t> latency{};
    bool has_display_name{false};
    text_component display_name{};
    std::optional<std::uint32_t> list_order{};
    std::optional<bool> show_hat{};

    friend auto operator==(const player_info_entry &, const player_info_entry &)
        -> bool = default;
};

/// A player-info update: an action bit set followed by the entries.
struct player_info_update {
    std::uint8_t actions{0};
    std::vector<player_info_entry> entries{};

    friend auto operator==(const player_info_update &,
                           const player_info_update &) -> bool = default;
};

[[nodiscard]] auto encode_player_info_update(writer &out,
                                             const player_info_update &value)
    -> status;
[[nodiscard]] auto decode_player_info_update(reader &in,
                                             player_info_update &value)
    -> status;

/// A tracked waypoint with its operation. Type 0 is empty, 1 a block position,
/// 2 a chunk, 3 an azimuth.
struct tracked_waypoint {
    std::uint32_t operation{0};
    bool has_uuid{true};
    uuid id{};
    std::string name{};
    data::resource_location style{};
    std::optional<std::uint32_t> color{};
    std::uint32_t type{0};
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t z{0};
    float angle{0.0F};

    friend auto operator==(const tracked_waypoint &, const tracked_waypoint &)
        -> bool = default;
};

[[nodiscard]] auto encode_tracked_waypoint(writer &out,
                                           const tracked_waypoint &value)
    -> status;
[[nodiscard]] auto decode_tracked_waypoint(reader &in, tracked_waypoint &value)
    -> status;

/// An item stack on the wire. Stacks without a data-component patch are
/// implemented; a non-empty patch is reported as unsupported because the
/// component graph is not modelled yet.
struct item_stack {
    std::uint32_t count{0};
    std::uint32_t item_id{0};

    friend auto operator==(const item_stack &, const item_stack &)
        -> bool = default;
};

/// Writes the count, item id and an empty component patch.
[[nodiscard]] auto encode_item_stack(writer &out, const item_stack &value)
    -> status;

/// Reads a stack; a non-empty component patch is reported as unsupported.
[[nodiscard]] auto decode_item_stack(reader &in, item_stack &value) -> status;

/// An exact data-component predicate. Only the empty predicate is implemented.
struct component_predicate {
    friend auto operator==(const component_predicate &,
                           const component_predicate &) -> bool = default;
};

[[nodiscard]] auto encode_component_predicate(writer &out,
                                              const component_predicate &value)
    -> status;
[[nodiscard]] auto decode_component_predicate(reader &in,
                                              component_predicate &value)
    -> status;

/// One side of a merchant trade: an item id, count and empty predicate.
struct item_cost {
    std::uint32_t item_id{0};
    std::uint32_t count{0};

    friend auto operator==(const item_cost &, const item_cost &)
        -> bool = default;
};

/// A merchant trade offer.
struct merchant_offer {
    item_cost cost_a{};
    item_stack result{};
    std::optional<item_cost> cost_b{};
    bool out_of_stock{false};
    std::int32_t uses{0};
    std::int32_t max_uses{0};
    std::int32_t xp{0};
    std::int32_t special_price_diff{0};
    float price_multiplier{0.0F};
    std::int32_t demand{0};

    friend auto operator==(const merchant_offer &, const merchant_offer &)
        -> bool = default;
};

[[nodiscard]] auto encode_merchant_offer(writer &out,
                                         const merchant_offer &value) -> status;
[[nodiscard]] auto decode_merchant_offer(reader &in, merchant_offer &value)
    -> status;

/// An item with a count and an empty component patch, used by advancement
/// icons.
struct item_stack_template {
    std::uint32_t item_id{0};
    std::uint32_t count{1};

    friend auto operator==(const item_stack_template &,
                           const item_stack_template &) -> bool = default;
};

[[nodiscard]] auto encode_item_stack_template(writer &out,
                                              const item_stack_template &value)
    -> status;
[[nodiscard]] auto decode_item_stack_template(reader &in,
                                              item_stack_template &value)
    -> status;

/// An advancement display: title, description, icon, frame, flags and an
/// optional background.
struct advancement_display {
    text_component title{};
    text_component description{};
    item_stack_template icon{};
    std::uint32_t frame{0};
    std::int32_t flags{0};
    std::optional<data::resource_location> background{};

    friend auto operator==(const advancement_display &,
                           const advancement_display &) -> bool = default;
};

[[nodiscard]] auto encode_advancement_display(writer &out,
                                              const advancement_display &value)
    -> status;
[[nodiscard]] auto decode_advancement_display(reader &in,
                                              advancement_display &value)
    -> status;

/// Requirement groups; the outer list holds alternative groups of criteria.
struct advancement_requirements {
    std::vector<std::vector<std::string>> groups{};

    friend auto operator==(const advancement_requirements &,
                           const advancement_requirements &) -> bool = default;
};

[[nodiscard]] auto encode_advancement_requirements(
    writer &out, const advancement_requirements &value) -> status;
[[nodiscard]] auto
decode_advancement_requirements(reader &in, advancement_requirements &value)
    -> status;

/// One advancement: parent, display, requirements and telemetry flag.
struct advancement {
    std::optional<data::resource_location> parent{};
    bool has_display{false};
    advancement_display display{};
    advancement_requirements requirements{};
    bool sends_telemetry{false};

    friend auto operator==(const advancement &, const advancement &)
        -> bool = default;
};

[[nodiscard]] auto encode_advancement(writer &out, const advancement &value)
    -> status;
[[nodiscard]] auto decode_advancement(reader &in, advancement &value) -> status;

/// One criterion's completion state.
struct criterion_progress {
    std::string criterion{};
    std::optional<std::int64_t> obtained{};

    friend auto operator==(const criterion_progress &,
                           const criterion_progress &) -> bool = default;
};

/// Progress for one advancement.
struct advancement_progress {
    std::vector<criterion_progress> criteria{};

    friend auto operator==(const advancement_progress &,
                           const advancement_progress &) -> bool = default;
};

[[nodiscard]] auto
encode_advancement_progress(writer &out, const advancement_progress &value)
    -> status;
[[nodiscard]] auto decode_advancement_progress(reader &in,
                                               advancement_progress &value)
    -> status;

/// A set of item holders: either a named tag or a direct id list.
struct item_holder_set {
    bool named{false};
    data::resource_location tag{};
    std::vector<std::uint32_t> ids{};

    friend auto operator==(const item_holder_set &,
                           const item_holder_set &) -> bool = default;
};

[[nodiscard]] auto encode_holder_set(writer &out,
                                     const item_holder_set &value) -> status;
[[nodiscard]] auto decode_holder_set(reader &in, item_holder_set &value)
    -> status;

/// A recipe slot display; the type selects which fields are present.
struct slot_display {
    std::uint32_t type{0};
    std::uint32_t item_id{0};
    item_stack_template stack{};
    item_holder_set tag{};
    std::uint32_t component_type{0};
    std::uint32_t pattern_id{0};
    std::vector<slot_display> children{};

    friend auto operator==(const slot_display &, const slot_display &)
        -> bool = default;
};

[[nodiscard]] auto encode_slot_display(writer &out,
                                       const slot_display &value) -> status;
[[nodiscard]] auto decode_slot_display(reader &in, slot_display &value)
    -> status;

/// A recipe display: type 0 shapeless, 1 shaped, 2 furnace, 3 stonecutter,
/// 4 smithing.
struct recipe_display {
    std::uint32_t type{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t duration{0};
    float experience{0.0F};
    std::vector<slot_display> slots{};
    slot_display result{};
    slot_display station{};

    friend auto operator==(const recipe_display &, const recipe_display &)
        -> bool = default;
};

[[nodiscard]] auto encode_recipe_display(writer &out,
                                         const recipe_display &value)
    -> status;
[[nodiscard]] auto decode_recipe_display(reader &in, recipe_display &value)
    -> status;

/// One recipe book entry.
struct recipe_display_entry {
    std::uint32_t id{0};
    recipe_display display{};
    std::optional<std::uint32_t> group{};
    std::uint32_t category{0};
    bool has_requirements{false};
    std::vector<item_holder_set> requirements{};

    friend auto operator==(const recipe_display_entry &,
                           const recipe_display_entry &) -> bool = default;
};

[[nodiscard]] auto encode_recipe_display_entry(writer &out,
                                               const recipe_display_entry &value)
    -> status;
[[nodiscard]] auto decode_recipe_display_entry(reader &in,
                                               recipe_display_entry &value)
    -> status;

/// One selectable stonecutter recipe: an input set and its display.
struct selectable_recipe_entry {
    item_holder_set input{};
    slot_display display{};

    friend auto operator==(const selectable_recipe_entry &,
                           const selectable_recipe_entry &) -> bool = default;
};

/// The selectable recipe list sent with recipe updates.
struct single_input_set {
    std::vector<selectable_recipe_entry> entries{};

    friend auto operator==(const single_input_set &, const single_input_set &)
        -> bool = default;
};

[[nodiscard]] auto encode_single_input_set(writer &out,
                                           const single_input_set &value)
    -> status;
[[nodiscard]] auto decode_single_input_set(reader &in,
                                           single_input_set &value) -> status;

/// One entity metadata entry. The active representation is selected by the
/// serializer id; unused fields keep their defaults.
struct metadata_value {
    std::uint8_t index{0};
    std::uint32_t serializer{0};
    std::int64_t integer{0};
    double floating{0.0};
    std::string text{};
    text_component component{};
    item_stack stack{};
    bool flag{false};
    std::array<double, 4> vector{};
    std::array<std::uint32_t, 3> ids{};
    data::resource_location dimension{};
    std::int64_t position{0};

    friend auto operator==(const metadata_value &, const metadata_value &)
        -> bool = default;
};

/// An entity metadata list ending with the 255 terminator.
struct entity_metadata {
    std::vector<metadata_value> values{};

    friend auto operator==(const entity_metadata &, const entity_metadata &)
        -> bool = default;
};

[[nodiscard]] auto encode_entity_metadata(writer &out,
                                          const entity_metadata &value)
    -> status;
[[nodiscard]] auto decode_entity_metadata(reader &in, entity_metadata &value)
    -> status;

/// Writes a network-NBT string tag. The text must be valid UTF-8 without NUL
/// and within the Basic Multilingual Plane, because Java's modified UTF-8
/// encodes supplementary code points as surrogate pairs.
[[nodiscard]] auto encode_text_component(writer &out,
                                         const text_component &value) -> status;

/// Reads a network-NBT string tag into `value`, or returns
/// `error_code::unsupported` for any other tag type.
[[nodiscard]] auto decode_text_component(reader &in, text_component &value)
    -> status;

} // namespace refrain::net
