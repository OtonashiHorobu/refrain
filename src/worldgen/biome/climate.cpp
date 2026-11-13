// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/climate.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#if defined(REFRAIN_CLIMATE_TIE_STATS)
#include <atomic>
#endif

namespace refrain::worldgen::climate {

namespace {

auto square(coord value) noexcept -> coord {
    // The reference squares a long with wrap-around; unsigned arithmetic keeps
    // the multiplication well-defined before converting back.
    return static_cast<coord>(static_cast<std::uint64_t>(value) *
                              static_cast<std::uint64_t>(value));
}

auto magnitude(coord value) noexcept -> coord {
    // Java's Math.abs(long) keeps the same bits for LONG_MIN.
    return value < 0
               ? static_cast<coord>(0ULL - static_cast<std::uint64_t>(value))
               : value;
}

auto center(const parameter &value) noexcept -> coord {
    // The reference adds two longs with wrap-around, then divides toward zero.
    const auto sum = static_cast<coord>(static_cast<std::uint64_t>(value.min) +
                                        static_cast<std::uint64_t>(value.max));
    return sum / 2;
}

auto wrapped_distance(coord minimum, coord maximum) noexcept -> coord {
    // The reference subtracts with wrap-around before taking the magnitude.
    return static_cast<coord>(static_cast<std::uint64_t>(maximum) -
                              static_cast<std::uint64_t>(minimum));
}

} // namespace

#if defined(REFRAIN_CLIMATE_TIE_STATS)
namespace {

// Counting state of the current `select`. `search` is recursive and runs on
// the calling thread, so plain thread-local state carries the per-call flags
// and event counts out of the recursion without widening its signature.
thread_local bool counting = false;
thread_local std::uint64_t equal_child_events = 0;
thread_local std::uint64_t rival_child_events = 0;
thread_local std::uint64_t equal_leaf_events = 0;
thread_local bool first_leaf_seen = false;
thread_local std::size_t first_leaf_incumbent = 0;
thread_local std::size_t first_leaf_rival = 0;

std::atomic<std::uint64_t> select_count{0};
std::atomic<std::uint64_t> select_equal_child_count{0};
std::atomic<std::uint64_t> child_prune_count{0};
std::atomic<std::uint64_t> select_rival_child_count{0};
std::atomic<std::uint64_t> rival_child_prune_count{0};
std::atomic<std::uint64_t> select_equal_leaf_count{0};
std::atomic<std::uint64_t> leaf_rejection_count{0};
std::atomic<std::uint64_t> hint_dependent_count{0};
std::atomic<std::uint64_t> hint_dependent_biome_change_count{0};
std::atomic<std::uint64_t> unsound_count{0};

std::atomic<std::size_t> hint_slots{0};
std::atomic<std::size_t> leaf_slots{0};
measurement::tie_capture hint_buffer[measurement::max_hint_captures];
measurement::tie_capture leaf_buffer[measurement::max_leaf_captures];

void store_hint(const measurement::tie_capture &capture) {
    const auto slot = hint_slots.fetch_add(1, std::memory_order_relaxed);
    if (slot < measurement::max_hint_captures) {
        hint_buffer[slot] = capture;
    }
}

void store_leaf(const measurement::tie_capture &capture) {
    const auto slot = leaf_slots.fetch_add(1, std::memory_order_relaxed);
    if (slot < measurement::max_leaf_captures) {
        leaf_buffer[slot] = capture;
    }
}

} // namespace

auto measurement::totals() noexcept -> measurement::counters {
    counters result;
    result.selects = select_count.load(std::memory_order_relaxed);
    result.selects_with_equal_child =
        select_equal_child_count.load(std::memory_order_relaxed);
    result.equal_child_prunes =
        child_prune_count.load(std::memory_order_relaxed);
    result.selects_with_rival_equal_child =
        select_rival_child_count.load(std::memory_order_relaxed);
    result.rival_equal_child_prunes =
        rival_child_prune_count.load(std::memory_order_relaxed);
    result.selects_with_equal_leaf =
        select_equal_leaf_count.load(std::memory_order_relaxed);
    result.equal_leaf_rejections =
        leaf_rejection_count.load(std::memory_order_relaxed);
    result.hint_dependent_selects =
        hint_dependent_count.load(std::memory_order_relaxed);
    result.hint_dependent_biome_changes =
        hint_dependent_biome_change_count.load(std::memory_order_relaxed);
    result.unsound_selects = unsound_count.load(std::memory_order_relaxed);
    return result;
}

auto measurement::hint_captures() noexcept -> const measurement::tie_capture * {
    return hint_buffer;
}

auto measurement::hint_capture_count() noexcept -> std::size_t {
    return std::min(hint_slots.load(std::memory_order_relaxed),
                    max_hint_captures);
}

auto measurement::leaf_captures() noexcept -> const measurement::tie_capture * {
    return leaf_buffer;
}

auto measurement::leaf_capture_count() noexcept -> std::size_t {
    return std::min(leaf_slots.load(std::memory_order_relaxed),
                    max_leaf_captures);
}

void measurement::reset_captures() noexcept {
    hint_slots.store(0, std::memory_order_relaxed);
    leaf_slots.store(0, std::memory_order_relaxed);
}
#endif

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
#if defined(REFRAIN_CLIMATE_TIE_STATS)
    parents_.assign(nodes_.size(), npos);
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        for (const auto child : nodes_[index].children) {
            parents_[child] = index;
        }
    }
