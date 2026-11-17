// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// World and player commands of the in-game tree: /tp, /setblock, /fill, /clone,
// /give, /me, /msg and /save-all.
//
// The reference reads each command's arguments through a typed Brigadier
// argument (Vec3Argument, BlockPosArgument, BlockStateArgument, ItemArgument,
// ...) and mutates the level it holds. This build has no content registry
// exposed through the world port, so the block-state and item arguments are
// parsed as words and resolved here against the runtime block registry and the
// item table the world provider exposes; an unknown item or an unresolvable
// block state fails the command instead of writing a placeholder.
//
// Bounds and iteration follow the reference: a region is rejected when it is
// not loaded before anything is written, the vertical bound is the model's
// section range instead of a dimension's height, and every region walk is
// ascending X, then Y, then Z so a capture of the same edit is reproducible.

#include "command_world_commands.hpp"

#include <refrain/command/parsers.hpp>
#include <refrain/sim/item.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::server {

namespace {

namespace command = refrain::command;
namespace sim = refrain::sim;
namespace world = refrain::world;

/// Default bound for one `/fill` or `/clone` region. The reference reads the
/// bound from the `max_block_modifications` game rule; game rules are not
/// modelled yet, so the command uses the rule's default value.
constexpr std::int64_t max_block_modifications = 32'768;

/// Horizontal world bounds of the reference (`Level.isInWorldBounds`).
constexpr std::int32_t world_horizontal_min = -30'000'000;
constexpr std::int32_t world_horizontal_max = 30'000'000;

/// Spawnable vertical bounds of the reference
/// (`Level.isOutsideSpawnableHeight`).
constexpr std::int32_t spawnable_y_min = -20'000'000;
constexpr std::int32_t spawnable_y_max = 20'000'000;

/// The reference's `/give` ceiling: 100 stacks of the item.
constexpr std::int64_t give_stack_limit = 100;

/// One resolved world coordinate. A relative or local component has already
/// been added to the command source anchor.
struct world_point {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

/// Where a relative coordinate resolves from: the feet position and rotation
/// of the command source. The console source sits at the origin without a
/// rotation, which is what its reference counterpart does as well.
struct anchor {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0F};
    float pitch{0.0F};
};

/// A whole block coordinate after validation.
struct block_coordinates {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t z{0};
};

/// Closed block region, corners already normalised to min/max.
struct block_region {
    block_coordinates min{};
    block_coordinates max{};

    [[nodiscard]] auto volume() const -> std::int64_t {
        return (static_cast<std::int64_t>(max.x) - min.x + 1) *
               (static_cast<std::int64_t>(max.y) - min.y + 1) *
               (static_cast<std::int64_t>(max.z) - min.z + 1);
    }
};

/// One resolved player a command may target. `record` is null when the active
/// world port keeps no in-process player table (the contract path), in which
/// case only the network connection is known.
struct resolved_player {
    std::string name{};
    std::uint64_t connection{sim::synthetic_connection};
    sim::player_state *record{nullptr};
};

struct player_pose {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0F};
    float pitch{0.0F};
};

/// Return one failed command: report the message to the source and hand it to
/// the dispatcher as the error value, so the bridge forwards it unchanged.
[[nodiscard]] auto fail(command::context &context, error_code code,
                        std::string message) -> refrain::result<int> {
    context.fail(message);
    return std::unexpected{make_error(code, message)};
}

[[nodiscard]] auto chunk_coordinate(std::int32_t block) noexcept
    -> std::int32_t {
    return block >= 0
               ? block / world::section_edge
               : (block - (world::section_edge - 1)) / world::section_edge;
}

[[nodiscard]] auto as_block_y(double value) noexcept -> std::int16_t {
    return static_cast<std::int16_t>(value);
}

[[nodiscard]] auto at_block(std::int32_t x, std::int32_t y, std::int32_t z)
    -> sim::block_pos {
    return sim::block_pos{.x = x, .y = as_block_y(y), .z = z};
}

/// Resolve the player that ran the command. A command source that is not a
/// player and has no target argument has no entity to move, which is the
/// reference's "no entity was found" case.
[[nodiscard]] auto
resolve_source_player(const std::shared_ptr<command_environment> &environment,
                      command::context &context)
    -> std::expected<resolved_player, std::string> {
    const auto connection{
        environment->connection_for(context.source().identity().key)};
    if (!connection.has_value()) {
        return std::unexpected{
            "Only a player can run this command without a target"};
    }
    resolved_player target{std::string{context.source().name()}, *connection,
                           nullptr};
    if (environment->world != nullptr) {
        if (auto *players = environment->world->players(); players != nullptr) {
            target.record = players->find_by_connection(*connection);
        }
    }
    return target;
}

[[nodiscard]] auto
pose_of(const std::shared_ptr<command_environment> &environment,
        const resolved_player &player)
    -> std::expected<player_pose, std::string> {
    if (player.record != nullptr) {
        return player_pose{player.record->x, player.record->y, player.record->z,
                           player.record->yaw, player.record->pitch};
    }
    if (environment->world != nullptr) {
        auto projection{environment->world->player(player.connection)};
        if (projection) {
            return player_pose{projection->x, projection->y, projection->z,
                               projection->yaw, projection->pitch};
        }
    }
    return std::unexpected{
        fmt::format("No position is available for '{}'", player.name)};
}

/// The anchor relative coordinates resolve from. Absolute-only arguments do not
/// need it, so callers ask for it lazily and a source without a position still
/// runs a command whose input is fully absolute.
[[nodiscard]] auto
source_anchor(const std::shared_ptr<command_environment> &environment,
              command::context &context) -> std::expected<anchor, std::string> {
    const auto &source{context.source()};
    if (!source.identity().player) {
        return anchor{};
    }
    auto player{resolve_source_player(environment, context)};
    if (!player) {
        return std::unexpected{std::move(player.error())};
    }
    auto pose{pose_of(environment, *player)};
    if (!pose) {
        return std::unexpected{std::move(pose.error())};
    }
    return anchor{pose->x, pose->y, pose->z, pose->yaw, pose->pitch};
}

