// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/climate.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace refrain::worldgen::climate {

namespace {

auto square(coord value) noexcept -> coord {
    // The reference squares a long with wrap-around; unsigned arithmetic keeps
    // the multiplication well-defined before converting back.
    return static_cast<coord>(static_cast<std::uint64_t>(value) *
                              static_cast<std::uint64_t>(value));
}

auto center(const parameter &value) noexcept -> coord {
    // Java long division truncates toward zero, like C++.
    return (value.min + value.max) / 2;
}

} // namespace

auto quantize(float value) noexcept -> coord {
    // Java's float-to-long conversion saturates and maps NaN to zero; C++'s is
    // undefined outside the representable range.
    const float scaled = value * 10000.0F;
    if (std::isnan(scaled)) {
        return 0;
    }
    constexpr float limit = 9223372036854775808.0F; // 2^63
    if (scaled >= limit) {
        return std::numeric_limits<coord>::max();
    }
    if (scaled <= -limit) {
        return std::numeric_limits<coord>::min();
    }
    return static_cast<coord>(scaled);
}

auto unquantize(coord value) noexcept -> float {
    return static_cast<float>(value) / 10000.0F;
}

auto parameter::distance(coord target) const noexcept -> coord {
    const coord above = target - max;
    const coord below = min - target;
    return above > 0 ? above : std::max(below, coord{0});
}

auto parameter::span(const parameter &other) const noexcept -> parameter {
    return parameter{std::min(min, other.min), std::max(max, other.max)};
}

auto point(coord value) noexcept -> parameter {
    return parameter{value, value};
}

auto span(coord min, coord max) noexcept -> parameter {
    return parameter{min, max};
}

auto target_point::to_array() const noexcept -> std::array<coord, 7> {
    return {temperature, humidity, continentalness, erosion, depth,
            weirdness,   0};
}

auto parameter_point::fitness(const target_point &target) const noexcept
    -> coord {
    std::uint64_t total = 0;
    total += static_cast<std::uint64_t>(
        square(temperature.distance(target.temperature)));
    total +=
        static_cast<std::uint64_t>(square(humidity.distance(target.humidity)));
    total += static_cast<std::uint64_t>(
        square(continentalness.distance(target.continentalness)));
    total +=
        static_cast<std::uint64_t>(square(erosion.distance(target.erosion)));
    total += static_cast<std::uint64_t>(square(depth.distance(target.depth)));
    total += static_cast<std::uint64_t>(
        square(weirdness.distance(target.weirdness)));
    total += static_cast<std::uint64_t>(square(offset));
    return static_cast<coord>(total);
}

auto parameter_point::space() const noexcept -> std::array<parameter, 7> {
    return {temperature, humidity,  continentalness, erosion,
            depth,       weirdness, point(offset)};
}

parameter_list::parameter_list(std::vector<entry> values)
    : values_{std::move(values)} {
    nodes_.reserve(values_.size() * 2);
    std::vector<std::size_t> children;
    children.reserve(values_.size());
    for (std::size_t i = 0; i < values_.size(); ++i) {
        children.push_back(make_leaf(values_[i], i));
    }
    root_ = build(std::move(children));
}

auto parameter_list::make_leaf(const entry &value, std::size_t index)
    -> std::size_t {
    node leaf;
    leaf.space = value.parameters.space();
    leaf.value = value.value;
    leaf.entry_index = index;
    nodes_.push_back(std::move(leaf));
    return nodes_.size() - 1;
}

auto parameter_list::make_subtree(std::vector<std::size_t> children)
    -> std::size_t {
    node subtree;
    subtree.space = space_of(children);
    subtree.children = std::move(children);
    nodes_.push_back(std::move(subtree));
    return nodes_.size() - 1;
}

auto parameter_list::space_of(const std::vector<std::size_t> &children) const
    -> std::array<parameter, 7> {
    std::array<parameter, 7> space = nodes_[children.front()].space;
    for (std::size_t i = 1; i < children.size(); ++i) {
        const auto &child = nodes_[children[i]].space;
        for (std::size_t d = 0; d < space.size(); ++d) {
            space[d] = space[d].span(child[d]);
        }
    }
    return space;
}

