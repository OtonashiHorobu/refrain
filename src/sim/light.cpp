// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/light.hpp>

#include <refrain/sim/world.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::sim {
namespace {

constexpr std::int32_t section_edge = 16;
constexpr std::int32_t section_columns = 256; // 16 × 16
constexpr std::int32_t min_section_y = -64;
constexpr std::int32_t max_section_y = 63;
constexpr std::int32_t section_span = max_section_y - min_section_y + 1;
constexpr std::int32_t section_index_offset = -min_section_y;
constexpr std::size_t light_bytes = 2048;
constexpr std::size_t layer_cells = 4096;
constexpr std::size_t layer_count = 2;

struct direction {
    std::int32_t dx;
    std::int32_t dy;
    std::int32_t dz;
};

constexpr std::array<direction, 6> directions{{
    {.dx = 1, .dy = 0, .dz = 0},
    {.dx = -1, .dy = 0, .dz = 0},
    {.dx = 0, .dy = 1, .dz = 0},
    {.dx = 0, .dy = -1, .dz = 0},
    {.dx = 0, .dy = 0, .dz = 1},
    {.dx = 0, .dy = 0, .dz = -1},
}};

[[nodiscard]] constexpr auto floor_div(std::int32_t value,
                                       std::int32_t divisor) noexcept
    -> std::int32_t {
    const auto quotient{value / divisor};
    return value % divisor != 0 && (value < 0) != (divisor < 0) ? quotient - 1
                                                                : quotient;
}

[[nodiscard]] constexpr auto section_coord(std::int32_t block) noexcept
    -> std::int32_t {
    return floor_div(block, section_edge);
}

[[nodiscard]] constexpr auto local_coord(std::int32_t block) noexcept
    -> std::int32_t {
    return block - section_coord(block) * section_edge;
}

[[nodiscard]] constexpr auto local_index(std::int32_t y, std::int32_t z,
                                         std::int32_t x) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(((y & 15) << 8) | ((z & 15) << 4) |
                                    (x & 15));
}

[[nodiscard]] auto nibble_get(std::span<const std::byte> data,
                              std::size_t index) noexcept -> std::uint8_t {
    const auto packed = static_cast<std::uint8_t>(data[index >> 1U]);
    return static_cast<std::uint8_t>((packed >> ((index & 1U) * 4U)) & 0x0FU);
}

void nibble_set(std::vector<std::byte> &data, std::size_t index,
                std::uint8_t value) noexcept {
    const auto position = index >> 1U;
    const auto shift = (index & 1U) * 4U;
    const auto packed = static_cast<std::uint8_t>(data[position]);
    data[position] = static_cast<std::byte>((packed & ~(0x0FU << shift)) |
                                            ((value & 0x0FU) << shift));
}

/// Directions of the propagation loop below, in the face order of the
/// content table: +x (east), -x (west), +y (up), -y (down), +z (south),
/// -z (north).
constexpr std::array<light_face, 6> step_face{{
    light_face::east,
    light_face::west,
    light_face::up,
    light_face::down,
    light_face::south,
    light_face::north,
}};

[[nodiscard]] constexpr auto opposite_face(light_face face) noexcept
    -> light_face {
    return static_cast<light_face>(static_cast<std::uint8_t>(face) ^ 1U);
}

/// The four horizontal side offsets of a cell, in the order west, east, north,
/// south.
constexpr std::array<std::pair<std::int32_t, std::int32_t>, 4> side_offsets{{
    {-1, 0},
    {1, 0},
    {0, -1},
    {0, 1},
}};

/// Direction of the step that enters a cell from the matching `side_offsets`
/// neighbour: the west neighbour steps east (+x, face 0), the east neighbour
/// steps west (face 1), the north neighbour steps south (+z, face 4) and the
/// south neighbour steps north (face 5).
constexpr std::array<std::size_t, 4> side_directions{{0, 1, 4, 5}};

/// Bounds indices of the two axes perpendicular to one face: (u, v) into
/// `{min_x, min_y, min_z, max_x, max_y, max_z}`.
constexpr std::array<std::pair<std::size_t, std::size_t>, 6> face_axes{{
    {0, 2}, // down:  u = x, v = z
    {0, 2}, // up:    u = x, v = z
    {0, 1}, // north: u = x, v = y
    {0, 1}, // south: u = x, v = y
    {1, 2}, // west:  u = y, v = z
    {1, 2}, // east:  u = y, v = z
}};

/// The content table bounds one face at `max_light_boxes_per_face` boxes.
constexpr std::size_t max_face_boxes = 16;

/// One face box projected onto the plane perpendicular to its face axis and
/// clipped to the block cell.
struct face_rect {
    double u0{0.0};
    double u1{0.0};
    double v0{0.0};
    double v1{0.0};
};

/// Append the projected, clipped rectangles of one face shape. Returns false
/// when a hand-built shape exceeds the content bound; the caller treats that
/// as occluding instead of silently misjudging it.
[[nodiscard]] auto
append_face_rects(const block_light_shape *shape, light_face face,
                  std::array<face_rect, 2 * max_face_boxes> &rects,
                  std::size_t &count) -> bool {
    if (shape == nullptr) {
        return true;
    }
    const auto &boxes{shape->faces[static_cast<std::size_t>(face)]};
    const auto [u_axis, v_axis]{face_axes[static_cast<std::size_t>(face)]};
    for (const auto &box : boxes) {
        face_rect rect{};
        rect.u0 = std::clamp(box.bounds[u_axis], 0.0, 1.0);
        rect.u1 = std::clamp(box.bounds[u_axis + 3], 0.0, 1.0);
        rect.v0 = std::clamp(box.bounds[v_axis], 0.0, 1.0);
        rect.v1 = std::clamp(box.bounds[v_axis + 3], 0.0, 1.0);
        if (rect.u1 <= rect.u0 || rect.v1 <= rect.v0) {
            continue;
        }
        if (count == rects.size()) {
            return false;
        }
        rects[count++] = rect;
    }
    return true;
}