/// How many results a selector argument accepts at execution time.
enum class selector_cardinality : std::uint8_t { one, any };

/// The candidates one selector resolution sees, plus the executor's view of
/// each of them. The two vectors are index-aligned: resolving a selector yields
/// indices into the candidate list, which the executor maps back to the player
/// names, connections and records.
struct selector_pool {
    std::vector<command::selector_candidate> candidates{};
    std::vector<resolved_player> targets{};
};

/// Convert a protocol game mode number to the command module's typed value.
[[nodiscard]] auto typed_gamemode(std::uint32_t value) noexcept
    -> std::optional<command::game_mode> {
    switch (value) {
    case sim::game_mode_survival:
        return command::game_mode::survival;
    case sim::game_mode_creative:
        return command::game_mode::creative;
    case sim::game_mode_adventure:
        return command::game_mode::adventure;
    case sim::game_mode_spectator:
        return command::game_mode::spectator;
    default:
        return std::nullopt;
    }
}

/// Build the candidates a selector may match, in ascending entity id order.
///
/// The world provider is the authority: its player projection carries position
/// and game mode for every player, and the in-process table additionally owns
/// the record `/give` writes to. A provider without a player projection (the
/// contract path of a composition that keeps no players in this process) falls
/// back to the network listener, which knows the players in play but not their
/// positions; a selector that needs a position then fails with a message
/// instead of guessing one.
[[nodiscard]] auto
collect_selector_pool(const std::shared_ptr<command_environment> &environment)
    -> std::expected<selector_pool, std::string> {
    selector_pool pool{};
    if (environment->world != nullptr) {
        auto count{environment->world->player_count()};
        if (!count) {
            if (environment->listener == nullptr) {
                return std::unexpected{fmt::format(
                    "The active world provider exposes no player records, so "
                    "entity selectors cannot resolve: {}",
                    count.error().message)};
            }
        } else {
            pool.candidates.reserve(*count);
            pool.targets.reserve(*count);
            auto *records{environment->world->players()};
            for (std::uint32_t index{0}; index < *count; ++index) {
                auto projected{environment->world->player_at(index)};
                if (!projected) {
                    return std::unexpected{
                        fmt::format("Could not read player record {}: {}",
                                    index, projected.error().message)};
                }
                command::selector_candidate candidate{};
                candidate.name = projected->name;
                candidate.identity = projected->connection;
                candidate.player = true;
                candidate.has_position = true;
                candidate.x = projected->x;
                candidate.y = projected->y;
                candidate.z = projected->z;
                candidate.mode = typed_gamemode(projected->game_mode);
                pool.candidates.push_back(std::move(candidate));
                resolved_player target{projected->name, projected->connection,
                                       nullptr};
                if (records != nullptr) {
                    target.record =
                        records->find_by_connection(projected->connection);
                }
                pool.targets.push_back(std::move(target));
            }
            return pool;
        }
    }
    if (environment->listener == nullptr) {
        return pool;
    }
    // The listener knows which players are in play but keeps no position; the
    // bridge's cached game mode is the only mode available here.
    auto names{environment->listener->player_names()};
    std::ranges::sort(names, {}, &net::server::player_name::connection);
    pool.candidates.reserve(names.size());
    pool.targets.reserve(names.size());
    for (const auto &player : names) {
        command::selector_candidate candidate{};
        candidate.name = player.name;
        candidate.identity = player.connection;
        candidate.player = true;
        candidate.has_position = false;
        candidate.mode =
            typed_gamemode(static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                environment->mode_of(player.connection))));
        pool.candidates.push_back(std::move(candidate));
        pool.targets.push_back(
            resolved_player{player.name, player.connection, nullptr});
    }
    return pool;
}

/// The position a selector origin resolves from: the source player's feet
/// position, or nothing for a source that is not a player or whose position the
/// world cannot report.
///
/// A missing position is not an error here: a selector that needs an origin
/// fails with its own message, while one that does not (an unfiltered `@a`, a
/// name target, a selector with all three absolute coordinates) still runs.
[[nodiscard]] auto
source_position_of(const std::shared_ptr<command_environment> &environment,
                   command::context &context)
    -> std::optional<command::selector_position> {
    if (!context.source().identity().player) {
        return std::nullopt;
    }
    const auto connection{
        environment->connection_for(context.source().identity().key)};
    if (!connection.has_value()) {
        return std::nullopt;
    }
    resolved_player self{std::string{context.source().name()}, *connection,
                         nullptr};
    if (environment->world != nullptr) {
        if (auto *players = environment->world->players(); players != nullptr) {
            self.record = players->find_by_connection(*connection);
        }
    }
    auto pose{pose_of(environment, self)};
    if (!pose) {
        return std::nullopt;
    }
    return command::selector_position{.x = pose->x, .y = pose->y, .z = pose->z};
}

/// Seed of the deterministic draw behind `@r` and `sort=random`.
///
/// The seed is derived from state instead of a global generator: the command
/// source identity, the world tick and the command text. A replay that runs the
/// same command at the same tick therefore makes the same choice, while
/// repeating the command on a later tick may pick a different player.
[[nodiscard]] auto
selector_random_seed(const std::shared_ptr<command_environment> &environment,
                     const command::context &context) noexcept
    -> std::uint64_t {
    std::uint64_t seed{1469598103934665603ULL};
    const auto mix = [&seed](std::uint64_t value) {
        for (int shift{0}; shift < 64; shift += 8) {
            seed ^= (value >> shift) & 0xFFULL;
            seed *= 1099511628211ULL;
        }
    };
    mix(context.source().identity().key);
    mix(environment->world != nullptr
            ? static_cast<std::uint64_t>(environment->world->ticks())
            : 0U);
    for (const char character : context.input()) {
        seed ^=
            static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        seed *= 1099511628211ULL;
    }
    return seed;
}