auto parameter_list::build(std::vector<std::size_t> children) -> std::size_t {
    if (children.size() == 1) {
        return children.front();
    }
    if (children.size() <= children_per_node) {
        std::stable_sort(children.begin(), children.end(),
                         [this](std::size_t left, std::size_t right) {
                             coord left_magnitude = 0;
                             coord right_magnitude = 0;
                             for (std::size_t d = 0; d < 7; ++d) {
                                 left_magnitude +=
                                     std::abs(center(nodes_[left].space[d]));
                                 right_magnitude +=
                                     std::abs(center(nodes_[right].space[d]));
                             }
                             return left_magnitude < right_magnitude;
                         });
        return make_subtree(std::move(children));
    }

    std::uint64_t min_cost = std::numeric_limits<std::uint64_t>::max();
    std::size_t min_dimension = 0;
    std::vector<std::vector<std::size_t>> min_buckets;
    for (std::size_t dimension = 0; dimension < 7; ++dimension) {
        std::stable_sort(
            children.begin(), children.end(),
            [this, dimension](std::size_t left, std::size_t right) {
                for (std::size_t offset = 0; offset < 7; ++offset) {
                    const std::size_t d = (dimension + offset) % 7;
                    const coord left_center = center(nodes_[left].space[d]);
                    const coord right_center = center(nodes_[right].space[d]);
                    if (left_center < right_center) {
                        return true;
                    }
                    if (right_center < left_center) {
                        return false;
                    }
                }
                return false;
            });

        std::size_t expected = 1;
        while (expected * children_per_node < children.size()) {
            expected *= children_per_node;
        }
        std::vector<std::vector<std::size_t>> buckets;
        std::vector<std::size_t> current;
        for (const auto child : children) {
            current.push_back(child);
            if (current.size() >= expected) {
                buckets.push_back(std::move(current));
                current.clear();
            }
        }
        if (!current.empty()) {
            buckets.push_back(std::move(current));
        }

        std::uint64_t total_cost = 0;
        for (const auto &bucket : buckets) {
            for (const auto &dimension_space : space_of(bucket)) {
                total_cost += static_cast<std::uint64_t>(
                    std::abs(dimension_space.max - dimension_space.min));
            }
        }
        if (min_cost > total_cost) {
            min_cost = total_cost;
            min_dimension = dimension;
            min_buckets = std::move(buckets);
        }
    }

    std::stable_sort(
        min_buckets.begin(), min_buckets.end(),
        [this, min_dimension](const auto &left, const auto &right) {
            const auto left_space = space_of(left);
            const auto right_space = space_of(right);
            for (std::size_t offset = 0; offset < 7; ++offset) {
                const std::size_t d = (min_dimension + offset) % 7;
                const coord left_center = std::abs(center(left_space[d]));
                const coord right_center = std::abs(center(right_space[d]));
                if (left_center < right_center) {
                    return true;
                }
                if (right_center < left_center) {
                    return false;
                }
            }
            return false;
        });

    std::vector<std::size_t> rebuilt;
    rebuilt.reserve(min_buckets.size());
    for (auto &bucket : min_buckets) {
        rebuilt.push_back(build(std::move(bucket)));
    }
    return make_subtree(std::move(rebuilt));
}

auto parameter_list::node_distance(std::size_t index,
                                   const std::array<coord, 7> &target) const
    -> coord {
    const auto &space = nodes_[index].space;
    std::uint64_t total = 0;
    for (std::size_t d = 0; d < space.size(); ++d) {
        total +=
            static_cast<std::uint64_t>(square(space[d].distance(target[d])));
    }
    return static_cast<coord>(total);
}

auto parameter_list::search(std::size_t index,
                            const std::array<coord, 7> &target,
                            std::size_t candidate) const -> std::size_t {
    const auto &current = nodes_[index];
    if (current.children.empty()) {
        return index;
    }
    coord min_distance = candidate == npos ? std::numeric_limits<coord>::max()
                                           : node_distance(candidate, target);
    std::size_t closest = candidate;
    for (const auto child : current.children) {
        const coord child_distance = node_distance(child, target);
        if (min_distance > child_distance) {
            const std::size_t leaf = search(child, target, closest);
            const coord leaf_distance =
                child == leaf ? child_distance : node_distance(leaf, target);
            if (min_distance > leaf_distance) {
                min_distance = leaf_distance;
                closest = leaf;
            }
        }
    }
    return closest;
}

auto parameter_list::select(const target_point &target) -> std::size_t {
    const auto array = target.to_array();
    const std::size_t leaf = search(root_, array, last_result_);
    last_result_ = leaf;
    return leaf;
}

auto parameter_list::find(const target_point &target) -> std::uint16_t {
    return nodes_[select(target)].value;
}

auto parameter_list::find_index(const target_point &target) -> std::size_t {
    return nodes_[select(target)].entry_index;
}

auto parameter_list::find_brute_force(const target_point &target) const
    -> std::uint16_t {
    auto best = values_.front();
    coord best_fitness = best.parameters.fitness(target);
    for (std::size_t i = 1; i < values_.size(); ++i) {
        const coord fitness = values_[i].parameters.fitness(target);
        if (fitness < best_fitness) {
            best_fitness = fitness;
            best = values_[i];
        }
    }
    return best.value;
}

} // namespace refrain::worldgen::climate