/// True when the union of the rectangles covers the whole unit square. The
/// reference lighting engine asks the same question of the joined face shapes.
[[nodiscard]] auto covers_unit_square(const face_rect *rects, std::size_t count)
    -> bool {
    if (count == 0) {
        return false;
    }
    std::array<double, 2 + 2 * 2 * max_face_boxes> u_coords{};
    std::size_t u_count{0};
    u_coords[u_count++] = 0.0;
    u_coords[u_count++] = 1.0;
    const auto add_u = [&u_coords, &u_count](double value) {
        if (value <= 0.0 || value >= 1.0) {
            return;
        }
        for (std::size_t i{0}; i < u_count; ++i) {
            if (u_coords[i] == value) {
                return;
            }
        }
        u_coords[u_count++] = value;
    };
    for (std::size_t i{0}; i < count; ++i) {
        add_u(rects[i].u0);
        add_u(rects[i].u1);
    }
    std::ranges::sort(
        u_coords.begin(),
        std::next(u_coords.begin(), static_cast<std::ptrdiff_t>(u_count)));
    for (std::size_t slab{1}; slab < u_count; ++slab) {
        const auto u0{u_coords[slab - 1]};
        const auto u1{u_coords[slab]};
        std::array<std::pair<double, double>, 2 * max_face_boxes> spans{};
        std::size_t span_count{0};
        for (std::size_t i{0}; i < count; ++i) {
            if (rects[i].u0 <= u0 && rects[i].u1 >= u1) {
                spans[span_count++] = {rects[i].v0, rects[i].v1};
            }
        }
        if (span_count == 0) {
            return false;
        }
        std::ranges::sort(
            spans.begin(),
            std::next(spans.begin(), static_cast<std::ptrdiff_t>(span_count)));
        double covered{0.0};
        for (std::size_t i{0}; i < span_count; ++i) {
            if (spans[i].first > covered) {
                return false;
            }
            covered = std::max(covered, spans[i].second);
        }
        if (covered < 1.0) {
            return false;
        }
    }
    return true;
}

/// True when the union of two face shapes covers the whole block cell: the
/// reference light engine's shape-occlusion test for one step between two
/// cells, or for the edge between two vertically adjacent cells.
[[nodiscard]] auto face_occludes(const block_light_shape *from,
                                 const block_light_shape *to, light_face face)
    -> bool {
    if (from == nullptr && to == nullptr) {
        return false;
    }
    std::array<face_rect, 2 * max_face_boxes> rects{};
    std::size_t count{0};
    if (!append_face_rects(from, face, rects, count) ||
        !append_face_rects(to, opposite_face(face), rects, count)) {
        return true;
    }
    return covers_unit_square(rects.data(), count);
}

/// Section Ys of the light layers a column requires: one layer per block
/// section plus the padding layer below `y_pos` and above the top section.
/// Sections the column does not have - a vertical gap of a sparse record - keep
/// no layer, because they are air and carry no stored light.
[[nodiscard]] auto required_light_layers(const chunk &column)
    -> std::vector<std::int32_t> {
    std::vector<std::int32_t> layer_ys{};
    if (column.sections.empty()) {
        return layer_ys;
    }
    const auto top_section{static_cast<std::int32_t>(column.sections.back().y)};
    layer_ys.reserve(column.sections.size() + 2);
    layer_ys.push_back(column.y_pos - 1);
    for (const auto &section : column.sections) {
        layer_ys.push_back(section.y);
    }
    layer_ys.push_back(top_section + 1);
    return layer_ys;
}

/// One loaded column as the engine sees it.
struct column_state {
    chunk *record{nullptr};
    /// Highest block section Y plus one: the light layer range follows it.
    std::int32_t top_layer_y{0};
    std::uint16_t section_count{0};
    /// Light layers sorted by section Y. A column whose block sections have
    /// gaps keeps layers only where a block section or the padding layer
    /// exists; the gap sections are air and carry no stored light. The arrays
    /// are owned by the column's storage record and stay address-stable after
    /// `reset_layers()`.
    struct layer_ref {
        std::int8_t y{0};
        ::refrain::storage::light_section *layer{nullptr};
    };
    std::vector<layer_ref> layers{};
    std::array<std::int16_t, section_span> section_lookup{};
    /// Lowest sky light source cell per column: the Y of the first cell below
    /// the column's highest light damping block. `block_bottom()` means the
    /// whole column sees the sky.
    std::array<std::int32_t, section_columns> lowest_source{};

    [[nodiscard]] auto layer(std::int32_t section_y) noexcept
        -> ::refrain::storage::light_section * {
        const auto found{std::lower_bound(
            layers.begin(), layers.end(), section_y,
            [](const layer_ref &entry, std::int32_t value) {
                return static_cast<std::int32_t>(entry.y) < value;
            })};
        if (found == layers.end() ||
            static_cast<std::int32_t>(found->y) != section_y) {
            return nullptr;
        }
        return found->layer;
    }
};

[[nodiscard]] auto layer_array(::refrain::storage::light_section &layer,
                               light_layer which) -> std::vector<std::byte> & {
    return which == light_layer::sky ? layer.sky_light : layer.block_light;
}

[[nodiscard]] auto layer_index(light_layer which) noexcept -> std::size_t {
    return which == light_layer::sky ? 0 : 1;
}

struct node_pos {
    std::int32_t x{0};
    std::int16_t y{0};
    std::int32_t z{0};
};

struct increase_entry {
    node_pos node{};
    std::uint8_t level{0};
};

struct decrease_entry {
    node_pos node{};
    std::uint8_t level{0};
};

struct layer_queues {
    std::deque<decrease_entry> decrease{};
    std::deque<increase_entry> increase{};
};

/// Which entry point asked for a drain. The initial pass of a column runs its
/// own drain; `run()` is the incremental path a block edit and the tick
/// backstop use.
enum class drain_origin : bool {
    initial,
    incremental,
};

/// Every `peak_sample_interval` processed entries the drain records the queue
/// length as a peak sample. Sampling keeps the counter useful while a drain
/// grows and shrinks the queues, without a store on every enqueue.
constexpr std::size_t peak_sample_interval = 1024;

} // namespace

struct light_engine::impl {
    explicit impl(world &owner) : owner_{&owner} {}

    world *owner_{nullptr};
    std::map<chunk_pos, column_state> columns_{};

    /// Most recently used column, so propagation within one column does not
    /// repeat the map lookup for every cell.
    column_state *cached_{nullptr};
    std::int32_t cached_x_{0};
    std::int32_t cached_z_{0};

    std::deque<node_pos> check_queue_{};
    std::array<layer_queues, layer_count> queues_{};
    std::set<light_section_change> changes_{};
    /// Columns an edit made stale, in position order. A column leaves the set
    /// - and its storage record is marked correct again - only when a drain
    /// processed every queued update, so a partially drained edit keeps the
    /// saved flag conservative.
    std::set<chunk_pos> dirty_columns_{};
    std::optional<chunk_pos> suppressed_origin_{};
    std::unordered_map<block_state_id, block_light_property> properties_{};