/// Resolve one selector argument against the live world and the command source.
///
/// This is the only place a selector reads player state, and it runs inside an
/// executor, never on the packet path. An empty result and a cardinality
/// violation become the reference's command errors; a resolution failure (no
/// source position, an unsupported entity type, a candidate without a position)
/// is forwarded with its own message.
[[nodiscard]] auto resolve_selector_targets(
    const std::shared_ptr<command_environment> &environment,
    command::context &context, const command::entity_selector &selector,
    selector_cardinality cardinality)
    -> std::expected<std::vector<resolved_player>, std::string> {
    auto pool{collect_selector_pool(environment)};
    if (!pool) {
        return std::unexpected{std::move(pool.error())};
    }
    command::selector_request request{};
    request.candidates = pool->candidates;
    request.source_position = source_position_of(environment, context);
    if (context.source().identity().player) {
        request.self_identity =
            environment->connection_for(context.source().identity().key);
    }
    request.random_seed = selector_random_seed(environment, context);
    auto matches{command::resolve_selector(selector, request)};
    if (!matches) {
        return std::unexpected{std::move(matches.error().message)};
    }
    std::vector<resolved_player> selected{};
    selected.reserve(matches->size());
    for (const auto index : *matches) {
        selected.push_back(pool->targets[index]);
    }
    if (selected.empty()) {
        return std::unexpected{std::string{selector.players_only
                                               ? "No player was found"
                                               : "No entity was found"}};
    }
    if (cardinality == selector_cardinality::one && selected.size() > 1) {
        return std::unexpected{std::string{
            selector.players_only
                ? "Only one player is allowed, but the provided selector "
                  "allows more than one"
                : "Only one entity is allowed, but the provided selector "
                  "allows more than one"}};
    }
    return selected;
}

[[nodiscard]] auto needs_anchor(const command::block_position &position)
    -> bool {
    return position.local || position.relative_x || position.relative_y ||
           position.relative_z;
}

[[nodiscard]] auto needs_anchor(const command::vec3_position &position)
    -> bool {
    return position.local || position.relative_x || position.relative_y ||
           position.relative_z;
}

/// Apply the reference's local (`^`) basis to a left/up/forwards offset. The
/// three components are the reference's `left`, `up` and `forwards`: the
/// source rotation is applied to its own axes, not to the world axes.
[[nodiscard]] auto apply_local(const anchor &from, double left_amount,
                               double up_amount,
                               double forwards_amount) noexcept -> world_point {
    constexpr double degrees_to_radians{3.14159265358979323846 / 180.0};
    const auto yaw{(static_cast<double>(from.yaw) + 90.0) * degrees_to_radians};
    const auto pitch{static_cast<double>(from.pitch) * degrees_to_radians};
    const auto yaw_cos{std::cos(yaw)};
    const auto yaw_sin{std::sin(yaw)};
    const auto pitch_cos{std::cos(-pitch)};
    const auto pitch_sin{std::sin(-pitch)};
    const auto pitch_up_cos{std::cos((-static_cast<double>(from.pitch) + 90.0) *
                                     degrees_to_radians)};
    const auto pitch_up_sin{std::sin((-static_cast<double>(from.pitch) + 90.0) *
                                     degrees_to_radians)};
    const std::array<double, 3> forwards{yaw_cos * pitch_cos, pitch_sin,
                                         yaw_sin * pitch_cos};
    const std::array<double, 3> up_vector{yaw_cos * pitch_up_cos, pitch_up_sin,
                                          yaw_sin * pitch_up_cos};
    // left = -(forwards x up)
    const std::array<double, 3> left{
        -(forwards[1] * up_vector[2] - forwards[2] * up_vector[1]),
        -(forwards[2] * up_vector[0] - forwards[0] * up_vector[2]),
        -(forwards[0] * up_vector[1] - forwards[1] * up_vector[0])};
    return world_point{
        .x = from.x + forwards[0] * forwards_amount + up_vector[0] * up_amount +
             left[0] * left_amount,
        .y = from.y + forwards[1] * forwards_amount + up_vector[1] * up_amount +
             left[1] * left_amount,
        .z = from.z + forwards[2] * forwards_amount + up_vector[2] * up_amount +
             left[2] * left_amount,
    };
}

[[nodiscard]] auto resolve_position(const command::vec3_position &position,
                                    const anchor &from) -> world_point {
    if (position.local) {
        return apply_local(from, position.x, position.y, position.z);
    }
    return world_point{
        .x = position.relative_x ? from.x + position.x : position.x,
        .y = position.relative_y ? from.y + position.y : position.y,
        .z = position.relative_z ? from.z + position.z : position.z,
    };
}

[[nodiscard]] auto resolve_position(const command::block_position &position,
                                    const anchor &from) -> world_point {
    if (position.local) {
        return apply_local(from, position.x, position.y, position.z);
    }
    return world_point{
        .x = position.relative_x ? from.x + position.x : position.x,
        .y = position.relative_y ? from.y + position.y : position.y,
        .z = position.relative_z ? from.z + position.z : position.z,
    };
}

/// Reject a position the reference's `BlockPosArgument.getLoadedBlockPos`
/// rejects: outside the horizontal world bounds or the model's build height.
[[nodiscard]] auto block_position_error(const world_point &value)
    -> std::optional<std::string> {
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z)) {
        return std::string{"The block position is not a finite coordinate"};
    }
    const auto x{std::floor(value.x)};
    const auto y{std::floor(value.y)};
    const auto z{std::floor(value.z)};
    if (x < world_horizontal_min || x >= world_horizontal_max ||
        z < world_horizontal_min || z >= world_horizontal_max) {
        return fmt::format("The block position ({}, {}, {}) is outside the "
                           "world border",
                           x, y, z);
    }
    if (y < static_cast<double>(sim::world::build_min_y()) ||
        y > static_cast<double>(sim::world::build_max_y())) {
        return fmt::format("Block Y {} is outside the build height {}..{}", y,
                           sim::world::build_min_y(),
                           sim::world::build_max_y());
    }
    return std::nullopt;
}

