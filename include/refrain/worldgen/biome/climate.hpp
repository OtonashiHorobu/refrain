// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Quantized climate parameters: intervals, parameter points and the R-tree
// that selects the nearest point for a sampled target. Biome selection
// is the first consumer; the search order is observable because equal-distance
// points are resolved by first encounter.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace refrain::worldgen::climate {

/// Quantized coordinate: multiplies by 10000 and truncates toward zero.
using coord = jlong;

[[nodiscard]] auto quantize(float value) noexcept -> coord;
[[nodiscard]] auto unquantize(coord value) noexcept -> float;

/// An inclusive interval in quantized space.
struct parameter {
    coord min{0};
    coord max{0};

    [[nodiscard]] auto distance(coord target) const noexcept -> coord;
    [[nodiscard]] auto span(const parameter &other) const noexcept -> parameter;
    [[nodiscard]] friend auto operator==(const parameter &, const parameter &)
        -> bool = default;
};

/// Builds a point interval or the span of two intervals (quantized).
[[nodiscard]] auto point(coord value) noexcept -> parameter;
[[nodiscard]] auto span(coord min, coord max) noexcept -> parameter;

/// The six quantized climate samples of one position.
struct target_point {
    coord temperature{0};
    coord humidity{0};
    coord continentalness{0};
    coord erosion{0};
    coord depth{0};
    coord weirdness{0};

    /// The search vector: six coordinates plus a zero offset dimension.
    [[nodiscard]] auto to_array() const noexcept -> std::array<coord, 7>;
};

/// One point in the seven-dimensional climate parameter space.
struct parameter_point {
    parameter temperature{};
    parameter humidity{};
    parameter continentalness{};
    parameter erosion{};
    parameter depth{};
    parameter weirdness{};
    coord offset{0};

    [[nodiscard]] auto fitness(const target_point &target) const noexcept
        -> coord;

    /// The seven search dimensions: the six intervals plus `(offset, offset)`.
    [[nodiscard]] auto space() const noexcept -> std::array<parameter, 7>;
};

#if defined(REFRAIN_CLIMATE_TIE_STATS)
/// Tie accounting for the warm-started nearest-point search.
///
/// The search keeps a child whose box distance is strictly below the current
/// best and keeps a leaf only when it is strictly closer. A candidate at
/// exactly the current best distance therefore loses, so which of two
/// equidistant leaves wins depends on the traversal order, which the previous
/// result decides. The counters below measure how often that happens; the
/// capture records one concrete example. Everything here is compiled only when
/// `REFRAIN_CLIMATE_TIE_STATS` is defined, so the shipping path carries no
/// counting code.
namespace measurement {

/// Coarse counters over every `select` call of the process.
struct counters {
    /// Calls to `select`.
    std::uint64_t selects{0};
    /// Calls that skipped at least one child whose box distance equalled the
    /// current best.
    std::uint64_t selects_with_equal_child{0};
    /// Child boxes skipped across those calls.
    std::uint64_t equal_child_prunes{0};
    /// Calls that skipped at least one equal child whose subtree does not
    /// contain the incumbent. Only those can hide a different equidistant
    /// leaf; an equal child that contains the incumbent cannot change the
    /// answer, because the incumbent is already the best leaf in it.
    std::uint64_t selects_with_rival_equal_child{0};
    /// Rival equal children skipped across those calls.
    std::uint64_t rival_equal_child_prunes{0};
    /// Calls whose traversal rejected at least one leaf at exactly the current
    /// best distance.
    std::uint64_t selects_with_equal_leaf{0};
    /// Equidistant leaves rejected across those calls.
    std::uint64_t equal_leaf_rejections{0};
    /// Calls whose result differs from a search started from a fresh root.
    /// Both results are equidistant; only their identity differs.
    std::uint64_t hint_dependent_selects{0};
    /// Hint-dependent calls whose two leaves carry different biome ids, so the
    /// tie changes the biome the chunk records rather than only the entry.
    std::uint64_t hint_dependent_biome_changes{0};
    /// Calls where the root search found a leaf at a different distance. Both
    /// searches return the nearest leaf, so this must stay zero; a non-zero
    /// count means the pruning was unsound rather than a tie.
    std::uint64_t unsound_selects{0};
};

enum class capture_kind : std::uint8_t {
    /// The warmed search returned a different equidistant leaf than a root
    /// search would have.
    hint_dependent,
    /// The warmed search passed over a distinct equidistant leaf.
    equal_leaf,
};

/// One concrete tie: the target and the two equidistant parameter points.
struct tie_capture {
    capture_kind kind{capture_kind::hint_dependent};
    std::array<coord, 7> target{};
    /// The leaf the warmed search returned, or the incumbent at the rejection.
    parameter_point incumbent{};
    /// The equidistant leaf it displaced (hint-dependent) or rejected.
    parameter_point rival{};
    std::uint16_t incumbent_value{0};
    std::uint16_t rival_value{0};
    std::size_t incumbent_entry{0};
    std::size_t rival_entry{0};
    /// The distance of the incumbent, and of the rival. A hint-dependent
    /// capture must have the two equal; a difference would mean the pruning
    /// was unsound rather than a tie.
    coord distance{0};
    coord rival_distance{0};
    /// One-based ordinal of the `select` call that produced this tie.
    std::uint64_t select_ordinal{0};
};

/// Process-wide counters; atomically updated, safe across workers.
[[nodiscard]] auto totals() noexcept -> counters;

/// A hint-dependent capture is the strongest evidence: two equidistant leaves
/// and the target show which one the warm start picked. Such captures are rare
/// and get the larger buffer.
inline constexpr std::size_t max_hint_captures = 8;
/// An equidistant leaf rejection that did not change the answer.
inline constexpr std::size_t max_leaf_captures = 4;

/// Captures collected since the last `reset_captures`, in the order seen.
[[nodiscard]] auto hint_captures() noexcept -> const tie_capture *;
[[nodiscard]] auto hint_capture_count() noexcept -> std::size_t;
[[nodiscard]] auto leaf_captures() noexcept -> const tie_capture *;
[[nodiscard]] auto leaf_capture_count() noexcept -> std::size_t;
void reset_captures() noexcept;

} // namespace measurement
#endif