    /// Observation counters. The engine is single-threaded, so these are plain
    /// integers and the hot path pays one clock read per drain, not per node.
    /// `pending_peak_` is mutable because a queue observation updates it
    /// through the const `pending_nodes()` accessor.
    std::uint64_t columns_lit_{0};
    std::uint64_t columns_trusted_{0};
    std::uint64_t initial_drain_calls_{0};
    std::uint64_t initial_nodes_{0};
    std::uint64_t initial_ns_{0};
    std::uint64_t drain_calls_{0};
    std::uint64_t drain_nodes_{0};
    std::uint64_t drain_ns_{0};
    std::uint64_t block_edits_{0};
    mutable std::uint64_t pending_peak_{0};

    [[nodiscard]] auto find_column(std::int32_t chunk_x, std::int32_t chunk_z)
        -> column_state * {
        if (cached_ != nullptr && cached_x_ == chunk_x &&
            cached_z_ == chunk_z) {
            return cached_;
        }
        const auto found{columns_.find(chunk_pos{.x = chunk_x, .z = chunk_z})};
        if (found == columns_.end()) {
            cached_ = nullptr;
            return nullptr;
        }
        cached_ = &found->second;
        cached_x_ = chunk_x;
        cached_z_ = chunk_z;
        return cached_;
    }

    void clear_cache() noexcept { cached_ = nullptr; }

    /// Light property of one runtime state. Unknown states fall back to the
    /// documented default of the table: opaque and emitting nothing.
    [[nodiscard]] auto property(block_state_id id) -> block_light_property {
        const auto found{properties_.find(id)};
        if (found != properties_.end()) {
            return found->second;
        }
        block_light_property property{};
        const auto &table{owner_->block_light()};
        if (!table.empty()) {
            if (const auto *entry{table.find(owner_->registry().key(id))};
                entry != nullptr) {
                property = *entry;
            }
        }
        properties_.emplace(id, property);
        return property;
    }

    /// True when the vertical edge under an open-air cell and `below` stops
    /// the sky source search: the cell below is not a sky source.
    [[nodiscard]] auto edge_occluded_under_air(block_state_id below) -> bool {
        const auto lower{property(below)};
        if (lower.dampening != 0) {
            return true;
        }
        return face_occludes(nullptr, lower.shape, light_face::down);
    }

    /// True when the vertical edge between `above` and `below` stops the sky
    /// source search: the cell below is not a sky source.
    [[nodiscard]] auto edge_occluded(block_state_id above, block_state_id below)
        -> bool {
        const auto lower{property(below)};
        if (lower.dampening != 0) {
            return true;
        }
        return face_occludes(property(above).shape, lower.shape,
                             light_face::down);
    }

    /// True when the step from `from` to `to` in `step_face[direction]` is
    /// blocked by the shapes of both states.
    [[nodiscard]] auto shape_occludes(block_state_id from, block_state_id to,
                                      std::size_t direction) -> bool {
        return face_occludes(property(from).shape, property(to).shape,
                             step_face[direction]);
    }

    [[nodiscard]] auto block_state_at(std::int32_t x, std::int32_t y,
                                      std::int32_t z) -> block_state_id {
        auto *column{find_column(section_coord(x), section_coord(z))};
        if (column == nullptr) {
            return air_block;
        }
        const auto &chunk{*column->record};
        if (y < block_bottom(*column) || y > block_top(*column)) {
            return air_block;
        }
        const auto lookup_y{section_coord(y) + section_index_offset};
        if (lookup_y < 0 || lookup_y >= section_span) {
            return air_block;
        }
        const auto section_index{
            column->section_lookup[static_cast<std::size_t>(lookup_y)]};
        if (section_index < 0) {
            return air_block;
        }
        const auto &section{
            chunk.sections[static_cast<std::size_t>(section_index)]};
        if (section.uniform) {
            return section.value;
        }
        return section.blocks[local_index(y, z, x)];
    }

    [[nodiscard]] auto opacity(std::int32_t x, std::int32_t y, std::int32_t z)
        -> std::uint8_t {
        const auto dampening{property(block_state_at(x, y, z)).dampening};
        return dampening == 0 ? 1 : dampening;
    }

    [[nodiscard]] auto emission(std::int32_t x, std::int32_t y, std::int32_t z)
        -> std::uint8_t {
        return property(block_state_at(x, y, z)).emission;
    }

    void note_change(const chunk_pos &pos, std::int32_t section_y) {
        if (suppressed_origin_ && *suppressed_origin_ == pos) {
            return;
        }
        changes_.insert(light_section_change{
            .pos = pos, .section_y = static_cast<std::int8_t>(section_y)});
    }

    /// Record that an edit made `pos`'s light stale: the storage record must
    /// not claim correctness until a drain finishes the incremental relight.
    void mark_light_dirty(const chunk_pos &pos) {
        if (auto *column{find_column(pos.x, pos.z)}; column != nullptr) {
            column->record->storage.light_correct = false;
        }
        dirty_columns_.insert(pos);
    }

    /// A drain that leaves nothing queued has applied every light change, so
    /// the columns an edit marked stale hold consistent arrays again and the
    /// storage records may claim correctness once more.
    void finish_drain() {
        if (queue_size() != 0) {
            return;
        }
        for (const auto &pos : dirty_columns_) {
            const auto found{columns_.find(pos)};
            if (found != columns_.end() && found->second.record != nullptr) {
                found->second.record->storage.light_correct = true;
            }
        }
        dirty_columns_.clear();
    }