[[nodiscard]] auto to_block_coordinates(const world_point &value)
    -> block_coordinates {
    return block_coordinates{
        .x = static_cast<std::int32_t>(std::floor(value.x)),
        .y = static_cast<std::int32_t>(std::floor(value.y)),
        .z = static_cast<std::int32_t>(std::floor(value.z)),
    };
}

/// Reject a teleport destination outside the reference's spawnable bounds.
/// The vertical range is deliberately the reference's wide one, not the build
/// height: the reference lets a teleport leave the build height and lets the
/// player fall.
[[nodiscard]] auto teleport_position_error(const world_point &value)
    -> std::optional<std::string> {
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z)) {
        return std::string{"The teleport destination is not a finite position"};
    }
    const auto x{std::floor(value.x)};
    const auto y{std::floor(value.y)};
    const auto z{std::floor(value.z)};
    if (x < world_horizontal_min || x >= world_horizontal_max ||
        z < world_horizontal_min || z >= world_horizontal_max ||
        y < spawnable_y_min || y >= spawnable_y_max) {
        return std::string{"The teleport destination is outside the world"};
    }
    return std::nullopt;
}

/// Resolve one block-state argument against the runtime registry and prove the
/// canonical key round-trips. A port that cannot name a block state cannot run
/// the command, so it fails instead of writing air for every requested block.
[[nodiscard]] auto resolve_block_state(world_port &level, std::string_view text)
    -> std::expected<std::pair<std::string, sim::block_state_id>, std::string> {
    auto parsed{world::parse_block_state_key(text)};
    if (!parsed) {
        return std::unexpected{
            fmt::format("'{}' is not a valid block state: {}", text,
                        parsed.error().message)};
    }
    auto canonical{world::format_block_state_key(*parsed)};
    const auto id{level.intern_block(canonical)};
    if (level.block_key(id) != canonical) {
        return std::unexpected{fmt::format(
            "The active world provider cannot resolve the block state '{}'",
            canonical)};
    }
    return std::pair{std::move(canonical), id};
}