/// Nearest-point selection over an integer payload (a biome id or index).
/// Builds the reference R-tree (19 children per node) and reproduces its
/// nearest-point search, including the last-result warm start.
class parameter_list {
  public:
    struct entry {
        parameter_point parameters{};
        std::uint16_t value{0};
    };

    explicit parameter_list(std::vector<entry> values);

    /// Nearest-point search; mutable because the R-tree keeps the previous
    /// result as the next search's starting candidate.
    [[nodiscard]] auto find(const target_point &target) -> std::uint16_t;

    /// Brute-force nearest-point search, in insertion order.
    [[nodiscard]] auto find_brute_force(const target_point &target) const
        -> std::uint16_t;

    /// The index in `entries()` of the selected point. The R-tree search is
    /// shared with `find`; values may repeat, indices do not.
    [[nodiscard]] auto find_index(const target_point &target) -> std::size_t;

    /// Forgets the previous result so the next search starts from the root.
    void reset_search() noexcept { last_result_ = npos; }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return values_.size();
    }
    [[nodiscard]] auto entries() const noexcept -> const std::vector<entry> & {
        return values_;
    }

  private:
    static constexpr std::size_t children_per_node = 19;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    struct node {
        std::array<parameter, 7> space{};
        std::uint16_t value{0};
        std::size_t entry_index{npos};
        std::vector<std::size_t> children{};
    };

    [[nodiscard]] auto make_leaf(const entry &value, std::size_t index)
        -> std::size_t;
    [[nodiscard]] auto make_subtree(std::vector<std::size_t> children)
        -> std::size_t;
    [[nodiscard]] auto build(std::vector<std::size_t> children) -> std::size_t;
    [[nodiscard]] auto search(std::size_t index,
                              const std::array<coord, 7> &target,
                              std::size_t candidate) const -> std::size_t;
#if defined(REFRAIN_CLIMATE_TIE_STATS)
    /// Whether `leaf_node` is in the subtree rooted at `ancestor`.
    [[nodiscard]] auto contains(std::size_t ancestor,
                                std::size_t leaf_node) const -> bool;
#endif

    [[nodiscard]] auto select(const target_point &target) -> std::size_t;

    [[nodiscard]] auto node_distance(std::size_t index,
                                     const std::array<coord, 7> &target) const
        -> coord;
    [[nodiscard]] auto space_of(const std::vector<std::size_t> &children) const
        -> std::array<parameter, 7>;

    std::vector<node> nodes_;
    std::vector<entry> values_;
    std::size_t root_{npos};
    std::size_t last_result_{npos};
#if defined(REFRAIN_CLIMATE_TIE_STATS)
    /// Parent of every node, filled once the tree is built. Only the tie
    /// measurement needs it, to tell the incumbent's own branch from a rival.
    std::vector<std::size_t> parents_{};
#endif
};

} // namespace refrain::worldgen::climate