    [[nodiscard]] auto level_at(const node_pos &node, light_layer which)
        -> std::uint8_t {
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return 0;
        }
        auto *layer{column->layer(section_coord(node.y))};
        if (layer == nullptr) {
            return 0;
        }
        const auto &array{layer_array(*layer, which)};
        if (array.size() != light_bytes) {
            return 0;
        }
        return nibble_get(array,
                          local_index(local_coord(node.y), local_coord(node.z),
                                      local_coord(node.x)));
    }

    void set_level(const node_pos &node, light_layer which,
                   std::uint8_t value) {
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return;
        }
        auto *layer{column->layer(section_coord(node.y))};
        if (layer == nullptr) {
            return;
        }
        auto &array{layer_array(*layer, which)};
        if (array.size() != light_bytes) {
            return;
        }
        const auto index{local_index(local_coord(node.y), local_coord(node.z),
                                     local_coord(node.x))};
        if (nibble_get(array, index) == value) {
            return;
        }
        nibble_set(array, index, value);
        note_change(column->record->pos, section_coord(node.y));
    }

    void enqueue_increase(light_layer which, const node_pos &node,
                          std::uint8_t level) {
        if (level == 0) {
            return;
        }
        queues_[layer_index(which)].increase.push_back(
            increase_entry{.node = node, .level = level});
    }

    void enqueue_decrease(light_layer which, const node_pos &node,
                          std::uint8_t level) {
        if (level == 0) {
            return;
        }
        queues_[layer_index(which)].decrease.push_back(
            decrease_entry{.node = node, .level = level});
    }

    [[nodiscard]] static auto block_bottom(const column_state &column) noexcept
        -> std::int32_t {
        return column.record->y_pos * section_edge;
    }

    [[nodiscard]] static auto block_top(const column_state &column) noexcept
        -> std::int32_t {
        const auto &chunk{*column.record};
        const auto top_section{
            chunk.sections.empty()
                ? chunk.y_pos
                : static_cast<std::int32_t>(chunk.sections.back().y) + 1};
        return top_section * section_edge - 1;
    }

    void rebuild_section_lookup(column_state &column) {
        column.section_lookup.fill(-1);
        const auto &sections{column.record->sections};
        for (std::size_t index{0}; index < sections.size(); ++index) {
            const auto lookup{static_cast<std::int32_t>(sections[index].y) +
                              section_index_offset};
            if (lookup >= 0 && lookup < section_span) {
                column.section_lookup[static_cast<std::size_t>(lookup)] =
                    static_cast<std::int16_t>(index);
            }
        }
        column.section_count = static_cast<std::uint16_t>(sections.size());
    }

    /// Replace the column's light layers with zeroed arrays and rebuild the
    /// layer index. Layers exist for every block section plus the padding
    /// layers below the lowest and above the highest block section; a column
    /// whose sections have gaps keeps no layer in the gap, which is air and
    /// carries no stored light.
    [[nodiscard]] auto reset_layers(column_state &column) -> bool {
        auto &chunk{*column.record};
        const auto layer_ys{required_light_layers(chunk)};
        if (layer_ys.empty()) {
            chunk.storage.light_sections.clear();
            column.layers.clear();
            column.top_layer_y = chunk.y_pos;
            return false;
        }
        const auto top_section{
            static_cast<std::int32_t>(chunk.sections.back().y)};
        chunk.storage.light_sections.clear();
        chunk.storage.light_sections.reserve(layer_ys.size());
        for (const auto y : layer_ys) {
            ::refrain::storage::light_section layer{};
            layer.y = static_cast<std::int8_t>(y);
            layer.sky_light.assign(light_bytes, std::byte{0});
            layer.block_light.assign(light_bytes, std::byte{0});
            chunk.storage.light_sections.push_back(std::move(layer));
        }
        column.top_layer_y = top_section + 1;
        column.layers.clear();
        column.layers.reserve(chunk.storage.light_sections.size());
        for (auto &layer : chunk.storage.light_sections) {
            column.layers.push_back(
                column_state::layer_ref{.y = layer.y, .layer = &layer});
        }
        return true;
    }

    /// Rebuild the layer index from the persisted light arrays and report
    /// whether the record can be trusted: exactly the required layer set, both
    /// nibble arrays at full length. A false answer means the stored light is
    /// incomplete or stale, so the caller relights the column from its blocks
    /// instead of reusing the arrays.
    [[nodiscard]] auto adopt_layers(column_state &column) -> bool {
        auto &chunk{*column.record};
        const auto required{required_light_layers(chunk)};
        const auto &stored{chunk.storage.light_sections};
        if (stored.size() != required.size()) {
            return false;
        }
        for (std::size_t index{0}; index < required.size(); ++index) {
            if (static_cast<std::int32_t>(stored[index].y) != required[index] ||
                stored[index].sky_light.size() != light_bytes ||
                stored[index].block_light.size() != light_bytes) {
                return false;
            }
        }
        column.layers.clear();
        column.layers.reserve(stored.size());
        for (auto &layer : chunk.storage.light_sections) {
            column.layers.push_back(
                column_state::layer_ref{.y = layer.y, .layer = &layer});
        }
        column.top_layer_y =
            chunk.sections.empty()
                ? chunk.y_pos
                : static_cast<std::int32_t>(chunk.sections.back().y) + 1;
        return true;
    }

    /// Scan one column of blocks from the top down and return the Y of its
    /// lowest sky source cell, or `block_top + 1` when no cell of the column
    /// sees the sky.
    ///
    /// The reference lighting engine places the sky source edge at the highest
    /// vertical edge whose two states occlude the edge together: the cell below
    /// loses its direct sky view when the cell below is not fully transparent
    /// (its light dampening is nonzero) or when the face shapes of the two
    /// cells cover the whole cell. A uniform section is one repeated state, so
    /// one edge test covers its whole interior.
    [[nodiscard]] auto compute_floor(const column_state &column,
                                     std::int32_t local_x, std::int32_t local_z)
        -> std::int32_t {
        const auto world_x{column.record->pos.x * section_edge + local_x};
        const auto world_z{column.record->pos.z * section_edge + local_z};
        const auto top{block_top(column)};
        const auto bottom{block_bottom(column)};
        // The cell above the column is open air; an occluding top cell starts
        // no sky source inside the column.
        if (edge_occluded_under_air(block_state_at(world_x, top, world_z))) {
            return top + 1;
        }
        auto y{top};
        while (y > bottom) {
            const auto lookup_y{section_coord(y) + section_index_offset};
            if (lookup_y >= 0 && lookup_y < section_span) {
                const auto index{
                    column.section_lookup[static_cast<std::size_t>(lookup_y)]};
                if (index >= 0) {
                    const auto &section{
                        column.record
                            ->sections[static_cast<std::size_t>(index)]};
                    if (section.uniform) {
                        if (edge_occluded(section.value, section.value)) {
                            return y;
                        }
                        const auto base{static_cast<std::int32_t>(section.y) *
                                        section_edge};
                        if (base <= bottom) {
                            return bottom;
                        }
                        if (edge_occluded(
                                section.value,
                                block_state_at(world_x, base - 1, world_z))) {
                            return base;
                        }
                        y = base - 1;
                        continue;
                    }
                }
            }
            if (edge_occluded(block_state_at(world_x, y, world_z),
                              block_state_at(world_x, y - 1, world_z))) {
                return y;
            }
            --y;
        }
        return bottom;
    }

    void refresh_floors(column_state &column) {
        for (auto z{0}; z < section_edge; ++z) {
            for (auto x{0}; x < section_edge; ++x) {
                column.lowest_source[static_cast<std::size_t>((z << 4) | x)] =
                    compute_floor(column, x, z);
            }
        }
    }

    [[nodiscard]] auto floor_at(const column_state &column,
                                std::int32_t local_x,
                                std::int32_t local_z) const -> std::int32_t {
        return column
            .lowest_source[static_cast<std::size_t>((local_z << 4) | local_x)];
    }

    [[nodiscard]] auto block_node(const column_state &column,
                                  std::int32_t local_x, std::int32_t y,
                                  std::int32_t local_z) const -> node_pos {
        return node_pos{.x = column.record->pos.x * section_edge + local_x,
                        .y = static_cast<std::int16_t>(y),
                        .z = column.record->pos.z * section_edge + local_z};
    }

    /// Fill the sky light source region of one column and enqueue the cells
    /// whose light reaches a non-source neighbour.
    void seed_sky_column(column_state &column, std::int32_t local_x,
                         std::int32_t local_z, std::int32_t floor_y) {
        const auto bottom{block_bottom(column)};
        const auto top{block_top(column)};
        const auto lowest{std::max(floor_y, bottom)};
        if (lowest <= top) {
            for (auto y{top}; y >= lowest; --y) {
                set_level(block_node(column, local_x, y, local_z),
                          light_layer::sky, max_light_level);
            }
            enqueue_increase(light_layer::sky,
                             block_node(column, local_x, lowest, local_z),
                             max_light_level);
        }
        for (std::size_t side{0}; side < side_offsets.size(); ++side) {
            const auto [dx, dz]{side_offsets[side]};
            const auto neighbour_x{local_x + dx};
            const auto neighbour_z{local_z + dz};
            if (neighbour_x < 0 || neighbour_x >= section_edge ||
                neighbour_z < 0 || neighbour_z >= section_edge) {
                continue;
            }
            const auto neighbour_floor{
                floor_at(column, neighbour_x, neighbour_z)};
            if (neighbour_floor >= floor_y) {
                continue;
            }
            for (auto y{std::max(floor_y - 1, bottom)};
                 y >= std::max(neighbour_floor, bottom); --y) {
                const auto node{block_node(column, local_x, y, local_z)};
                const auto source{
                    block_node(column, neighbour_x, y, neighbour_z)};
                // Every other propagation step tests the two face shapes
                // before it moves light across an edge; this seeding writes
                // the receiving cell directly, so it must run the same test.
                // The source cell is a sky source: its floor is below this
                // level.
                if (shape_occludes(block_state_at(source.x, source.y, source.z),
                                   block_state_at(node.x, node.y, node.z),
                                   side_directions[side])) {
                    continue;
                }
                const auto candidate{static_cast<std::int32_t>(
                    max_light_level - opacity(node.x, node.y, node.z))};
                if (candidate > 0) {
                    enqueue_increase(light_layer::sky, node,
                                     static_cast<std::uint8_t>(candidate));
                }
            }
        }
    }

    /// Enqueue this column's sky sources that face a non-source cell of an
    /// already loaded neighbour. Without this a column loaded after its
    /// neighbour would never push its own sky light across the border.
    void seed_cross_border_sources(column_state &column) {
        const auto top{block_top(column)};
        const auto bottom{block_bottom(column)};
        const auto base_x{column.record->pos.x * section_edge};
        const auto base_z{column.record->pos.z * section_edge};
        const std::array<std::pair<std::int32_t, std::int32_t>, 4> sides{{
            {-1, 0},
            {1, 0},
            {0, -1},
            {0, 1},
        }};
        for (const auto &[side_x, side_z] : sides) {
            for (auto lateral{0}; lateral < section_edge; ++lateral) {
                const auto local_x{side_x < 0   ? 0
                                   : side_x > 0 ? section_edge - 1
                                                : lateral};
                const auto local_z{side_z < 0   ? 0
                                   : side_z > 0 ? section_edge - 1
                                                : lateral};
                const auto world_x{base_x + local_x};
                const auto world_z{base_z + local_z};
                auto *neighbour{find_column(section_coord(world_x + side_x),
                                            section_coord(world_z + side_z))};
                if (neighbour == nullptr) {
                    continue;
                }
                const auto neighbour_x{local_coord(world_x + side_x)};
                const auto neighbour_z{local_coord(world_z + side_z)};
                const auto our_floor{floor_at(column, local_x, local_z)};
                const auto neighbour_floor{
                    floor_at(*neighbour, neighbour_x, neighbour_z)};
                if (our_floor >= neighbour_floor) {
                    continue;
                }
                const auto first{std::max(our_floor, bottom)};
                const auto last{std::min(neighbour_floor - 1, top)};
                for (auto y{first}; y <= last; ++y) {
                    enqueue_increase(light_layer::sky,
                                     block_node(column, local_x, y, local_z),
                                     max_light_level);
                }
            }
        }
    }

    /// Enqueue the light the neighbouring columns carry into this column
    /// across a border.
    void seed_border_light(column_state &column, light_layer which) {
        const auto bottom{block_bottom(column)};
        const auto top{block_top(column)};
        const auto base_x{column.record->pos.x * section_edge};
        const auto base_z{column.record->pos.z * section_edge};
        for (auto y{top}; y >= bottom; --y) {
            for (auto lateral{0}; lateral < section_edge; ++lateral) {
                const std::array<node_pos, 4> inside{{
                    {.x = base_x,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z + lateral},
                    {.x = base_x + section_edge - 1,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z + lateral},
                    {.x = base_x + lateral,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z},
                    {.x = base_x + lateral,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z + section_edge - 1},
                }};
                const std::array<node_pos, 4> outside{{
                    {.x = inside[0].x - 1, .y = inside[0].y, .z = inside[0].z},
                    {.x = inside[1].x + 1, .y = inside[1].y, .z = inside[1].z},
                    {.x = inside[2].x, .y = inside[2].y, .z = inside[2].z - 1},
                    {.x = inside[3].x, .y = inside[3].y, .z = inside[3].z + 1},
                }};
                for (std::size_t side{0}; side < inside.size(); ++side) {
                    if (find_column(section_coord(outside[side].x),
                                    section_coord(outside[side].z)) ==
                        nullptr) {
                        continue;
                    }
                    const auto outside_level{level_at(outside[side], which)};
                    if (outside_level == 0) {
                        continue;
                    }
                    if (shape_occludes(
                            block_state_at(outside[side].x, outside[side].y,
                                           outside[side].z),
                            block_state_at(inside[side].x, inside[side].y,
                                           inside[side].z),
                            side_directions[side])) {
                        continue;
                    }
                    const auto &node{inside[side]};
                    const auto candidate{static_cast<std::int32_t>(
                        outside_level - opacity(node.x, node.y, node.z))};
                    if (candidate > 0) {
                        enqueue_increase(which, node,
                                         static_cast<std::uint8_t>(candidate));
                    }
                }
            }
        }
    }

    /// Place every emissive block of the column into the increase queue.
    void seed_block_sources(column_state &column) {
        const auto &chunk{*column.record};
        const auto base_x{chunk.pos.x * section_edge};
        const auto base_z{chunk.pos.z * section_edge};
        for (const auto &section : chunk.sections) {
            const auto base_y{static_cast<std::int32_t>(section.y) *
                              section_edge};
            if (section.uniform) {
                const auto emission{property(section.value).emission};
                if (emission == 0) {
                    continue;
                }
                for (auto local_y{0}; local_y < section_edge; ++local_y) {
                    for (auto local_z{0}; local_z < section_edge; ++local_z) {
                        for (auto local_x{0}; local_x < section_edge;
                             ++local_x) {
                            queues_[layer_index(light_layer::block)]
                                .increase.push_back(increase_entry{
                                    .node =
                                        node_pos{.x = base_x + local_x,
                                                 .y = static_cast<std::int16_t>(
                                                     base_y + local_y),
                                                 .z = base_z + local_z},
                                    .level = emission});
                        }
                    }
                }
                continue;
            }
            for (auto local_y{0}; local_y < section_edge; ++local_y) {
                for (auto local_z{0}; local_z < section_edge; ++local_z) {
                    for (auto local_x{0}; local_x < section_edge; ++local_x) {
                        const auto id{section.blocks[local_index(
                            local_y, local_z, local_x)]};
                        const auto emission{property(id).emission};
                        if (emission == 0) {
                            continue;
                        }
                        queues_[layer_index(light_layer::block)]
                            .increase.push_back(increase_entry{
                                .node = node_pos{.x = base_x + local_x,
                                                 .y = static_cast<std::int16_t>(
                                                     base_y + local_y),
                                                 .z = base_z + local_z},
                                .level = emission});
                    }
                }
            }
        }
    }

    void propagate_increase(light_layer which, const node_pos &node,
                            std::uint8_t level) {
        if (level > level_at(node, which)) {
            set_level(node, which, level);
        }
        const auto from_state{block_state_at(node.x, node.y, node.z)};
        for (std::size_t direction{0}; direction < directions.size();
             ++direction) {
            const auto &step{directions[direction]};
            const node_pos neighbour{
                .x = node.x + step.dx,
                .y = static_cast<std::int16_t>(node.y + step.dy),
                .z = node.z + step.dz};
            auto *neighbour_column{find_column(section_coord(neighbour.x),
                                               section_coord(neighbour.z))};
            if (neighbour_column == nullptr ||
                neighbour.y < block_bottom(*neighbour_column) ||
                neighbour.y > block_top(*neighbour_column) ||
                neighbour_column->layer(section_coord(neighbour.y)) ==
                    nullptr) {
                continue;
            }
            const auto cost{opacity(neighbour.x, neighbour.y, neighbour.z)};
            if (level <= cost) {
                continue;
            }
            const auto candidate{static_cast<std::uint8_t>(level - cost)};
            if (candidate > level_at(neighbour, which) &&
                !shape_occludes(
                    from_state,
                    block_state_at(neighbour.x, neighbour.y, neighbour.z),
                    direction)) {
                set_level(neighbour, which, candidate);
                queues_[layer_index(which)].increase.push_back(
                    increase_entry{.node = neighbour, .level = candidate});
            }
        }
    }

    [[nodiscard]] auto intrinsic_level(const node_pos &node, light_layer which)
        -> std::uint8_t {
        if (which == light_layer::block) {
            return emission(node.x, node.y, node.z);
        }
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return 0;
        }
        return node.y >= floor_at(*column, local_coord(node.x),
                                  local_coord(node.z))
                   ? max_light_level
                   : 0;
    }

    void propagate_decrease(light_layer which, const node_pos &node,
                            std::uint8_t old_level) {
        for (const auto &step : directions) {
            const node_pos neighbour{
                .x = node.x + step.dx,
                .y = static_cast<std::int16_t>(node.y + step.dy),
                .z = node.z + step.dz};
            auto *neighbour_column{find_column(section_coord(neighbour.x),
                                               section_coord(neighbour.z))};
            if (neighbour_column == nullptr ||
                neighbour.y < block_bottom(*neighbour_column) ||
                neighbour.y > block_top(*neighbour_column) ||
                neighbour_column->layer(section_coord(neighbour.y)) ==
                    nullptr) {
                continue;
            }
            const auto neighbour_level{level_at(neighbour, which)};
            if (neighbour_level == 0) {
                continue;
            }
            const auto cost{opacity(neighbour.x, neighbour.y, neighbour.z)};
            if (old_level <= cost || neighbour_level <= old_level - cost) {
                set_level(neighbour, which, 0);
                enqueue_decrease(which, neighbour, neighbour_level);
                const auto intrinsic{intrinsic_level(neighbour, which)};
                if (intrinsic > 0) {
                    enqueue_increase(which, neighbour, intrinsic);
                }
            } else {
                // The neighbour keeps light from another source: let it
                // re-propagate into the cells this decrease cleared.
                enqueue_increase(which, neighbour, neighbour_level);
            }
        }
    }

    void apply_floor_change(column_state &column, const node_pos &node,
                            std::int32_t new_floor) {
        auto &floor{column.lowest_source[static_cast<std::size_t>(
            (local_coord(node.z) << 4) | local_coord(node.x))]};
        if (new_floor == floor) {
            return;
        }
        const auto bottom{block_bottom(column)};
        const auto top{block_top(column)};
        if (new_floor > floor) {
            const auto last{std::min(new_floor - 1, top)};
            for (auto y{std::max(floor, bottom)}; y <= last; ++y) {
                const node_pos cell{.x = node.x,
                                    .y = static_cast<std::int16_t>(y),
                                    .z = node.z};
                const auto level{level_at(cell, light_layer::sky)};
                if (level == 0) {
                    continue;
                }
                set_level(cell, light_layer::sky, 0);
                enqueue_decrease(light_layer::sky, cell, level);
            }
        } else {
            const auto last{std::min(floor - 1, top)};
            for (auto y{std::max(new_floor, bottom)}; y <= last; ++y) {
                const node_pos cell{.x = node.x,
                                    .y = static_cast<std::int16_t>(y),
                                    .z = node.z};
                set_level(cell, light_layer::sky, max_light_level);
                enqueue_increase(light_layer::sky, cell, max_light_level);
            }
        }
        floor = new_floor;
    }

    void check_node(const node_pos &node) {
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return;
        }
        if (column->section_count != column->record->sections.size()) {
            rebuild_section_lookup(*column);
        }
        const auto new_floor{
            compute_floor(*column, local_coord(node.x), local_coord(node.z))};
        apply_floor_change(*column, node, new_floor);
        for (const auto which : {light_layer::sky, light_layer::block}) {
            const auto old_level{level_at(node, which)};
            const auto intrinsic{intrinsic_level(node, which)};
            if (old_level != 0) {
                set_level(node, which, 0);
                enqueue_decrease(which, node, old_level);
            }
            if (intrinsic > 0) {
                enqueue_increase(which, node, intrinsic);
            }
            if (old_level == 0 && intrinsic == 0) {
                // The cell may admit light it blocked before.
                for (const auto &step : directions) {
                    const node_pos neighbour{
                        .x = node.x + step.dx,
                        .y = static_cast<std::int16_t>(node.y + step.dy),
                        .z = node.z + step.dz};
                    const auto level{level_at(neighbour, which)};
                    if (level > 0) {
                        enqueue_increase(which, neighbour, level);
                    }
                }
            }
        }
    }

    void initial_light(column_state &column) {
        refresh_floors(column);
        for (auto local_z{0}; local_z < section_edge; ++local_z) {
            for (auto local_x{0}; local_x < section_edge; ++local_x) {
                seed_sky_column(column, local_x, local_z,
                                floor_at(column, local_x, local_z));
            }
        }
        seed_cross_border_sources(column);
        seed_block_sources(column);
        seed_border_light(column, light_layer::sky);
        seed_border_light(column, light_layer::block);
    }

    [[nodiscard]] auto queue_size() const noexcept -> std::size_t {
        auto total{check_queue_.size()};
        for (const auto &queue : queues_) {
            total += queue.decrease.size() + queue.increase.size();
        }
        return total;
    }

    /// Record the current queue length as a peak sample.
    void note_pending() const noexcept {
        pending_peak_ = std::max(pending_peak_, queue_size());
    }

    [[nodiscard]] auto pending_nodes() const noexcept -> std::size_t {
        const auto total{queue_size()};
        pending_peak_ = std::max(pending_peak_, total);
        return total;
    }

    /// Process every queued node, decrease and increase pass in layer order.
    /// Returns the number of entries processed.
    ///
    /// The drain records the queue peak at its boundaries and every
    /// `peak_sample_interval` entries, and adds its processed nodes and wall
    /// time to the counters of `origin`. The clock is read once per drain, not
    /// per node.
    auto drain(std::size_t budget, drain_origin origin) -> std::size_t {
        const auto begin{std::chrono::steady_clock::now()};
        note_pending();
        std::size_t processed{0};
        while (!check_queue_.empty() && processed < budget) {
            const auto node{check_queue_.front()};
            check_queue_.pop_front();
            check_node(node);
            ++processed;
            if ((processed & (peak_sample_interval - 1U)) == 0) {
                note_pending();
            }
        }
        note_pending();
        for (std::size_t index{0}; index < layer_count; ++index) {
            const auto which{index == 0 ? light_layer::sky
                                        : light_layer::block};
            auto &queue{queues_[index].decrease};
            while (!queue.empty() && processed < budget) {
                const auto entry{queue.front()};
                queue.pop_front();
                propagate_decrease(which, entry.node, entry.level);
                ++processed;
                if ((processed & (peak_sample_interval - 1U)) == 0) {
                    note_pending();
                }
            }
        }
        note_pending();
        for (std::size_t index{0}; index < layer_count; ++index) {
            const auto which{index == 0 ? light_layer::sky
                                        : light_layer::block};
            auto &queue{queues_[index].increase};
            while (!queue.empty() && processed < budget) {
                const auto entry{queue.front()};
                queue.pop_front();
                propagate_increase(which, entry.node, entry.level);
                ++processed;
                if ((processed & (peak_sample_interval - 1U)) == 0) {
                    note_pending();
                }
            }
        }
        note_pending();
        const auto elapsed_ns{static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin)
                .count())};
        if (origin == drain_origin::initial) {
            ++initial_drain_calls_;
            initial_nodes_ += processed;
            initial_ns_ += elapsed_ns;
        }
        ++drain_calls_;
        drain_nodes_ += processed;
        drain_ns_ += elapsed_ns;
        finish_drain();
        return processed;
    }

    [[nodiscard]] auto stats() const noexcept -> light_stats {
        const auto waiting{pending_nodes()};
        return light_stats{
            .columns_lit = columns_lit_,
            .columns_trusted = columns_trusted_,
            .initial_drain_calls = initial_drain_calls_,
            .initial_nodes = initial_nodes_,
            .initial_ns = initial_ns_,
            .drain_calls = drain_calls_,
            .drain_nodes = drain_nodes_,
            .drain_ns = drain_ns_,
            .block_edits = block_edits_,
            .pending_peak = pending_peak_,
            .pending = static_cast<std::uint64_t>(waiting),
        };
    }

    void reset_stats() noexcept {
        columns_lit_ = 0;
        columns_trusted_ = 0;
        initial_drain_calls_ = 0;
        initial_nodes_ = 0;
        initial_ns_ = 0;
        drain_calls_ = 0;
        drain_nodes_ = 0;
        drain_ns_ = 0;
        block_edits_ = 0;
        pending_peak_ = 0;
    }
};