/// Every column of `region` must already be loadable, which is the reference's
/// loaded-chunk precondition for a block edit. The check walks the columns in
/// ascending order and reports the first one that is missing.
[[nodiscard]] auto ensure_region_loaded(world_port &level,
                                        const block_region &region)
    -> std::optional<std::string> {
    const auto min_chunk_x{chunk_coordinate(region.min.x)};
    const auto max_chunk_x{chunk_coordinate(region.max.x)};
    const auto min_chunk_z{chunk_coordinate(region.min.z)};
    const auto max_chunk_z{chunk_coordinate(region.max.z)};
    for (auto chunk_z{min_chunk_z}; chunk_z <= max_chunk_z; ++chunk_z) {
        for (auto chunk_x{min_chunk_x}; chunk_x <= max_chunk_x; ++chunk_x) {
            auto column{level.chunk_load(sim::chunk_pos{chunk_x, chunk_z})};
            if (!column) {
                return fmt::format(
                    "The area around chunk ({}, {}) is not loaded: {}", chunk_x,
                    chunk_z, column.error().message);
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto region_of(block_coordinates first, block_coordinates second)
    -> block_region {
    return block_region{
        .min =
            block_coordinates{
                .x = std::min(first.x, second.x),
                .y = std::min(first.y, second.y),
                .z = std::min(first.z, second.z),
            },
        .max =
            block_coordinates{
                .x = std::max(first.x, second.x),
                .y = std::max(first.y, second.y),
                .z = std::max(first.z, second.z),
            },
    };
}

/// Resolve one block-position argument, requiring an anchor only when the
/// input is relative or local.
template <typename Position>
[[nodiscard]] auto
resolve_block_argument(const std::shared_ptr<command_environment> &environment,
                       command::context &context, std::string_view name,
                       const Position &position)
    -> std::expected<block_coordinates, std::string> {
    if (position.local) {
        return std::unexpected{
            std::string{"You cannot mix world and local coordinates"}};
    }
    anchor from{};
    if (needs_anchor(position)) {
        auto resolved{source_anchor(environment, context)};
        if (!resolved) {
            return std::unexpected{std::move(resolved.error())};
        }
        from = *resolved;
    }
    const auto point{resolve_position(position, from)};
    if (auto problem{block_position_error(point)}; problem.has_value()) {
        return std::unexpected{
            fmt::format("{} (argument '{}')", *problem, name)};
    }
    return to_block_coordinates(point);
}

/// Resolve one `vec3` argument; local coordinates are allowed, unlike a block
/// position.
[[nodiscard]] auto
resolve_vec3_argument(const std::shared_ptr<command_environment> &environment,
                      command::context &context,
                      const command::vec3_position &position)
    -> std::expected<world_point, std::string> {
    anchor from{};
    if (needs_anchor(position)) {
        auto resolved{source_anchor(environment, context)};
        if (!resolved) {
            return std::unexpected{std::move(resolved.error())};
        }
        from = *resolved;
    }
    return resolve_position(position, from);
}

/// Region bound shared by `/fill` and `/clone`. The message mirrors the
/// reference's `commands.fill.toobig` wording.
[[nodiscard]] auto region_too_large(const block_region &region)
    -> std::optional<std::string> {
    const auto volume{region.volume()};
    if (volume > max_block_modifications) {
        return fmt::format("Too many blocks in the specified area "
                           "(maximum {}, found {})",
                           max_block_modifications, volume);
    }
    return std::nullopt;
}

// --- /tp -------------------------------------------------------------------

[[nodiscard]] auto
teleport_to(const std::shared_ptr<command_environment> &environment,
            command::context &context, std::span<const resolved_player> targets,
            const world_point &destination,
            std::optional<player_pose> destination_pose)
    -> refrain::result<int> {
    if (environment->world == nullptr) {
        return fail(context, error_code::not_found, "No world is open");
    }
    if (auto problem{teleport_position_error(destination)};
        problem.has_value()) {
        return fail(context, error_code::invalid_argument, *problem);
    }
    std::vector<std::string> names{};
    names.reserve(targets.size());
    for (const auto &target : targets) {
        float yaw{0.0F};
        float pitch{0.0F};
        if (destination_pose.has_value()) {
            yaw = destination_pose->yaw;
            pitch = destination_pose->pitch;
        } else {
            auto pose{pose_of(environment, target)};
            if (!pose) {
                return fail(context, error_code::not_found,
                            std::move(pose.error()));
            }
            yaw = pose->yaw;
            pitch = pose->pitch;
        }
        auto moved{environment->world->set_player_transform(
            target.connection, destination.x, destination.y, destination.z, yaw,
            pitch)};
        if (!moved) {
            return fail(context, moved.error().code,
                        fmt::format("Could not teleport '{}': {}", target.name,
                                    moved.error().message));
        }
        if (environment->listener != nullptr &&
            target.connection != sim::synthetic_connection) {
            environment->listener->teleport_player(target.connection,
                                                   destination.x, destination.y,
                                                   destination.z, yaw, pitch);
        }
        names.push_back(target.name);
    }
    if (names.size() == 1) {
        context.reply(fmt::format("Teleported {} to {}, {}, {}", names.front(),
                                  destination.x, destination.y, destination.z));
    } else {
        context.reply(fmt::format("Teleported {} players to {}, {}, {}",
                                  names.size(), destination.x, destination.y,
                                  destination.z));
    }
    return static_cast<int>(names.size());
}

void register_teleport(
    command::dispatcher &dispatcher,
    const std::shared_ptr<command_environment> &environment) {
    auto teleport{dispatcher.literal("teleport")
                      .require_node("refrain.command.teleport",
                                    command::level::gamemode)};

    // `/teleport <location>`: move the source entity to an absolute, relative
    // or local position.
    auto location{dispatcher.argument("location", command::vec3_arg())};
    location.executes([environment](
                          command::context &context) -> refrain::result<int> {
        const auto *argument{context.vec3("location")};
        if (argument == nullptr) {
            return fail(context, error_code::invalid_argument,
                        "The teleport position is missing");
        }
        auto position{resolve_vec3_argument(environment, context, *argument)};
        if (!position) {
            return fail(context, error_code::invalid_argument,
                        std::move(position.error()));
        }
        auto self{resolve_source_player(environment, context)};
        if (!self) {
            return fail(context, error_code::not_found,
                        std::move(self.error()));
        }
        const std::array<resolved_player, 1> targets{*self};
        return teleport_to(environment, context, targets, *position,
                           std::nullopt);
    });
    teleport.then(location);

    // `/teleport <targets> [<location>|<destination>]` and `/teleport
    // <destination>`. The tree carries the selector argument once: the parser
    // cannot backtrack, so the reference's separate single-entity
    // `destination` sibling of `targets` would shadow the two-token forms. The
    // executable `targets` node therefore also accepts the one-token form and
    // requires exactly one result there.
    auto targets{
        dispatcher.argument("targets", command::entity_selector_arg())};
    targets.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto *argument{context.selector("targets")};
            if (argument == nullptr) {
                return fail(context, error_code::invalid_argument,
                            "The teleport target is missing");
            }
            auto self{resolve_source_player(environment, context)};
            if (!self) {
                return fail(context, error_code::not_found,
                            std::move(self.error()));
            }
            auto selected{resolve_selector_targets(
                environment, context, *argument, selector_cardinality::one)};
            if (!selected) {
                return fail(context, error_code::not_found,
                            std::move(selected.error()));
            }
            const auto &destination{selected->front()};
            auto pose{pose_of(environment, destination)};
            if (!pose) {
                return fail(context, error_code::not_found,
                            std::move(pose.error()));
            }
            const world_point landing{pose->x, pose->y, pose->z};
            const std::array<resolved_player, 1> moved{*self};
            return teleport_to(environment, context, moved, landing,
                               std::optional{*pose});
        });
    auto targets_location{dispatcher.argument("location", command::vec3_arg())};
    targets_location.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto *argument{context.selector("targets")};
            if (argument == nullptr) {
                return fail(context, error_code::invalid_argument,
                            "The teleport target is missing");
            }
            auto selected{resolve_selector_targets(
                environment, context, *argument, selector_cardinality::any)};
            if (!selected) {
                return fail(context, error_code::not_found,
                            std::move(selected.error()));
            }
            const auto *location_argument{context.vec3("location")};
            if (location_argument == nullptr) {
                return fail(context, error_code::invalid_argument,
                            "The teleport position is missing");
            }
            auto position{resolve_vec3_argument(environment, context,
                                                *location_argument)};
            if (!position) {
                return fail(context, error_code::invalid_argument,
                            std::move(position.error()));
            }
            return teleport_to(environment, context, *selected, *position,
                               std::nullopt);
        });
    targets.then(targets_location);
    auto targets_destination{
        dispatcher.argument("destination", command::entity_selector_arg(true))};
    targets_destination.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto *argument{context.selector("targets")};
            const auto *destination_argument{context.selector("destination")};
            if (argument == nullptr || destination_argument == nullptr) {
                return fail(context, error_code::invalid_argument,
                            "The teleport target is missing");
            }
            auto selected{resolve_selector_targets(
                environment, context, *argument, selector_cardinality::any)};
            if (!selected) {
                return fail(context, error_code::not_found,
                            std::move(selected.error()));
            }
            auto destination{resolve_selector_targets(
                environment, context, *destination_argument,
                selector_cardinality::one)};
            if (!destination) {
                return fail(context, error_code::not_found,
                            std::move(destination.error()));
            }
            auto pose{pose_of(environment, destination->front())};
            if (!pose) {
                return fail(context, error_code::not_found,
                            std::move(pose.error()));
            }
            const world_point landing{pose->x, pose->y, pose->z};
            return teleport_to(environment, context, *selected, landing,
                               std::optional{*pose});
        });
    targets.then(targets_destination);
    teleport.then(targets);

    // `/tp` is the reference's alias: a redirect, so the alias and the full
    // name share one subtree and one permission check.
    dispatcher.literal("tp")
        .require_node("refrain.command.teleport", command::level::gamemode)
        .redirect(teleport);
}

// --- /setblock -------------------------------------------------------------

