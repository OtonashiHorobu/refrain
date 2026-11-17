// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/argument.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::command {

[[nodiscard]] auto to_string(selector_kind kind) noexcept -> std::string_view;

/// True when `value_squared` lies inside the squared bounds of `range`.
///
/// The distance filter compares squared distances, so the bounds are compared
/// squared as well; that is the reference's `MinMaxBounds.Doubles.matchesSqr`
/// and avoids a square root per candidate.
[[nodiscard]] auto selector_range_matches_squared(const selector_range &range,
                                                  double value_squared) noexcept
    -> bool;

/// A point in the world, as a selector origin or a candidate position.
struct selector_position {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    friend auto operator==(const selector_position &, const selector_position &)
        -> bool = default;
};

/// One entity a selector may match, as the executing world describes it.
///
/// The command module never reads a player table itself: the executor builds
/// the candidate list when the command runs and hands it to `resolve_selector`.
/// A candidate that has no position still matches a selector that needs none.
struct selector_candidate {
    std::string name{};
    /// Opaque identity of the entity within one resolution. For a player this
    /// is the network connection, which is what `@s` compares with the source.
    std::uint64_t identity{0};
    /// False when the executor models the candidate as a non-player entity.
    /// Every candidate of this build is a player.
    bool player{true};
    bool has_position{false};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    /// Hitbox size used by the `dx`/`dy`/`dz` box filter. The defaults are a
    /// player's, the only entity the command layer models.
    double width{0.6};
    double height{1.8};
    /// Game mode of a player candidate; null when the executor cannot tell.
    std::optional<game_mode> mode{};

    friend auto operator==(const selector_candidate &,
                           const selector_candidate &) -> bool = default;
};

/// Everything resolving one selector needs from the executing command.
struct selector_request {
    /// Candidates in a state-derived order, normally ascending entity id.
    std::span<const selector_candidate> candidates{};
    /// Feet position of the command source, the origin the `distance`, box and
    /// sorting filters resolve against. Null when the source has no position
    /// (the console); a selector that needs an origin then fails with
    /// `no_source_position` unless its `x`/`y`/`z` overrides supply one.
    std::optional<selector_position> source_position{};
    /// Identity of the source entity for `@s`; null when the source is not an
    /// entity.
    std::optional<std::uint64_t> self_identity{};
    /// Seed of the deterministic draw behind `@r` and `sort=random`. The
    /// executor derives it from state, so a replay reproduces the same choice.
    std::uint64_t random_seed{0};
};

/// Why a selector could not be resolved. An empty match list is not an error;
/// the executor turns it into the reference's "not found" message.
struct selector_error {
    enum class kind : std::uint8_t {
        /// The selector needs an origin and the source has none.
        no_source_position = 0,
        /// A matching candidate has no position for a distance or box filter.
        missing_candidate_position,
        /// The `type=` filter names a type this build does not model.
        unsupported_entity_type,
    };

    kind failure{kind::no_source_position};
    std::string message{};
};

/// Resolve one parsed selector against the command source and the candidates.
///
/// Returns the indices of the matched candidates in the reference's order
/// (sorted by the requested order, then truncated to the limit). The caller
/// keeps the candidate list alive; indices are the only thing the command
/// module hands back.
[[nodiscard]] auto resolve_selector(const entity_selector &selector,
                                    const selector_request &request)
    -> std::expected<std::vector<std::size_t>, selector_error>;

} // namespace refrain::command