light_engine::light_engine(world &owner)
    : impl_{std::make_unique<impl>(owner)} {}

light_engine::light_engine(light_engine &&other) noexcept = default;

auto light_engine::operator=(light_engine &&other) noexcept
    -> light_engine & = default;

light_engine::~light_engine() = default;

void light_engine::reload_properties() {
    impl_->properties_.clear();
    // Every stored array was computed with the previous table, so no loaded
    // column is correct under the new one; `world` relights them right after
    // installing the table, and this keeps a later `column_loaded` from
    // adopting arrays the new table did not produce.
    for (auto &[pos, column] : impl_->columns_) {
        static_cast<void>(pos);
        if (column.record != nullptr) {
            column.record->storage.light_correct = false;
        }
    }
    impl_->dirty_columns_.clear();
}

void light_engine::attach(world &owner) {
    impl_->owner_ = &owner;
    impl_->columns_.clear();
    impl_->properties_.clear();
    impl_->clear_cache();
    for (const auto &pos : owner.chunk_positions()) {
        auto *column{owner.find_chunk(pos)};
        if (column == nullptr) {
            continue;
        }
        column_state state{};
        state.record = column;
        state.top_layer_y = column->sections.empty()
                                ? column->y_pos
                                : column->sections.back().y + 1;
        for (auto &layer : column->storage.light_sections) {
            state.layers.push_back(
                column_state::layer_ref{.y = layer.y, .layer = &layer});
        }
        impl_->rebuild_section_lookup(state);
        state.section_count =
            static_cast<std::uint16_t>(column->sections.size());
        impl_->refresh_floors(state);
        impl_->columns_.emplace(pos, std::move(state));
    }
    // A column that does not claim correct light was mid-relight when the
    // engine took over. The queue that carried the pending update does not
    // survive the move, so relight the column instead of keeping arrays the
    // engine cannot prove.
    for (const auto &pos : owner.chunk_positions()) {
        auto *column{owner.find_chunk(pos)};
        if (column != nullptr && !column->storage.light_correct) {
            column_loaded(*column);
        }
    }
}