enum class setblock_mode : std::uint8_t { replace, keep };

void register_setblock(
    command::dispatcher &dispatcher,
    const std::shared_ptr<command_environment> &environment) {
    const auto place =
        [environment](command::context &context,
                      setblock_mode mode) -> refrain::result<int> {
        auto *level{environment->world};
        if (level == nullptr) {
            return fail(context, error_code::not_found, "No world is open");
        }
        const auto *position{context.position("pos")};
        if (position == nullptr) {
            return fail(context, error_code::invalid_argument,
                        "The block position is missing");
        }
        auto coordinates{
            resolve_block_argument(environment, context, "pos", *position)};
        if (!coordinates) {
            return fail(context, error_code::invalid_argument,
                        std::move(coordinates.error()));
        }
        const auto block{*coordinates};
        if (auto problem{ensure_region_loaded(*level, region_of(block, block))};
            problem.has_value()) {
            return fail(context, error_code::not_found, std::move(*problem));
        }
        auto state{resolve_block_state(*level, context.string("block"))};
        if (!state) {
            return fail(context, error_code::invalid_argument,
                        std::move(state.error()));
        }
        const auto target{at_block(block.x, block.y, block.z)};
        const auto previous{level->block_at(target)};
        if (mode == setblock_mode::keep && previous != sim::air_block) {
            return fail(
                context, error_code::invalid_argument,
                fmt::format("Could not place {} at {}, {}, {} because the "
                            "position is not empty",
                            state->first, block.x, block.y, block.z));
        }
        if (auto written{level->set_block(target, state->second)}; !written) {
            return fail(context, written.error().code,
                        fmt::format("Could not place the block at {}, {}, {}: "
                                    "{}",
                                    block.x, block.y, block.z,
                                    written.error().message));
        }
        context.reply(fmt::format("Placed {} at {}, {}, {}", state->first,
                                  block.x, block.y, block.z));
        return 1;
    };

    auto setblock{dispatcher.literal("setblock")
                      .require_node("refrain.command.setblock",
                                    command::level::gamemode)};
    auto position{dispatcher.argument("pos", command::block_position_arg())};
    auto block{dispatcher.argument("block", command::word_arg())};
    block.executes([place](command::context &context) -> refrain::result<int> {
        return place(context, setblock_mode::replace);
    });
    block.then(dispatcher.literal("replace").executes(
        [place](command::context &context) -> refrain::result<int> {
            return place(context, setblock_mode::replace);
        }));
    block.then(dispatcher.literal("keep").executes(
        [place](command::context &context) -> refrain::result<int> {
            return place(context, setblock_mode::keep);
        }));
    position.then(block);
    setblock.then(position);
}

// --- /fill -----------------------------------------------------------------

enum class fill_mode : std::uint8_t { replace, keep, hollow, outline };

void register_fill(command::dispatcher &dispatcher,
                   const std::shared_ptr<command_environment> &environment) {
    const auto run_fill =
        [environment](command::context &context,
                      fill_mode mode) -> refrain::result<int> {
        auto *level{environment->world};
        if (level == nullptr) {
            return fail(context, error_code::not_found, "No world is open");
        }
        const auto *from{context.position("from")};
        const auto *to{context.position("to")};
        if (from == nullptr || to == nullptr) {
            return fail(context, error_code::invalid_argument,
                        "The fill area is incomplete");
        }
        auto first{resolve_block_argument(environment, context, "from", *from)};
        if (!first) {
            return fail(context, error_code::invalid_argument,
                        std::move(first.error()));
        }
        auto last{resolve_block_argument(environment, context, "to", *to)};
        if (!last) {
            return fail(context, error_code::invalid_argument,
                        std::move(last.error()));
        }
        const auto region{region_of(*first, *last)};
        if (auto problem{region_too_large(region)}; problem.has_value()) {
            return fail(context, error_code::invalid_argument,
                        std::move(*problem));
        }
        if (auto problem{ensure_region_loaded(*level, region)};
            problem.has_value()) {
            return fail(context, error_code::not_found, std::move(*problem));
        }
        auto state{resolve_block_state(*level, context.string("block"))};
        if (!state) {
            return fail(context, error_code::invalid_argument,
                        std::move(state.error()));
        }
        std::vector<sim::block_change> changes{};
        changes.reserve(static_cast<std::size_t>(region.volume()));
        for (auto z{region.min.z}; z <= region.max.z; ++z) {
            for (auto y{region.min.y}; y <= region.max.y; ++y) {
                for (auto x{region.min.x}; x <= region.max.x; ++x) {
                    const auto shell{x == region.min.x || x == region.max.x ||
                                     y == region.min.y || y == region.max.y ||
                                     z == region.min.z || z == region.max.z};
                    const auto target{at_block(x, y, z)};
                    const auto previous{level->block_at(target)};
                    if (mode == fill_mode::keep && previous != sim::air_block) {
                        continue;
                    }
                    sim::block_state_id next{state->second};
                    if (mode == fill_mode::outline && !shell) {
                        continue;
                    }
                    if (mode == fill_mode::hollow && !shell) {
                        next = sim::air_block;
                    }
                    if (previous == next) {
                        continue;
                    }
                    changes.push_back(sim::block_change{
                        .pos = target,
                        .old_state = previous,
                        .new_state = next,
                    });
                }
            }
        }
        if (changes.empty()) {
            return fail(context, error_code::invalid_argument,
                        "No blocks were filled");
        }
        auto applied{level->set_blocks(changes)};
        if (!applied) {
            return fail(context, applied.error().code,
                        fmt::format("Could not fill the area: {}",
                                    applied.error().message));
        }
        context.reply(fmt::format("Filled {} blocks",
                                  static_cast<std::int64_t>(*applied)));
        return static_cast<int>(*applied);
    };

    auto node{dispatcher.literal("fill").require_node(
        "refrain.command.fill", command::level::gamemode)};
    auto from{dispatcher.argument("from", command::block_position_arg())};
    auto to{dispatcher.argument("to", command::block_position_arg())};
    auto block{dispatcher.argument("block", command::word_arg())};
    block.executes(
        [run_fill](command::context &context) -> refrain::result<int> {
            return run_fill(context, fill_mode::replace);
        });
    const std::array<std::pair<std::string_view, fill_mode>, 4> modes{{
        {"replace", fill_mode::replace},
        {"keep", fill_mode::keep},
        {"hollow", fill_mode::hollow},
        {"outline", fill_mode::outline},
    }};
    for (const auto &[name, mode] : modes) {
        block.then(dispatcher.literal(name).executes(
            [run_fill, mode](command::context &context)
                -> refrain::result<int> { return run_fill(context, mode); }));
    }
    to.then(block);
    from.then(to);
    node.then(from);
}