#endif
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
                             std::uint64_t left_magnitude = 0;
                             std::uint64_t right_magnitude = 0;
                             for (std::size_t d = 0; d < 7; ++d) {
                                 left_magnitude += static_cast<std::uint64_t>(
                                     magnitude(center(nodes_[left].space[d])));
                                 right_magnitude += static_cast<std::uint64_t>(
                                     magnitude(center(nodes_[right].space[d])));
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
                total_cost +=
                    static_cast<std::uint64_t>(magnitude(wrapped_distance(
                        dimension_space.min, dimension_space.max)));
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

#if defined(REFRAIN_CLIMATE_TIE_STATS)
auto parameter_list::contains(std::size_t ancestor, std::size_t leaf_node) const
    -> bool {
    for (std::size_t cursor = leaf_node; cursor != npos;
         cursor = parents_[cursor]) {
        if (cursor == ancestor) {
            return true;
        }
    }
    return false;
}
#endif

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
#if defined(REFRAIN_CLIMATE_TIE_STATS)
        // The strict comparison below skips this child exactly when its lower
        // bound equals the current best: an equidistant subtree is pruned.
        if (counting && closest != npos && child_distance == min_distance) {
            ++equal_child_events;
            if (!contains(child, closest)) {
                ++rival_child_events;
            }
        }
#endif
        if (min_distance > child_distance) {
            const std::size_t leaf = search(child, target, closest);
            const coord leaf_distance =
                child == leaf ? child_distance : node_distance(leaf, target);
#if defined(REFRAIN_CLIMATE_TIE_STATS)
            // A distinct leaf at exactly the current best does not replace the
            // incumbent; the earlier one keeps the tie.
            if (counting && leaf != closest && leaf_distance == min_distance) {
                ++equal_leaf_events;
                if (!first_leaf_seen) {
                    first_leaf_seen = true;
                    first_leaf_incumbent = closest;
                    first_leaf_rival = leaf;
                }
            }
#endif
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
#if defined(REFRAIN_CLIMATE_TIE_STATS)
    const std::uint64_t ordinal =
        select_count.fetch_add(1, std::memory_order_relaxed) + 1;
    equal_child_events = 0;
    rival_child_events = 0;
    equal_leaf_events = 0;
    first_leaf_seen = false;
    counting = true;
    const std::size_t leaf = search(root_, array, last_result_);
    counting = false;

    const auto to_point = [this](std::size_t node_index) {
        const auto &space = nodes_[node_index].space;
        return parameter_point{.temperature = space[0],
                               .humidity = space[1],
                               .continentalness = space[2],
                               .erosion = space[3],
                               .depth = space[4],
                               .weirdness = space[5],
                               .offset = space[6].min};
    };
    const auto capture_for = [&](std::size_t incumbent_node,
                                 std::size_t rival_node) {
        measurement::tie_capture capture;
        capture.target = array;
        capture.incumbent = to_point(incumbent_node);
        capture.rival = to_point(rival_node);
        capture.incumbent_value = nodes_[incumbent_node].value;
        capture.rival_value = nodes_[rival_node].value;
        capture.incumbent_entry = nodes_[incumbent_node].entry_index;
        capture.rival_entry = nodes_[rival_node].entry_index;
        capture.distance = node_distance(incumbent_node, array);
        capture.rival_distance = node_distance(rival_node, array);
        capture.select_ordinal = ordinal;
        return capture;
    };

    if (equal_child_events != 0) {
        select_equal_child_count.fetch_add(1, std::memory_order_relaxed);
        child_prune_count.fetch_add(equal_child_events,
                                    std::memory_order_relaxed);
    }
    if (rival_child_events != 0) {
        select_rival_child_count.fetch_add(1, std::memory_order_relaxed);
        rival_child_prune_count.fetch_add(rival_child_events,
                                          std::memory_order_relaxed);
    }
    if (equal_leaf_events != 0) {
        select_equal_leaf_count.fetch_add(1, std::memory_order_relaxed);
        leaf_rejection_count.fetch_add(equal_leaf_events,
                                       std::memory_order_relaxed);
        if (first_leaf_incumbent != npos && first_leaf_rival != npos) {
            auto capture = capture_for(first_leaf_incumbent, first_leaf_rival);
            capture.kind = measurement::capture_kind::equal_leaf;
            store_leaf(capture);
        }
    }
    // Without a tie event every pruned subtree was strictly farther than the
    // incumbent, so the answer cannot depend on the starting candidate.
    if ((equal_child_events != 0 || equal_leaf_events != 0) && leaf != npos) {
        const std::size_t fresh = search(root_, array, npos);
        if (fresh != npos && fresh != leaf) {
            if (node_distance(fresh, array) != node_distance(leaf, array)) {
                unsound_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                hint_dependent_count.fetch_add(1, std::memory_order_relaxed);
                if (nodes_[fresh].value != nodes_[leaf].value) {
                    hint_dependent_biome_change_count.fetch_add(
                        1, std::memory_order_relaxed);
                }
                auto capture = capture_for(leaf, fresh);
                capture.kind = measurement::capture_kind::hint_dependent;
                store_hint(capture);
            }
        }
    }
    last_result_ = leaf;
    return leaf;
#else
    const std::size_t leaf = search(root_, array, last_result_);
    last_result_ = leaf;
    return leaf;
#endif
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