void light_engine::column_loaded(chunk &column) {
    impl_->clear_cache();
    auto &state{impl_->columns_[column.pos]};
    state.record = &column;
    // A record that claims correct light is only reused when the stored arrays
    // are exactly the complete layer set the blocks require. Everything else -
    // old data without the marker, an imported or edited column, a record whose
    // arrays were compacted away - cannot be proven correct and is relit.
    if (column.storage.light_correct && impl_->adopt_layers(state)) {
        impl_->dirty_columns_.erase(column.pos);
        impl_->rebuild_section_lookup(state);
        // The stored light already covers the column; the sky-source index is
        // derived from the blocks and later edits need it.
        impl_->refresh_floors(state);
        ++impl_->columns_trusted_;
        return;
    }
    if (!impl_->reset_layers(state)) {
        impl_->dirty_columns_.erase(column.pos);
        impl_->rebuild_section_lookup(state);
        column.storage.light_correct = true;
        return;
    }
    impl_->rebuild_section_lookup(state);
    impl_->suppressed_origin_ = column.pos;
    impl_->initial_light(state);
    impl_->suppressed_origin_.reset();
    ++impl_->columns_lit_;
    // The column's sections travel in its chunk packet; only light that
    // reached a neighbour column needs a separate update.
    static_cast<void>(impl_->drain(std::numeric_limits<std::size_t>::max(),
                                   drain_origin::initial));
    // The initial pass has completed: the stored arrays are complete and the
    // same bytes the chunk packet carries. The drain above also cleared the
    // column's stale marker, so a later save writes the correct value.
    column.storage.light_correct = true;
}