// --- /clone ----------------------------------------------------------------

enum class clone_mode : std::uint8_t { replace, masked };

void register_clone(command::dispatcher &dispatcher,
                    const std::shared_ptr<command_environment> &environment) {
    const auto clone = [environment](command::context &context,
                                     clone_mode mode) -> refrain::result<int> {
        auto *level{environment->world};
        if (level == nullptr) {
            return fail(context, error_code::not_found, "No world is open");
        }
        const auto *begin{context.position("from")};
        const auto *end{context.position("to")};
        const auto *destination{context.position("destination")};
        if (begin == nullptr || end == nullptr || destination == nullptr) {
            return fail(context, error_code::invalid_argument,
                        "The clone area is incomplete");
        }
        auto first{
            resolve_block_argument(environment, context, "from", *begin)};
        if (!first) {
            return fail(context, error_code::invalid_argument,
                        std::move(first.error()));
        }
        auto last{resolve_block_argument(environment, context, "to", *end)};
        if (!last) {
            return fail(context, error_code::invalid_argument,
                        std::move(last.error()));
        }
        auto target{resolve_block_argument(environment, context, "destination",
                                           *destination)};
        if (!target) {
            return fail(context, error_code::invalid_argument,
                        std::move(target.error()));
        }
        const auto source{region_of(*first, *last)};
        if (auto problem{region_too_large(source)}; problem.has_value()) {
            return fail(context, error_code::invalid_argument,
                        std::move(*problem));
        }
        // The destination box has the source's spans, anchored at its min
        // corner; validate the far corner too so no destination block can be
        // outside the world.
        const auto destination_region{region_of(
            *target, block_coordinates{
                         .x = target->x + (source.max.x - source.min.x),
                         .y = target->y + (source.max.y - source.min.y),
                         .z = target->z + (source.max.z - source.min.z),
                     })};
        if (auto problem{ensure_region_loaded(*level, source)};
            problem.has_value()) {
            return fail(context, error_code::not_found, std::move(*problem));
        }
        if (auto problem{ensure_region_loaded(*level, destination_region)};
            problem.has_value()) {
            return fail(context, error_code::not_found, std::move(*problem));
        }
        // Snapshot the source before writing: the two regions may overlap, and
        // a snapshot is what makes the overlap order-independent instead of
        // reading blocks this command already replaced.
        struct cloned_block {
            std::int32_t x{0};
            std::int32_t y{0};
            std::int32_t z{0};
            sim::block_state_id state{sim::air_block};
        };
        std::vector<cloned_block> snapshot{};
        snapshot.reserve(static_cast<std::size_t>(source.volume()));
        for (auto z{source.min.z}; z <= source.max.z; ++z) {
            for (auto y{source.min.y}; y <= source.max.y; ++y) {
                for (auto x{source.min.x}; x <= source.max.x; ++x) {
                    const auto state{level->block_at(at_block(x, y, z))};
                    if (mode == clone_mode::masked && state == sim::air_block) {
                        continue;
                    }
                    snapshot.push_back(cloned_block{
                        .x = x,
                        .y = y,
                        .z = z,
                        .state = state,
                    });
                }
            }
        }
        std::vector<sim::block_change> changes{};
        changes.reserve(snapshot.size());
        for (const auto &entry : snapshot) {
            const auto target_pos{
                at_block(entry.x - source.min.x + destination_region.min.x,
                         entry.y - source.min.y + destination_region.min.y,
                         entry.z - source.min.z + destination_region.min.z)};
            const auto previous{level->block_at(target_pos)};
            if (previous == entry.state) {
                continue;
            }
            changes.push_back(sim::block_change{
                .pos = target_pos,
                .old_state = previous,
                .new_state = entry.state,
            });
        }
        if (changes.empty()) {
            return fail(context, error_code::invalid_argument,
                        "No blocks were cloned");
        }
        auto applied{level->set_blocks(changes)};
        if (!applied) {
            return fail(context, applied.error().code,
                        fmt::format("Could not clone the area: {}",
                                    applied.error().message));
        }
        context.reply(fmt::format("Cloned {} blocks",
                                  static_cast<std::int64_t>(*applied)));
        return static_cast<int>(*applied);
    };

    auto node{dispatcher.literal("clone").require_node(
        "refrain.command.clone", command::level::gamemode)};
    auto from{dispatcher.argument("from", command::block_position_arg())};
    auto to{dispatcher.argument("to", command::block_position_arg())};
    auto destination{
        dispatcher.argument("destination", command::block_position_arg())};
    destination.executes(
        [clone](command::context &context) -> refrain::result<int> {
            return clone(context, clone_mode::replace);
        });
    const std::array<std::pair<std::string_view, clone_mode>, 2> modes{{
        {"replace", clone_mode::replace},
        {"masked", clone_mode::masked},
    }};
    for (const auto &[name, mode] : modes) {
        destination.then(dispatcher.literal(name).executes(
            [clone, mode](command::context &context) -> refrain::result<int> {
                return clone(context, mode);
            }));
    }
    to.then(destination);
    from.then(to);
    node.then(from);
}

// --- /give -----------------------------------------------------------------

