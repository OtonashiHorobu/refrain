// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Java `Climate`: quantized parameter intervals, parameter points and the
// R-tree that selects the nearest point for a sampled target. Biome selection
// is the first consumer; the search order is observable because equal-distance
// points are resolved by first encounter.

#pragma once

#include <refrain/worldgen/java_compat.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace refrain::worldgen::climate {

/// Quantized coordinate: `Climate.quantizeCoord` multiplies by 10000 and
/// truncates toward zero.
using coord = jlong;

[[nodiscard]] auto quantize(float value) noexcept -> coord;
[[nodiscard]] auto unquantize(coord value) noexcept -> float;

/// Java `Climate.Parameter`: an inclusive interval in quantized space.
struct parameter {
    coord min{0};
    coord max{0};

    [[nodiscard]] auto distance(coord target) const noexcept -> coord;
    [[nodiscard]] auto span(const parameter &other) const noexcept -> parameter;
    [[nodiscard]] friend auto operator==(const parameter &, const parameter &)
        -> bool = default;
};

/// `Climate.Parameter.point` / `Climate.Parameter.span` (quantized).
[[nodiscard]] auto point(coord value) noexcept -> parameter;
[[nodiscard]] auto span(coord min, coord max) noexcept -> parameter;

/// `Climate.TargetPoint`: the six quantized climate samples of one position.
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

/// `Climate.ParameterPoint`.
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

/// `Climate.ParameterList<T>` restricted to an integer payload (a biome id or
/// index). Builds the reference R-tree (19 children per node) and reproduces
/// its nearest-point search, including the last-result warm start.
class parameter_list {
  public:
    struct entry {
        parameter_point parameters{};
        std::uint16_t value{0};
    };

    explicit parameter_list(std::vector<entry> values);

    /// `Climate.ParameterList.findValue`; mutable because the R-tree keeps the
    /// previous result as the next search's starting candidate.
    [[nodiscard]] auto find(const target_point &target) -> std::uint16_t;

    /// `Climate.ParameterList.findValueBruteForce`, in insertion order.
    [[nodiscard]] auto find_brute_force(const target_point &target) const
        -> std::uint16_t;

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
        std::vector<std::size_t> children{};
    };

    [[nodiscard]] auto make_leaf(const entry &value) -> std::size_t;
    [[nodiscard]] auto make_subtree(std::vector<std::size_t> children)
        -> std::size_t;
    [[nodiscard]] auto build(std::vector<std::size_t> children) -> std::size_t;
    [[nodiscard]] auto search(std::size_t index,
                              const std::array<coord, 7> &target,
                              std::size_t candidate) const -> std::size_t;

    [[nodiscard]] auto node_distance(std::size_t index,
                                     const std::array<coord, 7> &target) const
        -> coord;
    [[nodiscard]] auto space_of(const std::vector<std::size_t> &children) const
        -> std::array<parameter, 7>;

    std::vector<node> nodes_;
    std::vector<entry> values_;
    std::size_t root_{npos};
    std::size_t last_result_{npos};
};

} // namespace refrain::worldgen::climate