void light_engine::column_unloaded(const chunk_pos &pos) {
    impl_->columns_.erase(pos);
    impl_->dirty_columns_.erase(pos);
    impl_->clear_cache();
}

void light_engine::block_changed(const block_pos &pos) {
    ++impl_->block_edits_;
    // The edit changes the light the blocks imply. Until the update this call
    // queues has been fully drained, the stored arrays are stale and the
    // column must not claim correctness; `finish_drain()` clears the marker
    // once the incremental relight completed.
    const chunk_pos changed_column{section_coord(pos.x), section_coord(pos.z)};
    impl_->mark_light_dirty(changed_column);
    // A block edit can add a section the column did not have: above the current
    // top, or inside a vertical gap of a sparse imported column. The light
    // layer set follows the block section set, and a layer that does not exist
    // would silently drop every `set_level` for the new section, so the column
    // is rebuilt instead of extending the index in place. Comparing the section
    // count catches an insertion anywhere; the top comparison is the same
    // answer for an appended section and documents the common case.
    auto *column{impl_->find_column(changed_column.x, changed_column.z)};
    if (column != nullptr) {
        const auto &sections{column->record->sections};
        const auto top_changed{!sections.empty() &&
                               static_cast<std::int32_t>(sections.back().y) +
                                       1 !=
                                   column->top_layer_y};
        const auto section_set_changed{
            static_cast<std::size_t>(column->section_count) != sections.size()};
        if (top_changed || section_set_changed) {
            const auto changed_pos{column->record->pos};
            const auto changed_top{
                sections.empty()
                    ? column->record->y_pos
                    : static_cast<std::int32_t>(sections.back().y) + 1};
            column_loaded(*column->record);
            // The whole column was relit: clients holding it need every
            // section.
            for (auto y{column->record->y_pos - 1}; y <= changed_top; ++y) {
                impl_->changes_.insert(light_section_change{
                    .pos = changed_pos,
                    .section_y = static_cast<std::int8_t>(y)});
            }
            return;
        }
    }
    impl_->check_queue_.push_back(node_pos{.x = pos.x, .y = pos.y, .z = pos.z});
}