void register_give(command::dispatcher &dispatcher,
                   const std::shared_ptr<command_environment> &environment) {
    const auto give =
        [environment](command::context &context) -> refrain::result<int> {
        auto *level{environment->world};
        if (level == nullptr) {
            return fail(context, error_code::not_found, "No world is open");
        }
        const auto *argument{context.selector("targets")};
        if (argument == nullptr) {
            return fail(context, error_code::invalid_argument,
                        "The give target is missing");
        }
        auto selected{resolve_selector_targets(environment, context, *argument,
                                               selector_cardinality::any)};
        if (!selected) {
            return fail(context, error_code::not_found,
                        std::move(selected.error()));
        }
        const auto *item{context.location("item")};
        if (item == nullptr) {
            return fail(context, error_code::invalid_argument,
                        "The item id is missing");
        }
        const auto item_name{item->to_string()};
        const auto *items{level->items()};
        if (items == nullptr) {
            return fail(context, error_code::unsupported,
                        "The active world provider keeps no item registry");
        }
        const auto *definition{items->find(item_name)};
        if (definition == nullptr) {
            return fail(context, error_code::not_found,
                        fmt::format("Unknown item '{}'", item_name));
        }
        const auto count{context.has("count") ? context.integer("count") : 1};
        const auto maximum{
            static_cast<std::int64_t>(definition->max_stack_size) *
            give_stack_limit};
        if (count < 1 || count > maximum) {
            return fail(context, error_code::invalid_argument,
                        fmt::format("Cannot give more than {} items", maximum));
        }
        for (const auto &player : *selected) {
            if (player.record == nullptr) {
                return fail(context, error_code::unsupported,
                            fmt::format("The active world provider keeps no "
                                        "inventory for '{}'",
                                        player.name));
            }
            auto added{level->add_item(*player.record, item_name,
                                       static_cast<std::uint32_t>(count))};
            if (!added) {
                return fail(context, added.error().code,
                            fmt::format("Could not add {} to the inventory of "
                                        "'{}': {}",
                                        item_name, player.name,
                                        added.error().message));
            }
        }
        if (selected->size() == 1U) {
            context.reply(fmt::format("Gave {} {} to {}", count, item_name,
                                      selected->front().name));
        } else {
            context.reply(fmt::format("Gave {} {} to {} players", count,
                                      item_name, selected->size()));
        }
        return static_cast<int>(count);
    };

    auto node{dispatcher.literal("give").require_node(
        "refrain.command.give", command::level::gamemode)};
    auto targets{dispatcher.argument(
        "targets", command::entity_selector_arg(false, true))};
    auto item{dispatcher.argument("item", command::resource_location_arg())};
    item.executes([give](command::context &context) -> refrain::result<int> {
        return give(context);
    });
    auto count{dispatcher.argument("count", command::integer_arg(1))};
    count.executes([give](command::context &context) -> refrain::result<int> {
        return give(context);
    });
    item.then(count);
    targets.then(item);
    node.then(targets);
}

// --- /me and /msg ----------------------------------------------------------

void register_chat_commands(
    command::dispatcher &dispatcher,
    const std::shared_ptr<command_environment> &environment) {
    auto me{dispatcher.literal("me").require_node("refrain.command.me")};
    auto action{dispatcher.argument("action", command::greedy_arg())};
    action.executes([environment](
                        command::context &context) -> refrain::result<int> {
        const auto text{context.string("action")};
        const auto line{fmt::format("* {} {}", context.source().name(), text)};
        if (environment->listener != nullptr) {
            environment->listener->broadcast_system_chat(line);
        } else {
            context.reply(line);
        }
        return 1;
    });
    me.then(action);

    auto msg{dispatcher.literal("msg").require_node("refrain.command.msg")};
    auto targets{dispatcher.argument(
        "targets", command::entity_selector_arg(false, true))};
    auto message{dispatcher.argument("message", command::greedy_arg())};
    message.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto *argument{context.selector("targets")};
            if (argument == nullptr) {
                return fail(context, error_code::invalid_argument,
                            "The message target is missing");
            }
            auto selected{resolve_selector_targets(
                environment, context, *argument, selector_cardinality::any)};
            if (!selected) {
                return fail(context, error_code::not_found,
                            std::move(selected.error()));
            }
            const auto text{context.string("message")};
            const auto sender{context.source().name()};
            for (const auto &player : *selected) {
                if (environment->listener != nullptr &&
                    player.connection != sim::synthetic_connection) {
                    environment->listener->send_system_chat(
                        player.connection,
                        fmt::format("{} whispers to you: {}", sender, text));
                }
                context.reply(
                    fmt::format("You whisper to {}: {}", player.name, text));
            }
            return static_cast<int>(selected->size());
        });
    targets.then(message);
    msg.then(targets);
    // The reference registers both aliases as redirects of `/msg`.
    dispatcher.literal("tell")
        .require_node("refrain.command.msg")
        .redirect(msg);
    dispatcher.literal("w").require_node("refrain.command.msg").redirect(msg);
}

// --- /save-all -------------------------------------------------------------

void register_save_all(
    command::dispatcher &dispatcher,
    const std::shared_ptr<command_environment> &environment) {
    const auto save =
        [environment](command::context &context) -> refrain::result<int> {
        if (environment->world == nullptr) {
            return fail(context, error_code::not_found, "No world is open");
        }
        if (auto flushed{environment->world->flush()}; !flushed) {
            return fail(context, flushed.error().code,
                        fmt::format("Could not save the world: {}",
                                    flushed.error().message));
        }
        context.reply("Saved the world");
        return 1;
    };
    auto node{
        dispatcher.literal("save-all")
            .require_node("refrain.command.save-all", command::level::owner)};
    node.executes(save);
    node.then(dispatcher.literal("flush").executes(save));
}

} // namespace

void register_world_commands(std::shared_ptr<command_environment> environment) {
    if (!environment) {
        return;
    }
    auto &dispatcher{environment->dispatcher};
    register_teleport(dispatcher, environment);
    register_setblock(dispatcher, environment);
    register_fill(dispatcher, environment);
    register_clone(dispatcher, environment);
    register_give(dispatcher, environment);
    register_chat_commands(dispatcher, environment);
    register_save_all(dispatcher, environment);
}

} // namespace refrain::server