auto light_engine::run(std::size_t budget) -> std::size_t {
    return impl_->drain(budget, drain_origin::incremental);
}

auto light_engine::pending() const noexcept -> bool {
    return !impl_->check_queue_.empty() ||
           !impl_->queues_[0].decrease.empty() ||
           !impl_->queues_[1].decrease.empty() ||
           !impl_->queues_[0].increase.empty() ||
           !impl_->queues_[1].increase.empty();
}

auto light_engine::pending_nodes() const noexcept -> std::size_t {
    return impl_->pending_nodes();
}

auto light_engine::take_changes() -> std::vector<light_section_change> {
    std::vector<light_section_change> changes{impl_->changes_.begin(),
                                              impl_->changes_.end()};
    impl_->changes_.clear();
    return changes;
}

auto light_engine::stats() const noexcept -> light_stats {
    return impl_->stats();
}

void light_engine::reset_stats() noexcept { impl_->reset_stats(); }

auto light_engine::sky_light_at(const block_pos &pos) const -> std::uint8_t {
    return impl_->level_at(node_pos{.x = pos.x, .y = pos.y, .z = pos.z},
                           light_layer::sky);
}

auto light_engine::block_light_at(const block_pos &pos) const -> std::uint8_t {
    return impl_->level_at(node_pos{.x = pos.x, .y = pos.y, .z = pos.z},
                           light_layer::block);
}

} // namespace refrain::sim
