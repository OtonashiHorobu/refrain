// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Ocean monument start generation and placement. The building owns a room
// graph of fine rooms, fits the remaining rooms in a fixed fitter order and
// writes the shell; each room writes its interior when its bounding box
// intersects the chunk being placed.

#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/monument.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

using monument_direction::down;
using monument_direction::east;
using monument_direction::north;
using monument_direction::south;
using monument_direction::up;
using monument_direction::west;

constexpr jint room_width = 8;
constexpr jint room_depth = 8;
constexpr jint room_height = 4;
constexpr jint grid_width = 5;
constexpr jint grid_depth = 5;
constexpr jint grid_height = 3;
constexpr jint grid_size = 75;
constexpr jint source_index = 2;        // getRoomIndex(2, 0, 0)
constexpr jint top_connect_index = 52;  // getRoomIndex(2, 2, 0)
constexpr jint left_wing_connect = 25;  // getRoomIndex(0, 1, 0)
constexpr jint right_wing_connect = 29; // getRoomIndex(4, 1, 0)
constexpr jint left_wing_index = 1001;
constexpr jint right_wing_index = 1002;
constexpr jint penthouse_index = 1003;
constexpr jint building_width = 58;
constexpr jint building_height = 23;
constexpr jint building_depth = 58;
constexpr jint building_y = 39;
constexpr jint biome_range_check = 29;

constexpr std::array<std::array<jint, 3>, 6> direction_steps{{
    {0, -1, 0}, // down
    {0, 1, 0},  // up
    {0, 0, -1}, // north
    {0, 0, 1},  // south
    {-1, 0, 0}, // west
    {1, 0, 0},  // east
}};

constexpr std::array<monument_direction, 6> opposite_directions{
    monument_direction::up,    monument_direction::down,
    monument_direction::south, monument_direction::north,
    monument_direction::east,  monument_direction::west,
};

constexpr auto opposite(monument_direction direction) -> monument_direction {
    return opposite_directions[static_cast<std::size_t>(direction)];
}

constexpr auto get_room_index(jint room_x, jint room_y, jint room_z) -> jint {
    return room_y * 25 + room_z * 5 + room_x;
}

struct room_definition {
    jint index{-1};
    std::array<room_definition *, 6> connections{};
    std::array<bool, 6> has_opening{};
    bool claimed{false};
    bool is_source{false};
    jint scan_index{0};

    void set_connection(monument_direction direction, room_definition *other) {
        connections[static_cast<std::size_t>(direction)] = other;
        other->connections[static_cast<std::size_t>(opposite(direction))] =
            this;
    }

    void update_openings() {
        for (std::size_t i = 0; i < connections.size(); ++i) {
            has_opening[i] = connections[i] != nullptr;
        }
    }

    [[nodiscard]] auto find_source(jint scan) -> bool {
        if (is_source) {
            return true;
        }
        scan_index = scan;
        for (std::size_t i = 0; i < connections.size(); ++i) {
            if (connections[i] != nullptr && has_opening[i] &&
                connections[i]->scan_index != scan &&
                connections[i]->find_source(scan)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto is_special() const -> bool { return index >= 75; }

    [[nodiscard]] auto count_openings() const -> jint {
        jint count = 0;
        for (const bool open : has_opening) {
            if (open) {
                ++count;
            }
        }
        return count;
    }
};

struct room_graph {
    std::vector<std::unique_ptr<room_definition>> owned{};
    std::vector<room_definition *> ordered{};
    room_definition *source{nullptr};
    room_definition *core{nullptr};
    room_definition *roof{nullptr};
    room_definition *left_wing{nullptr};
    room_definition *right_wing{nullptr};
};

auto make_room(room_graph &graph, jint index) -> room_definition * {
    graph.owned.push_back(std::make_unique<room_definition>());
    graph.owned.back()->index = index;
    return graph.owned.back().get();
}

auto shuffle_rooms(std::vector<room_definition *> &rooms,
                   worldgen_random &random) -> void {
    for (std::size_t size = rooms.size(); size > 1; --size) {
        const auto swap_to =
            static_cast<std::size_t>(random.next_int(static_cast<jint>(size)));
        std::swap(rooms[size - 1], rooms[swap_to]);
    }
}

auto generate_room_graph(worldgen_random &random) -> room_graph {
    room_graph graph;
    std::array<room_definition *, grid_size> grid{};

    for (jint x = 0; x < grid_width; ++x) {
        for (jint z = 0; z < 4; ++z) {
            const jint index = get_room_index(x, 0, z);
            grid[static_cast<std::size_t>(index)] = make_room(graph, index);
        }
    }
    for (jint x = 0; x < grid_width; ++x) {
        for (jint z = 0; z < 4; ++z) {
            const jint index = get_room_index(x, 1, z);
            grid[static_cast<std::size_t>(index)] = make_room(graph, index);
        }
    }
    for (jint x = 1; x < 4; ++x) {
        for (jint z = 0; z < 2; ++z) {
            const jint index = get_room_index(x, 2, z);
            grid[static_cast<std::size_t>(index)] = make_room(graph, index);
        }
    }

    graph.source = grid[source_index];

    for (jint x = 0; x < grid_width; ++x) {
        for (jint z = 0; z < grid_depth; ++z) {
            for (jint y = 0; y < grid_height; ++y) {
                const jint index = get_room_index(x, y, z);
                auto *room = grid[static_cast<std::size_t>(index)];
                if (room == nullptr) {
                    continue;
                }
                for (std::size_t i = 0; i < direction_steps.size(); ++i) {
                    const auto &step = direction_steps[i];
                    const jint near_x = x + step[0];
                    const jint near_y = y + step[1];
                    const jint near_z = z + step[2];
                    if (near_x < 0 || near_x >= grid_width || near_y < 0 ||
                        near_y >= grid_height || near_z < 0 ||
                        near_z >= grid_depth) {
                        continue;
                    }
                    auto *neighbour = grid[static_cast<std::size_t>(
                        get_room_index(near_x, near_y, near_z))];
                    if (neighbour == nullptr) {
                        continue;
                    }
                    const auto direction = static_cast<monument_direction>(i);
                    if (near_z == z) {
                        room->set_connection(direction, neighbour);
                    } else {
                        room->set_connection(opposite(direction), neighbour);
                    }
                }
            }
        }
    }

    graph.roof = make_room(graph, penthouse_index);
    graph.left_wing = make_room(graph, left_wing_index);
    graph.right_wing = make_room(graph, right_wing_index);
    grid[top_connect_index]->set_connection(up, graph.roof);
    grid[left_wing_connect]->set_connection(south, graph.left_wing);
    grid[right_wing_connect]->set_connection(south, graph.right_wing);
    graph.roof->claimed = true;
    graph.left_wing->claimed = true;
    graph.right_wing->claimed = true;
    graph.source->is_source = true;
    graph.core = grid[static_cast<std::size_t>(
        get_room_index(random.next_int(4), 0, 2))];
    graph.core->claimed = true;
    graph.core->connections[static_cast<std::size_t>(east)]->claimed = true;
    graph.core->connections[static_cast<std::size_t>(north)]->claimed = true;
    graph.core->connections[static_cast<std::size_t>(east)]
        ->connections[static_cast<std::size_t>(north)]
        ->claimed = true;
    graph.core->connections[static_cast<std::size_t>(up)]->claimed = true;
    graph.core->connections[static_cast<std::size_t>(east)]
        ->connections[static_cast<std::size_t>(up)]
        ->claimed = true;
    graph.core->connections[static_cast<std::size_t>(north)]
        ->connections[static_cast<std::size_t>(up)]
        ->claimed = true;
    graph.core->connections[static_cast<std::size_t>(east)]
        ->connections[static_cast<std::size_t>(north)]
        ->connections[static_cast<std::size_t>(up)]
        ->claimed = true;

    std::vector<room_definition *> rooms;
    rooms.reserve(grid_size);
    for (auto *definition : grid) {
        if (definition == nullptr) {
            continue;
        }
        definition->update_openings();
        rooms.push_back(definition);
    }
    graph.roof->update_openings();
    shuffle_rooms(rooms, random);

    jint scan_index = 1;
    for (auto *definition : rooms) {
        jint close_count = 0;
        jint attempts = 0;
        while (close_count < 2 && attempts < 5) {
            ++attempts;
            const auto face = static_cast<std::size_t>(random.next_int(6));
            if (!definition->has_opening[face]) {
                continue;
            }
            const auto back = static_cast<std::size_t>(
                opposite(static_cast<monument_direction>(face)));
            definition->has_opening[face] = false;
            definition->connections[face]->has_opening[back] = false;
            const bool first = definition->find_source(scan_index++);
            bool second = false;
            if (first) {
                second =
                    definition->connections[face]->find_source(scan_index++);
            }
            if (first && second) {
                ++close_count;
            } else {
                definition->has_opening[face] = true;
                definition->connections[face]->has_opening[back] = true;
            }
        }
    }

    rooms.push_back(graph.roof);
    rooms.push_back(graph.left_wing);
    rooms.push_back(graph.right_wing);
    graph.ordered = std::move(rooms);
    return graph;
}

auto make_bounding_box(jint west, jint floor, jint north, jint orientation,
                       jint width, jint height, jint depth) -> structure_bbox {
    const bool along_z = orientation == 0 || orientation == 2;
    return structure_bbox{
        .min_x = west,
        .min_y = floor,
        .min_z = north,
        .max_x = west + (along_z ? width : depth) - 1,
        .max_y = floor + height - 1,
        .max_z = north + (along_z ? depth : width) - 1,
    };
}

void move_box(structure_bbox &box, jint x, jint y, jint z) {
    box.min_x += x;
    box.min_y += y;
    box.min_z += z;
    box.max_x += x;
    box.max_y += y;
    box.max_z += z;
}

auto make_room_box(jint orientation, jint room_index, jint width, jint height,
                   jint depth) -> structure_bbox {
    auto box = make_bounding_box(0, 0, 0, orientation, width * room_width,
                                 height * room_height, depth * room_depth);
    const jint room_x = room_index % 5;
    const jint room_z = room_index / 5 % 5;
    const jint room_y = room_index / 25;
    switch (orientation) {
    case 2: // north
        move_box(box, room_x * room_width, room_y * room_height,
                 -(room_z + depth) * room_depth + 1);
        break;
    case 0: // south
        move_box(box, room_x * room_width, room_y * room_height,
                 room_z * room_depth);
        break;
    case 1: // west
        move_box(box, -(room_z + depth) * room_depth + 1, room_y * room_height,
                 room_x * room_width);
        break;
    default: // east
        move_box(box, room_z * room_depth, room_y * room_height,
                 room_x * room_width);
        break;
    }
    return box;
}

auto building_world_x(const structure_bbox &box, jint orientation, jint x,
                      jint z) -> jint {
    switch (orientation) {
    case 2: // north
    case 0: // south
        return box.min_x + x;
    case 1: // west
        return box.max_x - z;
    default: // east
        return box.min_x + z;
    }
}

auto building_world_z(const structure_bbox &box, jint orientation, jint x,
                      jint z) -> jint {
    switch (orientation) {
    case 2: // north
        return box.max_z - z;
    case 0: // south
        return box.min_z + z;
    case 1: // west
    case 3: // east
        return box.min_z + x;
    default:
        return box.min_z + z;
    }
}

auto building_pos(const structure_bbox &box, jint orientation, jint x, jint y,
                  jint z) -> block_pos {
    return block_pos{.x = building_world_x(box, orientation, x, z),
                     .y = box.min_y + y,
                     .z = building_world_z(box, orientation, x, z)};
}

auto room_state(const room_definition &room) -> monument_room_state {
    return monument_room_state{
        .index = room.index,
        .openings = room.has_opening,
        .up_connected =
            room.connections[static_cast<std::size_t>(up)] != nullptr,
    };
}

constexpr auto piece_type(monument_piece_kind kind) -> std::string_view {
    switch (kind) {
    case monument_piece_kind::building:
        return "minecraft:omb";
    case monument_piece_kind::entry_room:
        return "minecraft:omentry";
    case monument_piece_kind::core_room:
        return "minecraft:omcr";
    case monument_piece_kind::double_x_room:
        return "minecraft:omdxr";
    case monument_piece_kind::double_xy_room:
        return "minecraft:omdxyr";
    case monument_piece_kind::double_y_room:
        return "minecraft:omdyr";
    case monument_piece_kind::double_yz_room:
        return "minecraft:omdyzr";
    case monument_piece_kind::double_z_room:
        return "minecraft:omdzr";
    case monument_piece_kind::simple_room:
        return "minecraft:omsimple";
    case monument_piece_kind::simple_top_room:
        return "minecraft:omsimplet";
    case monument_piece_kind::wing_room:
        return "minecraft:omwr";
    case monument_piece_kind::penthouse:
        return "minecraft:ompenthouse";
    }
    return "minecraft:omb";
}

constexpr auto room_box_size(monument_piece_kind kind) -> std::array<jint, 3> {
    switch (kind) {
    case monument_piece_kind::entry_room:
        return {1, 1, 1};
    case monument_piece_kind::core_room:
        return {2, 2, 2};
    case monument_piece_kind::double_x_room:
        return {2, 1, 1};
    case monument_piece_kind::double_xy_room:
        return {2, 2, 1};
    case monument_piece_kind::double_y_room:
        return {1, 2, 1};
    case monument_piece_kind::double_yz_room:
        return {1, 2, 2};
    case monument_piece_kind::double_z_room:
        return {1, 1, 2};
    default:
        return {1, 1, 1};
    }
}

} // namespace

auto ocean_monument_start(const chunk_pos &chunk, jlong seed)
    -> structure_start {
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    static constexpr std::array<jint, 4> orientations{2, 3, 0, 1};
    const jint orientation =
        orientations[static_cast<std::size_t>(random.next_int(4))];

    auto graph = generate_room_graph(random);
    graph.source->claimed = true;

    const jint west_block = chunk.x * 16 - biome_range_check;
    const jint north_block = chunk.z * 16 - biome_range_check;
    const auto building_box =
        make_bounding_box(west_block, building_y, north_block, orientation,
                          building_width, building_height, building_depth);
    const auto offset = building_pos(building_box, orientation, 9, 0, 22);

    std::vector<structure_piece> children;
    const auto add_room =
        [&](monument_piece_kind kind, const room_definition &room,
            std::array<monument_room_state, 4> states, jint main_design) {
            const auto [width, height, depth] = room_box_size(kind);
            auto bounds =
                make_room_box(orientation, room.index, width, height, depth);
            move_box(bounds, offset.x, offset.y, offset.z);
            states[0] = room_state(room);
            structure_piece piece{
                .type = std::string{piece_type(kind)},
                .bounds = bounds,
                .orientation = orientation,
                .monument =
                    monument_piece_data{
                        .kind = kind,
                        .rooms = states,
                        .main_design = main_design,
                    },
            };
            children.push_back(std::move(piece));
        };

    add_room(monument_piece_kind::entry_room, *graph.source, {}, 0);
    add_room(monument_piece_kind::core_room, *graph.core, {}, 0);

    const auto has_opening = [](const room_definition &room,
                                monument_direction face) {
        return room.has_opening[static_cast<std::size_t>(face)];
    };

    for (auto *room : graph.ordered) {
        if (room->claimed || room->is_special()) {
            continue;
        }
        auto *east_room = room->connections[static_cast<std::size_t>(east)];
        auto *north_room = room->connections[static_cast<std::size_t>(north)];
        auto *up_room = room->connections[static_cast<std::size_t>(up)];

        if (has_opening(*room, east) && !east_room->claimed &&
            has_opening(*room, up) && !up_room->claimed &&
            has_opening(*east_room, up) &&
            !east_room->connections[static_cast<std::size_t>(up)]->claimed) {
            room->claimed = true;
            east_room->claimed = true;
            up_room->claimed = true;
            east_room->connections[static_cast<std::size_t>(up)]->claimed =
                true;
            add_room(
                monument_piece_kind::double_xy_room, *room,
                {room_state(*room), room_state(*east_room),
                 room_state(*up_room),
                 room_state(
                     *east_room->connections[static_cast<std::size_t>(up)])},
                0);
            continue;
        }
        if (has_opening(*room, north) && !north_room->claimed &&
            has_opening(*room, up) && !up_room->claimed &&
            has_opening(*north_room, up) &&
            !north_room->connections[static_cast<std::size_t>(up)]->claimed) {
            room->claimed = true;
            north_room->claimed = true;
            up_room->claimed = true;
            north_room->connections[static_cast<std::size_t>(up)]->claimed =
                true;
            add_room(
                monument_piece_kind::double_yz_room, *room,
                {room_state(*room), room_state(*north_room),
                 room_state(*up_room),
                 room_state(
                     *north_room->connections[static_cast<std::size_t>(up)])},
                0);
            continue;
        }
        if (has_opening(*room, north) && !north_room->claimed) {
            auto *source = room;
            if (!(has_opening(*room, north) && !north_room->claimed)) {
                source = room->connections[static_cast<std::size_t>(south)];
            }
            source->claimed = true;
            source->connections[static_cast<std::size_t>(north)]->claimed =
                true;
            add_room(
                monument_piece_kind::double_z_room, *source,
                {room_state(*source),
                 room_state(
                     *source->connections[static_cast<std::size_t>(north)])},
                0);
            continue;
        }
        if (has_opening(*room, east) && !east_room->claimed) {
            room->claimed = true;
            east_room->claimed = true;
            add_room(monument_piece_kind::double_x_room, *room,
                     {room_state(*room), room_state(*east_room)}, 0);
            continue;
        }
        if (has_opening(*room, up) && !up_room->claimed) {
            room->claimed = true;
            up_room->claimed = true;
            add_room(monument_piece_kind::double_y_room, *room,
                     {room_state(*room), room_state(*up_room)}, 0);
            continue;
        }
        if (!has_opening(*room, west) && !has_opening(*room, east) &&
            !has_opening(*room, north) && !has_opening(*room, south) &&
            !has_opening(*room, up)) {
            room->claimed = true;
            add_room(monument_piece_kind::simple_top_room, *room,
                     {room_state(*room)}, 0);
            continue;
        }
        room->claimed = true;
        add_room(monument_piece_kind::simple_room, *room, {room_state(*room)},
                 random.next_int(3));
    }

    const auto wing_box = [&](jint x0, jint y0, jint z0, jint x1, jint y1,
                              jint z1) {
        const auto a = building_pos(building_box, orientation, x0, y0, z0);
        const auto b = building_pos(building_box, orientation, x1, y1, z1);
        return structure_bbox{.min_x = std::min(a.x, b.x),
                              .min_y = std::min(a.y, b.y),
                              .min_z = std::min(a.z, b.z),
                              .max_x = std::max(a.x, b.x),
                              .max_y = std::max(a.y, b.y),
                              .max_z = std::max(a.z, b.z)};
    };

    const auto left_wing = wing_box(1, 1, 1, 23, 8, 21);
    const auto right_wing = wing_box(34, 1, 1, 56, 8, 21);
    const auto penthouse = wing_box(22, 13, 22, 35, 17, 35);

    const jint wing_random = random.next_int();
    const auto add_special = [&](monument_piece_kind kind,
                                 const structure_bbox &bounds,
                                 jint main_design) {
        children.push_back(structure_piece{
            .type = std::string{piece_type(kind)},
            .bounds = bounds,
            .orientation = orientation,
            .monument =
                monument_piece_data{
                    .kind = kind,
                    .rooms = {},
                    .main_design = main_design,
                },
        });
    };
    add_special(monument_piece_kind::wing_room, left_wing, wing_random & 1);
    add_special(monument_piece_kind::wing_room, right_wing,
                (wing_random + 1) & 1);
    add_special(monument_piece_kind::penthouse, penthouse, 0);

    structure_piece building{
        .type = std::string{piece_type(monument_piece_kind::building)},
        .bounds = building_box,
        .orientation = orientation,
        .monument =
            monument_piece_data{
                .kind = monument_piece_kind::building,
                .rooms = {},
                .main_design = 0,
            },
        .children = children,
    };

    structure_start start{
        .structure = "minecraft:monument",
        .chunk = chunk,
        .references = 0,
        .max_references = 1,
        .bounds = building_box,
    };
    start.pieces.push_back(std::move(building));
    return start;
}

namespace {

constexpr auto block_is(const structure_place_context &context,
                        block_state_id state, std::string_view name) -> bool {
    return state_block_name(context.states->name(state)) == name;
}

struct monument_writer {
    const structure_place_context &context;
    const structure_piece_record &piece;
    block_state_id air;
    block_state_id water;
    block_state_id gray;
    block_state_id light;
    block_state_id black;
    block_state_id lamp;
    block_state_id gold;
    block_state_id sponge;
    jint chunk_min_x;
    jint chunk_max_x;
    jint chunk_min_z;
    jint chunk_max_z;
    jint chunk_min_y;
    jint chunk_max_y;

    monument_writer(const structure_place_context &place_context,
                    const structure_piece_record &place_piece)
        : context{place_context}, piece{place_piece},
          air{resolve("minecraft:air")}, water{resolve("minecraft:water")},
          gray{resolve("minecraft:prismarine")},
          light{resolve("minecraft:prismarine_bricks")},
          black{resolve("minecraft:dark_prismarine")},
          lamp{resolve("minecraft:sea_lantern")},
          gold{resolve("minecraft:gold_block")},
          sponge{resolve("minecraft:wet_sponge")},
          chunk_min_x{context.chunk->chunk_x() * 16},
          chunk_max_x{context.chunk->chunk_x() * 16 + 15},
          chunk_min_z{context.chunk->chunk_z() * 16},
          chunk_max_z{context.chunk->chunk_z() * 16 + 15},
          chunk_min_y{context.chunk->min_y() + 1},
          chunk_max_y{context.chunk->min_y() + context.chunk->height() - 1} {}

    [[nodiscard]] auto resolve(std::string_view key) const -> block_state_id {
        return context.state ? context.state(key) : no_block_state;
    }

    [[nodiscard]] auto world_x(jint x, jint z) const -> jint {
        switch (piece.orientation) {
        case 2: // north
        case 0: // south
            return piece.min_x + x;
        case 1: // west
            return piece.max_x - z;
        default: // east
            return piece.min_x + z;
        }
    }

    [[nodiscard]] auto world_z(jint x, jint z) const -> jint {
        switch (piece.orientation) {
        case 2: // north
            return piece.max_z - z;
        case 0: // south
            return piece.min_z + z;
        case 1: // west
        case 3: // east
            return piece.min_z + x;
        default:
            return piece.min_z + z;
        }
    }

    [[nodiscard]] auto world_y(jint y) const -> jint { return piece.min_y + y; }

    [[nodiscard]] auto inside(jint x, jint y, jint z) const -> bool {
        const jint wx = world_x(x, z);
        const jint wz = world_z(x, z);
        const jint wy = world_y(y);
        return wx >= chunk_min_x && wx <= chunk_max_x && wz >= chunk_min_z &&
               wz <= chunk_max_z && wy >= chunk_min_y && wy <= chunk_max_y;
    }

    [[nodiscard]] auto get(jint x, jint y, jint z) const -> block_state_id {
        if (!inside(x, y, z)) {
            return air;
        }
        return context.region->state_at(world_x(x, z), world_y(y),
                                        world_z(x, z));
    }

    void set(jint x, jint y, jint z, block_state_id state) const {
        if (inside(x, y, z)) {
            (void)context.region->set_block_state(world_x(x, z), world_y(y),
                                                  world_z(x, z), state);
        }
    }

    void box(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
             block_state_id edge, block_state_id fill) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 &&
                        z != z1) {
                        set(x, y, z, fill);
                    } else {
                        set(x, y, z, edge);
                    }
                }
            }
        }
    }

    void water_box(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    const auto current = get(x, y, z);
                    if (block_is(context, current, "minecraft:ice") ||
                        block_is(context, current, "minecraft:packed_ice") ||
                        block_is(context, current, "minecraft:blue_ice") ||
                        block_is(context, current, "minecraft:water")) {
                        continue;
                    }
                    if (world_y(y) >= context.sea_level && current != water) {
                        set(x, y, z, air);
                    } else {
                        set(x, y, z, water);
                    }
                }
            }
        }
    }

    void box_on_fill_only(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
                          block_state_id target) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    if (get(x, y, z) == water) {
                        set(x, y, z, target);
                    }
                }
            }
        }
    }

    [[nodiscard]] auto chunk_intersects(jint x0, jint z0, jint x1,
                                        jint z1) const -> bool {
        const jint wx0 = world_x(x0, z0);
        const jint wz0 = world_z(x0, z0);
        const jint wx1 = world_x(x1, z1);
        const jint wz1 = world_z(x1, z1);
        return chunk_max_x >= std::min(wx0, wx1) &&
               chunk_min_x <= std::max(wx0, wx1) &&
               chunk_max_z >= std::min(wz0, wz1) &&
               chunk_min_z <= std::max(wz0, wz1);
    }

    [[nodiscard]] auto replaceable(block_state_id state) const -> bool {
        const auto traits = context.states->traits(state);
        return traits.air || traits.fluid ||
               block_is(context, state, "minecraft:glow_lichen") ||
               block_is(context, state, "minecraft:seagrass") ||
               block_is(context, state, "minecraft:tall_seagrass");
    }

    void fill_column_down(block_state_id state, jint x, jint y, jint z) const {
        jint wx = world_x(x, z);
        jint wz = world_z(x, z);
        jint wy = world_y(y);
        if (wx < chunk_min_x || wx > chunk_max_x || wz < chunk_min_z ||
            wz > chunk_max_z || wy < chunk_min_y || wy > chunk_max_y) {
            return;
        }
        while (wy > context.chunk->min_y() + 1 &&
               replaceable(context.region->state_at(wx, wy, wz))) {
            (void)context.region->set_block_state(wx, wy, wz, state);
            --wy;
        }
    }
};

void place_default_floor(const monument_writer &writer, jint x_off, jint z_off,
                         bool down_opening) {
    if (down_opening) {
        writer.box(x_off + 0, 0, z_off + 0, x_off + 2, 0, z_off + 7,
                   writer.gray, writer.gray);
        writer.box(x_off + 5, 0, z_off + 0, x_off + 7, 0, z_off + 7,
                   writer.gray, writer.gray);
        writer.box(x_off + 3, 0, z_off + 0, x_off + 4, 0, z_off + 2,
                   writer.gray, writer.gray);
        writer.box(x_off + 3, 0, z_off + 5, x_off + 4, 0, z_off + 7,
                   writer.gray, writer.gray);
        writer.box(x_off + 3, 0, z_off + 2, x_off + 4, 0, z_off + 2,
                   writer.light, writer.light);
        writer.box(x_off + 3, 0, z_off + 5, x_off + 4, 0, z_off + 5,
                   writer.light, writer.light);
        writer.box(x_off + 2, 0, z_off + 3, x_off + 2, 0, z_off + 4,
                   writer.light, writer.light);
        writer.box(x_off + 5, 0, z_off + 3, x_off + 5, 0, z_off + 4,
                   writer.light, writer.light);
    } else {
        writer.box(x_off + 0, 0, z_off + 0, x_off + 7, 0, z_off + 7,
                   writer.gray, writer.gray);
    }
}

void place_entry_room(const monument_writer &writer) {
    const auto &rooms = writer.piece.monument->rooms;
    const auto &own = rooms[0];
    writer.box(0, 3, 0, 2, 3, 7, writer.light, writer.light);
    writer.box(5, 3, 0, 7, 3, 7, writer.light, writer.light);
    writer.box(0, 2, 0, 1, 2, 7, writer.light, writer.light);
    writer.box(6, 2, 0, 7, 2, 7, writer.light, writer.light);
    writer.box(0, 1, 0, 0, 1, 7, writer.light, writer.light);
    writer.box(7, 1, 0, 7, 1, 7, writer.light, writer.light);
    writer.box(0, 1, 7, 7, 3, 7, writer.light, writer.light);
    writer.box(1, 1, 0, 2, 3, 0, writer.light, writer.light);
    writer.box(5, 1, 0, 6, 3, 0, writer.light, writer.light);
    if (own.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(3, 1, 7, 4, 2, 7);
    }
    if (own.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 1, 3, 1, 2, 4);
    }
    if (own.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(6, 1, 3, 7, 2, 4);
    }
}

void place_core_room(const monument_writer &writer) {
    writer.box_on_fill_only(1, 8, 0, 14, 8, 14, writer.gray);
    writer.box(0, 7, 0, 0, 7, 15, writer.light, writer.light);
    writer.box(15, 7, 0, 15, 7, 15, writer.light, writer.light);
    writer.box(1, 7, 0, 15, 7, 0, writer.light, writer.light);
    writer.box(1, 7, 15, 14, 7, 15, writer.light, writer.light);
    for (jint y = 1; y <= 6; ++y) {
        const auto block = (y == 2 || y == 6) ? writer.gray : writer.light;
        for (jint x = 0; x <= 15; x += 15) {
            writer.box(x, y, 0, x, y, 1, block, block);
            writer.box(x, y, 6, x, y, 9, block, block);
            writer.box(x, y, 14, x, y, 15, block, block);
        }
        writer.box(1, y, 0, 1, y, 0, block, block);
        writer.box(6, y, 0, 9, y, 0, block, block);
        writer.box(14, y, 0, 14, y, 0, block, block);
        writer.box(1, y, 15, 14, y, 15, block, block);
    }
    writer.box(6, 3, 6, 9, 6, 9, writer.black, writer.black);
    writer.box(7, 4, 7, 8, 5, 8, writer.gold, writer.gold);
    for (jint y = 3; y <= 6; y += 3) {
        for (jint x = 6; x <= 9; x += 3) {
            writer.set(x, y, 6, writer.lamp);
            writer.set(x, y, 9, writer.lamp);
        }
    }
    writer.box(5, 1, 6, 5, 2, 6, writer.light, writer.light);
    writer.box(5, 1, 9, 5, 2, 9, writer.light, writer.light);
    writer.box(10, 1, 6, 10, 2, 6, writer.light, writer.light);
    writer.box(10, 1, 9, 10, 2, 9, writer.light, writer.light);
    writer.box(6, 1, 5, 6, 2, 5, writer.light, writer.light);
    writer.box(9, 1, 5, 9, 2, 5, writer.light, writer.light);
    writer.box(6, 1, 10, 6, 2, 10, writer.light, writer.light);
    writer.box(9, 1, 10, 9, 2, 10, writer.light, writer.light);
    writer.box(5, 2, 5, 5, 6, 5, writer.light, writer.light);
    writer.box(5, 2, 10, 5, 6, 10, writer.light, writer.light);
    writer.box(10, 2, 5, 10, 6, 5, writer.light, writer.light);
    writer.box(10, 2, 10, 10, 6, 10, writer.light, writer.light);
    writer.box(5, 7, 1, 5, 7, 6, writer.light, writer.light);
    writer.box(10, 7, 1, 10, 7, 6, writer.light, writer.light);
    writer.box(5, 7, 9, 5, 7, 14, writer.light, writer.light);
    writer.box(10, 7, 9, 10, 7, 14, writer.light, writer.light);
    writer.box(1, 7, 5, 6, 7, 5, writer.light, writer.light);
    writer.box(1, 7, 10, 6, 7, 10, writer.light, writer.light);
    writer.box(9, 7, 5, 14, 7, 5, writer.light, writer.light);
    writer.box(9, 7, 10, 14, 7, 10, writer.light, writer.light);
    writer.box(2, 1, 2, 2, 1, 3, writer.light, writer.light);
    writer.box(3, 1, 2, 3, 1, 2, writer.light, writer.light);
    writer.box(13, 1, 2, 13, 1, 3, writer.light, writer.light);
    writer.box(12, 1, 2, 12, 1, 2, writer.light, writer.light);
    writer.box(2, 1, 12, 2, 1, 13, writer.light, writer.light);
    writer.box(3, 1, 13, 3, 1, 13, writer.light, writer.light);
    writer.box(13, 1, 12, 13, 1, 13, writer.light, writer.light);
    writer.box(12, 1, 13, 12, 1, 13, writer.light, writer.light);
}

void place_double_x_room(const monument_writer &writer) {
    const auto &rooms = writer.piece.monument->rooms;
    const auto &west_room = rooms[0];
    const auto &east_room = rooms[1];
    if (west_room.index / 25 > 0) {
        place_default_floor(writer, 8, 0,
                            east_room.openings[static_cast<std::size_t>(down)]);
        place_default_floor(writer, 0, 0,
                            west_room.openings[static_cast<std::size_t>(down)]);
    }
    if (!west_room.up_connected) {
        writer.box_on_fill_only(1, 4, 1, 7, 4, 6, writer.gray);
    }
    if (!east_room.up_connected) {
        writer.box_on_fill_only(8, 4, 1, 14, 4, 6, writer.gray);
    }
    writer.box(0, 3, 0, 0, 3, 7, writer.light, writer.light);
    writer.box(15, 3, 0, 15, 3, 7, writer.light, writer.light);
    writer.box(1, 3, 0, 15, 3, 0, writer.light, writer.light);
    writer.box(1, 3, 7, 14, 3, 7, writer.light, writer.light);
    writer.box(0, 2, 0, 0, 2, 7, writer.gray, writer.gray);
    writer.box(15, 2, 0, 15, 2, 7, writer.gray, writer.gray);
    writer.box(1, 2, 0, 15, 2, 0, writer.gray, writer.gray);
    writer.box(1, 2, 7, 14, 2, 7, writer.gray, writer.gray);
    writer.box(0, 1, 0, 0, 1, 7, writer.light, writer.light);
    writer.box(15, 1, 0, 15, 1, 7, writer.light, writer.light);
    writer.box(1, 1, 0, 15, 1, 0, writer.light, writer.light);
    writer.box(1, 1, 7, 14, 1, 7, writer.light, writer.light);
    writer.box(5, 1, 0, 10, 1, 4, writer.light, writer.light);
    writer.box(6, 2, 0, 9, 2, 3, writer.gray, writer.gray);
    writer.box(5, 3, 0, 10, 3, 4, writer.light, writer.light);
    writer.set(6, 2, 3, writer.lamp);
    writer.set(9, 2, 3, writer.lamp);
    if (west_room.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(3, 1, 0, 4, 2, 0);
    }
    if (west_room.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(3, 1, 7, 4, 2, 7);
    }
    if (west_room.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 1, 3, 0, 2, 4);
    }
    if (east_room.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(11, 1, 0, 12, 2, 0);
    }
    if (east_room.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(11, 1, 7, 12, 2, 7);
    }
    if (east_room.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(15, 1, 3, 15, 2, 4);
    }
}

void place_double_xy_room(const monument_writer &writer) {
    const auto &rooms = writer.piece.monument->rooms;
    const auto &west_room = rooms[0];
    const auto &east_room = rooms[1];
    const auto &west_up = rooms[2];
    const auto &east_up = rooms[3];
    if (west_room.index / 25 > 0) {
        place_default_floor(writer, 8, 0,
                            east_room.openings[static_cast<std::size_t>(down)]);
        place_default_floor(writer, 0, 0,
                            west_room.openings[static_cast<std::size_t>(down)]);
    }
    if (!west_up.up_connected) {
        writer.box_on_fill_only(1, 8, 1, 7, 8, 6, writer.gray);
    }
    if (!east_up.up_connected) {
        writer.box_on_fill_only(8, 8, 1, 14, 8, 6, writer.gray);
    }
    for (jint y = 1; y <= 7; ++y) {
        const auto block = (y == 2 || y == 6) ? writer.gray : writer.light;
        writer.box(0, y, 0, 0, y, 7, block, block);
        writer.box(15, y, 0, 15, y, 7, block, block);
        writer.box(1, y, 0, 15, y, 0, block, block);
        writer.box(1, y, 7, 14, y, 7, block, block);
    }
    writer.box(2, 1, 3, 2, 7, 4, writer.light, writer.light);
    writer.box(3, 1, 2, 4, 7, 2, writer.light, writer.light);
    writer.box(3, 1, 5, 4, 7, 5, writer.light, writer.light);
    writer.box(13, 1, 3, 13, 7, 4, writer.light, writer.light);
    writer.box(11, 1, 2, 12, 7, 2, writer.light, writer.light);
    writer.box(11, 1, 5, 12, 7, 5, writer.light, writer.light);
    writer.box(5, 1, 3, 5, 3, 4, writer.light, writer.light);
    writer.box(10, 1, 3, 10, 3, 4, writer.light, writer.light);
    writer.box(5, 7, 2, 10, 7, 5, writer.light, writer.light);
    writer.box(5, 5, 2, 5, 7, 2, writer.light, writer.light);
    writer.box(10, 5, 2, 10, 7, 2, writer.light, writer.light);
    writer.box(5, 5, 5, 5, 7, 5, writer.light, writer.light);
    writer.box(10, 5, 5, 10, 7, 5, writer.light, writer.light);
    writer.set(6, 6, 2, writer.light);
    writer.set(9, 6, 2, writer.light);
    writer.set(6, 6, 5, writer.light);
    writer.set(9, 6, 5, writer.light);
    writer.box(5, 4, 3, 6, 4, 4, writer.light, writer.light);
    writer.box(9, 4, 3, 10, 4, 4, writer.light, writer.light);
    writer.set(5, 4, 2, writer.lamp);
    writer.set(5, 4, 5, writer.lamp);
    writer.set(10, 4, 2, writer.lamp);
    writer.set(10, 4, 5, writer.lamp);
    if (west_room.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(3, 1, 0, 4, 2, 0);
    }
    if (west_room.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(3, 1, 7, 4, 2, 7);
    }
    if (west_room.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 1, 3, 0, 2, 4);
    }
    if (east_room.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(11, 1, 0, 12, 2, 0);
    }
    if (east_room.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(11, 1, 7, 12, 2, 7);
    }
    if (east_room.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(15, 1, 3, 15, 2, 4);
    }
    if (west_up.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(3, 5, 0, 4, 6, 0);
    }
    if (west_up.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(3, 5, 7, 4, 6, 7);
    }
    if (west_up.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 5, 3, 0, 6, 4);
    }
    if (east_up.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(11, 5, 0, 12, 6, 0);
    }
    if (east_up.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(11, 5, 7, 12, 6, 7);
    }
    if (east_up.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(15, 5, 3, 15, 6, 4);
    }
}

void place_double_y_room(const monument_writer &writer) {
    const auto &rooms = writer.piece.monument->rooms;
    const auto &own = rooms[0];
    const auto &above = rooms[1];
    if (own.index / 25 > 0) {
        place_default_floor(writer, 0, 0,
                            own.openings[static_cast<std::size_t>(down)]);
    }
    if (!above.up_connected) {
        writer.box_on_fill_only(1, 8, 1, 6, 8, 6, writer.gray);
    }
    writer.box(0, 4, 0, 0, 4, 7, writer.light, writer.light);
    writer.box(7, 4, 0, 7, 4, 7, writer.light, writer.light);
    writer.box(1, 4, 0, 6, 4, 0, writer.light, writer.light);
    writer.box(1, 4, 7, 6, 4, 7, writer.light, writer.light);
    writer.box(2, 4, 1, 2, 4, 2, writer.light, writer.light);
    writer.box(1, 4, 2, 1, 4, 2, writer.light, writer.light);
    writer.box(5, 4, 1, 5, 4, 2, writer.light, writer.light);
    writer.box(6, 4, 2, 6, 4, 2, writer.light, writer.light);
    writer.box(2, 4, 5, 2, 4, 6, writer.light, writer.light);
    writer.box(1, 4, 5, 1, 4, 5, writer.light, writer.light);
    writer.box(5, 4, 5, 5, 4, 6, writer.light, writer.light);
    writer.box(6, 4, 5, 6, 4, 5, writer.light, writer.light);

    const monument_room_state *definition = &own;
    for (jint y = 1; y <= 5; y += 4) {
        const jint z = 0;
        if (definition->openings[static_cast<std::size_t>(south)]) {
            writer.box(2, y, z, 2, y + 2, z, writer.light, writer.light);
            writer.box(5, y, z, 5, y + 2, z, writer.light, writer.light);
            writer.box(3, y + 2, z, 4, y + 2, z, writer.light, writer.light);
        } else {
            writer.box(0, y, z, 7, y + 2, z, writer.light, writer.light);
            writer.box(0, y + 1, z, 7, y + 1, z, writer.gray, writer.gray);
        }
        const jint far_z = 7;
        if (definition->openings[static_cast<std::size_t>(north)]) {
            writer.box(2, y, far_z, 2, y + 2, far_z, writer.light,
                       writer.light);
            writer.box(5, y, far_z, 5, y + 2, far_z, writer.light,
                       writer.light);
            writer.box(3, y + 2, far_z, 4, y + 2, far_z, writer.light,
                       writer.light);
        } else {
            writer.box(0, y, far_z, 7, y + 2, far_z, writer.light,
                       writer.light);
            writer.box(0, y + 1, far_z, 7, y + 1, far_z, writer.gray,
                       writer.gray);
        }
        const jint x = 0;
        if (definition->openings[static_cast<std::size_t>(west)]) {
            writer.box(x, y, 2, x, y + 2, 2, writer.light, writer.light);
            writer.box(x, y, 5, x, y + 2, 5, writer.light, writer.light);
            writer.box(x, y + 2, 3, x, y + 2, 4, writer.light, writer.light);
        } else {
            writer.box(x, y, 0, x, y + 2, 7, writer.light, writer.light);
            writer.box(x, y + 1, 0, x, y + 1, 7, writer.gray, writer.gray);
        }
        const jint far_x = 7;
        if (definition->openings[static_cast<std::size_t>(east)]) {
            writer.box(far_x, y, 2, far_x, y + 2, 2, writer.light,
                       writer.light);
            writer.box(far_x, y, 5, far_x, y + 2, 5, writer.light,
                       writer.light);
            writer.box(far_x, y + 2, 3, far_x, y + 2, 4, writer.light,
                       writer.light);
        } else {
            writer.box(far_x, y, 0, far_x, y + 2, 7, writer.light,
                       writer.light);
            writer.box(far_x, y + 1, 0, far_x, y + 1, 7, writer.gray,
                       writer.gray);
        }
        definition = &above;
    }
}

void place_double_yz_room(const monument_writer &writer) {
    const auto &rooms = writer.piece.monument->rooms;
    const auto &south_room = rooms[0];
    const auto &north_room = rooms[1];
    const auto &south_up = rooms[2];
    const auto &north_up = rooms[3];
    if (south_room.index / 25 > 0) {
        place_default_floor(
            writer, 0, 8, north_room.openings[static_cast<std::size_t>(down)]);
        place_default_floor(
            writer, 0, 0, south_room.openings[static_cast<std::size_t>(down)]);
    }
    if (!south_up.up_connected) {
        writer.box_on_fill_only(1, 8, 1, 6, 8, 7, writer.gray);
    }
    if (!north_up.up_connected) {
        writer.box_on_fill_only(1, 8, 8, 6, 8, 14, writer.gray);
    }
    for (jint y = 1; y <= 7; ++y) {
        const auto block = (y == 2 || y == 6) ? writer.gray : writer.light;
        writer.box(0, y, 0, 0, y, 15, block, block);
        writer.box(7, y, 0, 7, y, 15, block, block);
        writer.box(1, y, 0, 6, y, 0, block, block);
        writer.box(1, y, 15, 6, y, 15, block, block);
    }
    for (jint y = 1; y <= 7; ++y) {
        const auto block = (y == 2 || y == 6) ? writer.lamp : writer.black;
        writer.box(3, y, 7, 4, y, 8, block, block);
    }
    if (south_room.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(3, 1, 0, 4, 2, 0);
    }
    if (south_room.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(7, 1, 3, 7, 2, 4);
    }
    if (south_room.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 1, 3, 0, 2, 4);
    }
    if (north_room.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(3, 1, 15, 4, 2, 15);
    }
    if (north_room.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 1, 11, 0, 2, 12);
    }
    if (north_room.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(7, 1, 11, 7, 2, 12);
    }
    if (south_up.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(3, 5, 0, 4, 6, 0);
    }
    if (south_up.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(7, 5, 3, 7, 6, 4);
        writer.box(5, 4, 2, 6, 4, 5, writer.light, writer.light);
        writer.box(6, 1, 2, 6, 3, 2, writer.light, writer.light);
        writer.box(6, 1, 5, 6, 3, 5, writer.light, writer.light);
    }
    if (south_up.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 5, 3, 0, 6, 4);
        writer.box(1, 4, 2, 2, 4, 5, writer.light, writer.light);
        writer.box(1, 1, 2, 1, 3, 2, writer.light, writer.light);
        writer.box(1, 1, 5, 1, 3, 5, writer.light, writer.light);
    }
    if (north_up.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(3, 5, 15, 4, 6, 15);
    }
    if (north_up.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 5, 11, 0, 6, 12);
        writer.box(1, 4, 10, 2, 4, 13, writer.light, writer.light);
        writer.box(1, 1, 10, 1, 3, 10, writer.light, writer.light);
        writer.box(1, 1, 13, 1, 3, 13, writer.light, writer.light);
    }
    if (north_up.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(7, 5, 11, 7, 6, 12);
        writer.box(5, 4, 10, 6, 4, 13, writer.light, writer.light);
        writer.box(6, 1, 10, 6, 3, 10, writer.light, writer.light);
        writer.box(6, 1, 13, 6, 3, 13, writer.light, writer.light);
    }
}

void place_double_z_room(const monument_writer &writer) {
    const auto &rooms = writer.piece.monument->rooms;
    const auto &south_room = rooms[0];
    const auto &north_room = rooms[1];
    if (south_room.index / 25 > 0) {
        place_default_floor(
            writer, 0, 8, north_room.openings[static_cast<std::size_t>(down)]);
        place_default_floor(
            writer, 0, 0, south_room.openings[static_cast<std::size_t>(down)]);
    }
    if (!south_room.up_connected) {
        writer.box_on_fill_only(1, 4, 1, 6, 4, 7, writer.gray);
    }
    if (!north_room.up_connected) {
        writer.box_on_fill_only(1, 4, 8, 6, 4, 14, writer.gray);
    }
    writer.box(0, 3, 0, 0, 3, 15, writer.light, writer.light);
    writer.box(7, 3, 0, 7, 3, 15, writer.light, writer.light);
    writer.box(1, 3, 0, 7, 3, 0, writer.light, writer.light);
    writer.box(1, 3, 15, 6, 3, 15, writer.light, writer.light);
    writer.box(0, 2, 0, 0, 2, 15, writer.gray, writer.gray);
    writer.box(7, 2, 0, 7, 2, 15, writer.gray, writer.gray);
    writer.box(1, 2, 0, 7, 2, 0, writer.gray, writer.gray);
    writer.box(1, 2, 15, 6, 2, 15, writer.gray, writer.gray);
    writer.box(0, 1, 0, 0, 1, 15, writer.light, writer.light);
    writer.box(7, 1, 0, 7, 1, 15, writer.light, writer.light);
    writer.box(1, 1, 0, 7, 1, 0, writer.light, writer.light);
    writer.box(1, 1, 15, 6, 1, 15, writer.light, writer.light);
    writer.box(1, 1, 1, 1, 1, 2, writer.light, writer.light);
    writer.box(6, 1, 1, 6, 1, 2, writer.light, writer.light);
    writer.box(1, 3, 1, 1, 3, 2, writer.light, writer.light);
    writer.box(6, 3, 1, 6, 3, 2, writer.light, writer.light);
    writer.box(1, 1, 13, 1, 1, 14, writer.light, writer.light);
    writer.box(6, 1, 13, 6, 1, 14, writer.light, writer.light);
    writer.box(1, 3, 13, 1, 3, 14, writer.light, writer.light);
    writer.box(6, 3, 13, 6, 3, 14, writer.light, writer.light);
    writer.box(2, 1, 6, 2, 3, 6, writer.light, writer.light);
    writer.box(5, 1, 6, 5, 3, 6, writer.light, writer.light);
    writer.box(2, 1, 9, 2, 3, 9, writer.light, writer.light);
    writer.box(5, 1, 9, 5, 3, 9, writer.light, writer.light);
    writer.box(3, 2, 6, 4, 2, 6, writer.light, writer.light);
    writer.box(3, 2, 9, 4, 2, 9, writer.light, writer.light);
    writer.box(2, 2, 7, 2, 2, 8, writer.light, writer.light);
    writer.box(5, 2, 7, 5, 2, 8, writer.light, writer.light);
    writer.set(2, 2, 5, writer.lamp);
    writer.set(5, 2, 5, writer.lamp);
    writer.set(2, 2, 10, writer.lamp);
    writer.set(5, 2, 10, writer.lamp);
    writer.set(2, 3, 5, writer.light);
    writer.set(5, 3, 5, writer.light);
    writer.set(2, 3, 10, writer.light);
    writer.set(5, 3, 10, writer.light);
    if (south_room.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(3, 1, 0, 4, 2, 0);
    }
    if (south_room.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(7, 1, 3, 7, 2, 4);
    }
    if (south_room.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 1, 3, 0, 2, 4);
    }
    if (north_room.openings[static_cast<std::size_t>(north)]) {
        writer.water_box(3, 1, 15, 4, 2, 15);
    }
    if (north_room.openings[static_cast<std::size_t>(west)]) {
        writer.water_box(0, 1, 11, 0, 2, 12);
    }
    if (north_room.openings[static_cast<std::size_t>(east)]) {
        writer.water_box(7, 1, 11, 7, 2, 12);
    }
}

void place_simple_top_room(const monument_writer &writer,
                           worldgen_random &random) {
    const auto &own = writer.piece.monument->rooms[0];
    if (own.index / 25 > 0) {
        place_default_floor(writer, 0, 0,
                            own.openings[static_cast<std::size_t>(down)]);
    }
    if (!own.up_connected) {
        writer.box_on_fill_only(1, 4, 1, 6, 4, 6, writer.gray);
    }
    for (jint x = 1; x <= 6; ++x) {
        for (jint z = 1; z <= 6; ++z) {
            if (random.next_int(3) == 0) {
                continue;
            }
            const jint bottom =
                2 + (random.next_int(4) == 0 ? jint{0} : jint{1});
            writer.box(x, bottom, z, x, 3, z, writer.sponge, writer.sponge);
        }
    }
    writer.box(0, 1, 0, 0, 1, 7, writer.light, writer.light);
    writer.box(7, 1, 0, 7, 1, 7, writer.light, writer.light);
    writer.box(1, 1, 0, 6, 1, 0, writer.light, writer.light);
    writer.box(1, 1, 7, 6, 1, 7, writer.light, writer.light);
    writer.box(0, 2, 0, 0, 2, 7, writer.black, writer.black);
    writer.box(7, 2, 0, 7, 2, 7, writer.black, writer.black);
    writer.box(1, 2, 0, 6, 2, 0, writer.black, writer.black);
    writer.box(1, 2, 7, 6, 2, 7, writer.black, writer.black);
    writer.box(0, 3, 0, 0, 3, 7, writer.light, writer.light);
    writer.box(7, 3, 0, 7, 3, 7, writer.light, writer.light);
    writer.box(1, 3, 0, 6, 3, 0, writer.light, writer.light);
    writer.box(1, 3, 7, 6, 3, 7, writer.light, writer.light);
    writer.box(0, 1, 3, 0, 2, 4, writer.black, writer.black);
    writer.box(7, 1, 3, 7, 2, 4, writer.black, writer.black);
    writer.box(3, 1, 0, 4, 2, 0, writer.black, writer.black);
    writer.box(3, 1, 7, 4, 2, 7, writer.black, writer.black);
    if (own.openings[static_cast<std::size_t>(south)]) {
        writer.water_box(3, 1, 0, 4, 2, 0);
    }
}

void place_simple_room(const monument_writer &writer, worldgen_random &random) {
    const auto &own = writer.piece.monument->rooms[0];
    const jint main_design = writer.piece.monument->main_design;
    if (own.index / 25 > 0) {
        place_default_floor(writer, 0, 0,
                            own.openings[static_cast<std::size_t>(down)]);
    }
    if (!own.up_connected) {
        writer.box_on_fill_only(1, 4, 1, 6, 4, 6, writer.gray);
    }
    bool center_pillar = false;
    if (main_design != 0 && random.next_boolean() &&
        !own.openings[static_cast<std::size_t>(down)] &&
        !own.openings[static_cast<std::size_t>(up)] &&
        own.count_openings() > 1) {
        center_pillar = true;
    }
    if (main_design == 0) {
        writer.box(0, 1, 0, 2, 1, 2, writer.light, writer.light);
        writer.box(0, 3, 0, 2, 3, 2, writer.light, writer.light);
        writer.box(0, 2, 0, 0, 2, 2, writer.gray, writer.gray);
        writer.box(1, 2, 0, 2, 2, 0, writer.gray, writer.gray);
        writer.set(1, 2, 1, writer.lamp);
        writer.box(5, 1, 0, 7, 1, 2, writer.light, writer.light);
        writer.box(5, 3, 0, 7, 3, 2, writer.light, writer.light);
        writer.box(7, 2, 0, 7, 2, 2, writer.gray, writer.gray);
        writer.box(5, 2, 0, 6, 2, 0, writer.gray, writer.gray);
        writer.set(6, 2, 1, writer.lamp);
        writer.box(0, 1, 5, 2, 1, 7, writer.light, writer.light);
        writer.box(0, 3, 5, 2, 3, 7, writer.light, writer.light);
        writer.box(0, 2, 5, 0, 2, 7, writer.gray, writer.gray);
        writer.box(1, 2, 7, 2, 2, 7, writer.gray, writer.gray);
        writer.set(1, 2, 6, writer.lamp);
        writer.box(5, 1, 5, 7, 1, 7, writer.light, writer.light);
        writer.box(5, 3, 5, 7, 3, 7, writer.light, writer.light);
        writer.box(7, 2, 5, 7, 2, 7, writer.gray, writer.gray);
        writer.box(5, 2, 7, 6, 2, 7, writer.gray, writer.gray);
        writer.set(6, 2, 6, writer.lamp);
        if (own.openings[static_cast<std::size_t>(south)]) {
            writer.box(3, 3, 0, 4, 3, 0, writer.light, writer.light);
        } else {
            writer.box(3, 3, 0, 4, 3, 1, writer.light, writer.light);
            writer.box(3, 2, 0, 4, 2, 0, writer.gray, writer.gray);
            writer.box(3, 1, 0, 4, 1, 1, writer.light, writer.light);
        }
        if (own.openings[static_cast<std::size_t>(north)]) {
            writer.box(3, 3, 7, 4, 3, 7, writer.light, writer.light);
        } else {
            writer.box(3, 3, 6, 4, 3, 7, writer.light, writer.light);
            writer.box(3, 2, 7, 4, 2, 7, writer.gray, writer.gray);
            writer.box(3, 1, 6, 4, 1, 7, writer.light, writer.light);
        }
        if (own.openings[static_cast<std::size_t>(west)]) {
            writer.box(0, 3, 3, 0, 3, 4, writer.light, writer.light);
        } else {
            writer.box(0, 3, 3, 1, 3, 4, writer.light, writer.light);
            writer.box(0, 2, 3, 0, 2, 4, writer.gray, writer.gray);
            writer.box(0, 1, 3, 1, 1, 4, writer.light, writer.light);
        }
        if (own.openings[static_cast<std::size_t>(east)]) {
            writer.box(7, 3, 3, 7, 3, 4, writer.light, writer.light);
        } else {
            writer.box(6, 3, 3, 7, 3, 4, writer.light, writer.light);
            writer.box(7, 2, 3, 7, 2, 4, writer.gray, writer.gray);
            writer.box(6, 1, 3, 7, 1, 4, writer.light, writer.light);
        }
    } else if (main_design == 1) {
        writer.box(2, 1, 2, 2, 3, 2, writer.light, writer.light);
        writer.box(2, 1, 5, 2, 3, 5, writer.light, writer.light);
        writer.box(5, 1, 5, 5, 3, 5, writer.light, writer.light);
        writer.box(5, 1, 2, 5, 3, 2, writer.light, writer.light);
        writer.set(2, 2, 2, writer.lamp);
        writer.set(2, 2, 5, writer.lamp);
        writer.set(5, 2, 5, writer.lamp);
        writer.set(5, 2, 2, writer.lamp);
        writer.box(0, 1, 0, 1, 3, 0, writer.light, writer.light);
        writer.box(0, 1, 1, 0, 3, 1, writer.light, writer.light);
        writer.box(0, 1, 7, 1, 3, 7, writer.light, writer.light);
        writer.box(0, 1, 6, 0, 3, 6, writer.light, writer.light);
        writer.box(6, 1, 7, 7, 3, 7, writer.light, writer.light);
        writer.box(7, 1, 6, 7, 3, 6, writer.light, writer.light);
        writer.box(6, 1, 0, 7, 3, 0, writer.light, writer.light);
        writer.box(7, 1, 1, 7, 3, 1, writer.light, writer.light);
        writer.set(1, 2, 0, writer.gray);
        writer.set(0, 2, 1, writer.gray);
        writer.set(1, 2, 7, writer.gray);
        writer.set(0, 2, 6, writer.gray);
        writer.set(6, 2, 7, writer.gray);
        writer.set(7, 2, 6, writer.gray);
        writer.set(6, 2, 0, writer.gray);
        writer.set(7, 2, 1, writer.gray);
        if (!own.openings[static_cast<std::size_t>(south)]) {
            writer.box(1, 3, 0, 6, 3, 0, writer.light, writer.light);
            writer.box(1, 2, 0, 6, 2, 0, writer.gray, writer.gray);
            writer.box(1, 1, 0, 6, 1, 0, writer.light, writer.light);
        }
        if (!own.openings[static_cast<std::size_t>(north)]) {
            writer.box(1, 3, 7, 6, 3, 7, writer.light, writer.light);
            writer.box(1, 2, 7, 6, 2, 7, writer.gray, writer.gray);
            writer.box(1, 1, 7, 6, 1, 7, writer.light, writer.light);
        }
        if (!own.openings[static_cast<std::size_t>(west)]) {
            writer.box(0, 3, 1, 0, 3, 6, writer.light, writer.light);
            writer.box(0, 2, 1, 0, 2, 6, writer.gray, writer.gray);
            writer.box(0, 1, 1, 0, 1, 6, writer.light, writer.light);
        }
        if (!own.openings[static_cast<std::size_t>(east)]) {
            writer.box(7, 3, 1, 7, 3, 6, writer.light, writer.light);
            writer.box(7, 2, 1, 7, 2, 6, writer.gray, writer.gray);
            writer.box(7, 1, 1, 7, 1, 6, writer.light, writer.light);
        }
    } else if (main_design == 2) {
        writer.box(0, 1, 0, 0, 1, 7, writer.light, writer.light);
        writer.box(7, 1, 0, 7, 1, 7, writer.light, writer.light);
        writer.box(1, 1, 0, 6, 1, 0, writer.light, writer.light);
        writer.box(1, 1, 7, 6, 1, 7, writer.light, writer.light);
        writer.box(0, 2, 0, 0, 2, 7, writer.black, writer.black);
        writer.box(7, 2, 0, 7, 2, 7, writer.black, writer.black);
        writer.box(1, 2, 0, 6, 2, 0, writer.black, writer.black);
        writer.box(1, 2, 7, 6, 2, 7, writer.black, writer.black);
        writer.box(0, 3, 0, 0, 3, 7, writer.light, writer.light);
        writer.box(7, 3, 0, 7, 3, 7, writer.light, writer.light);
        writer.box(1, 3, 0, 6, 3, 0, writer.light, writer.light);
        writer.box(1, 3, 7, 6, 3, 7, writer.light, writer.light);
        writer.box(0, 1, 3, 0, 2, 4, writer.black, writer.black);
        writer.box(7, 1, 3, 7, 2, 4, writer.black, writer.black);
        writer.box(3, 1, 0, 4, 2, 0, writer.black, writer.black);
        writer.box(3, 1, 7, 4, 2, 7, writer.black, writer.black);
        if (own.openings[static_cast<std::size_t>(south)]) {
            writer.water_box(3, 1, 0, 4, 2, 0);
        }
        if (own.openings[static_cast<std::size_t>(north)]) {
            writer.water_box(3, 1, 7, 4, 2, 7);
        }
        if (own.openings[static_cast<std::size_t>(west)]) {
            writer.water_box(0, 1, 3, 0, 2, 4);
        }
        if (own.openings[static_cast<std::size_t>(east)]) {
            writer.water_box(7, 1, 3, 7, 2, 4);
        }
    }
    if (center_pillar) {
        writer.box(3, 1, 3, 4, 1, 4, writer.light, writer.light);
        writer.box(3, 2, 3, 4, 2, 4, writer.gray, writer.gray);
        writer.box(3, 3, 3, 4, 3, 4, writer.light, writer.light);
    }
}

void place_penthouse(const monument_writer &writer) {
    writer.box(2, -1, 2, 11, -1, 11, writer.light, writer.light);
    writer.box(0, -1, 0, 1, -1, 11, writer.gray, writer.gray);
    writer.box(12, -1, 0, 13, -1, 11, writer.gray, writer.gray);
    writer.box(2, -1, 0, 11, -1, 1, writer.gray, writer.gray);
    writer.box(2, -1, 12, 11, -1, 13, writer.gray, writer.gray);
    writer.box(0, 0, 0, 0, 0, 13, writer.light, writer.light);
    writer.box(13, 0, 0, 13, 0, 13, writer.light, writer.light);
    writer.box(1, 0, 0, 12, 0, 0, writer.light, writer.light);
    writer.box(1, 0, 13, 12, 0, 13, writer.light, writer.light);
    for (jint i = 2; i <= 11; i += 3) {
        writer.set(0, 0, i, writer.lamp);
        writer.set(13, 0, i, writer.lamp);
        writer.set(i, 0, 0, writer.lamp);
    }
    writer.box(2, 0, 3, 4, 0, 9, writer.light, writer.light);
    writer.box(9, 0, 3, 11, 0, 9, writer.light, writer.light);
    writer.box(4, 0, 9, 9, 0, 11, writer.light, writer.light);
    writer.set(5, 0, 8, writer.light);
    writer.set(8, 0, 8, writer.light);
    writer.set(10, 0, 10, writer.light);
    writer.set(3, 0, 10, writer.light);
    writer.box(3, 0, 3, 3, 0, 7, writer.black, writer.black);
    writer.box(10, 0, 3, 10, 0, 7, writer.black, writer.black);
    writer.box(6, 0, 10, 7, 0, 10, writer.black, writer.black);
    jint x = 3;
    for (jint i = 0; i < 2; ++i) {
        for (jint z = 2; z <= 8; z += 3) {
            writer.box(x, 0, z, x, 2, z, writer.light, writer.light);
        }
        x = 10;
    }
    writer.box(5, 0, 10, 5, 2, 10, writer.light, writer.light);
    writer.box(8, 0, 10, 8, 2, 10, writer.light, writer.light);
    writer.box(6, -1, 7, 7, -1, 8, writer.black, writer.black);
    writer.water_box(6, -1, 3, 7, -1, 4);
}

void place_wing_room(const monument_writer &writer, worldgen_random &random) {
    (void)random;
    const jint main_design = writer.piece.monument->main_design;
    if (main_design == 0) {
        for (jint i = 0; i < 4; ++i) {
            writer.box(10 - i, 3 - i, 20 - i, 12 + i, 3 - i, 20, writer.light,
                       writer.light);
        }
        writer.box(7, 0, 6, 15, 0, 16, writer.light, writer.light);
        writer.box(6, 0, 6, 6, 3, 20, writer.light, writer.light);
        writer.box(16, 0, 6, 16, 3, 20, writer.light, writer.light);
        writer.box(7, 1, 7, 7, 1, 20, writer.light, writer.light);
        writer.box(15, 1, 7, 15, 1, 20, writer.light, writer.light);
        writer.box(7, 1, 6, 9, 3, 6, writer.light, writer.light);
        writer.box(13, 1, 6, 15, 3, 6, writer.light, writer.light);
        writer.box(8, 1, 7, 9, 1, 7, writer.light, writer.light);
        writer.box(13, 1, 7, 14, 1, 7, writer.light, writer.light);
        writer.box(9, 0, 5, 13, 0, 5, writer.light, writer.light);
        writer.box(10, 0, 7, 12, 0, 7, writer.black, writer.black);
        writer.box(8, 0, 10, 8, 0, 12, writer.black, writer.black);
        writer.box(14, 0, 10, 14, 0, 12, writer.black, writer.black);
        for (jint z = 18; z >= 7; z -= 3) {
            writer.set(6, 3, z, writer.lamp);
            writer.set(16, 3, z, writer.lamp);
        }
        writer.set(10, 0, 10, writer.lamp);
        writer.set(12, 0, 10, writer.lamp);
        writer.set(10, 0, 12, writer.lamp);
        writer.set(12, 0, 12, writer.lamp);
        writer.set(8, 3, 6, writer.lamp);
        writer.set(14, 3, 6, writer.lamp);
        writer.set(4, 2, 4, writer.light);
        writer.set(4, 1, 4, writer.lamp);
        writer.set(4, 0, 4, writer.light);
        writer.set(18, 2, 4, writer.light);
        writer.set(18, 1, 4, writer.lamp);
        writer.set(18, 0, 4, writer.light);
        writer.set(4, 2, 18, writer.light);
        writer.set(4, 1, 18, writer.lamp);
        writer.set(4, 0, 18, writer.light);
        writer.set(18, 2, 18, writer.light);
        writer.set(18, 1, 18, writer.lamp);
        writer.set(18, 0, 18, writer.light);
        writer.set(9, 7, 20, writer.light);
        writer.set(13, 7, 20, writer.light);
        writer.box(6, 0, 21, 7, 4, 21, writer.light, writer.light);
        writer.box(15, 0, 21, 16, 4, 21, writer.light, writer.light);
    } else if (main_design == 1) {
        writer.box(9, 3, 18, 13, 3, 20, writer.light, writer.light);
        writer.box(9, 0, 18, 9, 2, 18, writer.light, writer.light);
        writer.box(13, 0, 18, 13, 2, 18, writer.light, writer.light);
        jint x = 9;
        for (jint i = 0; i < 2; ++i) {
            writer.set(x, 6, 20, writer.light);
            writer.set(x, 5, 20, writer.lamp);
            writer.set(x, 4, 20, writer.light);
            x = 13;
        }
        writer.box(7, 3, 7, 15, 3, 14, writer.light, writer.light);
        jint wall = 10;
        for (jint i = 0; i < 2; ++i) {
            writer.box(wall, 0, 10, wall, 6, 10, writer.light, writer.light);
            writer.box(wall, 0, 12, wall, 6, 12, writer.light, writer.light);
            writer.set(wall, 0, 10, writer.lamp);
            writer.set(wall, 0, 12, writer.lamp);
            writer.set(wall, 4, 10, writer.lamp);
            writer.set(wall, 4, 12, writer.lamp);
            wall = 12;
        }
        jint post = 8;
        for (jint i = 0; i < 2; ++i) {
            writer.box(post, 0, 7, post, 2, 7, writer.light, writer.light);
            writer.box(post, 0, 14, post, 2, 14, writer.light, writer.light);
            post = 14;
        }
        writer.box(8, 3, 8, 8, 3, 13, writer.black, writer.black);
        writer.box(14, 3, 8, 14, 3, 13, writer.black, writer.black);
    }
}

void place_building_shell(const monument_writer &writer) {
    const jint water_height =
        std::max(writer.context.sea_level, jint{64}) - writer.piece.min_y;
    writer.water_box(0, 0, 0, 58, water_height, 58);

    const auto generate_wing = [&](bool flipped, jint x_off) {
        if (!writer.chunk_intersects(x_off, 0, x_off + 23, 20)) {
            return;
        }
        writer.box(x_off + 0, 0, 0, x_off + 24, 0, 20, writer.gray,
                   writer.gray);
        writer.water_box(x_off + 0, 1, 0, x_off + 24, 10, 20);
        for (jint i = 0; i < 4; ++i) {
            writer.box(x_off + i, i + 1, i, x_off + i, i + 1, 20, writer.light,
                       writer.light);
            writer.box(x_off + i + 7, i + 5, i + 7, x_off + i + 7, i + 5, 20,
                       writer.light, writer.light);
            writer.box(x_off + 17 - i, i + 5, i + 7, x_off + 17 - i, i + 5, 20,
                       writer.light, writer.light);
            writer.box(x_off + 24 - i, i + 1, i, x_off + 24 - i, i + 1, 20,
                       writer.light, writer.light);
            writer.box(x_off + i + 1, i + 1, i, x_off + 23 - i, i + 1, i,
                       writer.light, writer.light);
            writer.box(x_off + i + 8, i + 5, i + 7, x_off + 16 - i, i + 5,
                       i + 7, writer.light, writer.light);
        }
        writer.box(x_off + 4, 4, 4, x_off + 6, 4, 20, writer.gray, writer.gray);
        writer.box(x_off + 7, 4, 4, x_off + 17, 4, 6, writer.gray, writer.gray);
        writer.box(x_off + 18, 4, 4, x_off + 20, 4, 20, writer.gray,
                   writer.gray);
        writer.box(x_off + 11, 8, 11, x_off + 13, 8, 20, writer.gray,
                   writer.gray);
        writer.set(x_off + 12, 9, 12, writer.light);
        writer.set(x_off + 12, 9, 15, writer.light);
        writer.set(x_off + 12, 9, 18, writer.light);
        const jint left_pos = x_off + (flipped ? 19 : 5);
        const jint right_pos = x_off + (flipped ? 5 : 19);
        for (jint z = 20; z >= 5; z -= 3) {
            writer.set(left_pos, 5, z, writer.light);
        }
        for (jint z = 19; z >= 7; z -= 3) {
            writer.set(right_pos, 5, z, writer.light);
        }
        for (jint i = 0; i < 4; ++i) {
            const jint pos =
                flipped ? x_off + 24 - (17 - i * 3) : x_off + 17 - i * 3;
            writer.set(pos, 5, 5, writer.light);
        }
        writer.set(right_pos, 5, 5, writer.light);
        writer.box(x_off + 11, 1, 12, x_off + 13, 7, 12, writer.gray,
                   writer.gray);
        writer.box(x_off + 12, 1, 11, x_off + 12, 7, 13, writer.gray,
                   writer.gray);
    };

    generate_wing(false, 0);
    generate_wing(true, 33);

    if (writer.chunk_intersects(22, 5, 35, 17)) {
        writer.water_box(25, 0, 0, 32, 8, 20);
        for (jint i = 0; i < 4; ++i) {
            const jint z = 5 + i * 4;
            writer.box(24, 2, z, 24, 4, z, writer.light, writer.light);
            writer.box(22, 4, z, 23, 4, z, writer.light, writer.light);
            writer.set(25, 5, z, writer.light);
            writer.set(26, 6, z, writer.light);
            writer.set(26, 5, z, writer.lamp);
            writer.box(33, 2, z, 33, 4, z, writer.light, writer.light);
            writer.box(34, 4, z, 35, 4, z, writer.light, writer.light);
            writer.set(32, 5, z, writer.light);
            writer.set(31, 6, z, writer.light);
            writer.set(31, 5, z, writer.lamp);
            writer.box(27, 6, z, 30, 6, z, writer.gray, writer.gray);
        }
    }

    if (writer.chunk_intersects(15, 20, 42, 21)) {
        writer.box(15, 0, 21, 42, 0, 21, writer.gray, writer.gray);
        writer.water_box(26, 1, 21, 31, 3, 21);
        writer.box(21, 12, 21, 36, 12, 21, writer.gray, writer.gray);
        writer.box(17, 11, 21, 40, 11, 21, writer.gray, writer.gray);
        writer.box(16, 10, 21, 41, 10, 21, writer.gray, writer.gray);
        writer.box(15, 7, 21, 42, 9, 21, writer.gray, writer.gray);
        writer.box(16, 6, 21, 41, 6, 21, writer.gray, writer.gray);
        writer.box(17, 5, 21, 40, 5, 21, writer.gray, writer.gray);
        writer.box(21, 4, 21, 36, 4, 21, writer.gray, writer.gray);
        writer.box(22, 3, 21, 26, 3, 21, writer.gray, writer.gray);
        writer.box(31, 3, 21, 35, 3, 21, writer.gray, writer.gray);
        writer.box(23, 2, 21, 25, 2, 21, writer.gray, writer.gray);
        writer.box(32, 2, 21, 34, 2, 21, writer.gray, writer.gray);
        writer.box(28, 4, 20, 29, 4, 21, writer.light, writer.light);
        writer.set(27, 3, 21, writer.light);
        writer.set(30, 3, 21, writer.light);
        writer.set(26, 2, 21, writer.light);
        writer.set(31, 2, 21, writer.light);
        writer.set(25, 1, 21, writer.light);
        writer.set(32, 1, 21, writer.light);
        for (jint i = 0; i < 7; ++i) {
            writer.set(28 - i, 6 + i, 21, writer.black);
            writer.set(29 + i, 6 + i, 21, writer.black);
        }
        for (jint i = 0; i < 4; ++i) {
            writer.set(28 - i, 9 + i, 21, writer.black);
            writer.set(29 + i, 9 + i, 21, writer.black);
        }
        writer.set(28, 12, 21, writer.black);
        writer.set(29, 12, 21, writer.black);
        for (jint i = 0; i < 3; ++i) {
            writer.set(22 - i * 2, 8, 21, writer.black);
            writer.set(22 - i * 2, 9, 21, writer.black);
            writer.set(35 + i * 2, 8, 21, writer.black);
            writer.set(35 + i * 2, 9, 21, writer.black);
        }
        writer.water_box(15, 13, 21, 42, 15, 21);
        writer.water_box(15, 1, 21, 15, 6, 21);
        writer.water_box(16, 1, 21, 16, 5, 21);
        writer.water_box(17, 1, 21, 20, 4, 21);
        writer.water_box(21, 1, 21, 21, 3, 21);
        writer.water_box(22, 1, 21, 22, 2, 21);
        writer.water_box(23, 1, 21, 24, 1, 21);
        writer.water_box(42, 1, 21, 42, 6, 21);
        writer.water_box(41, 1, 21, 41, 5, 21);
        writer.water_box(37, 1, 21, 40, 4, 21);
        writer.water_box(36, 1, 21, 36, 3, 21);
        writer.water_box(33, 1, 21, 34, 1, 21);
        writer.water_box(35, 1, 21, 35, 2, 21);
    }

    if (writer.chunk_intersects(21, 21, 36, 36)) {
        writer.box(21, 0, 22, 36, 0, 36, writer.gray, writer.gray);
        writer.water_box(21, 1, 22, 36, 23, 36);
        for (jint i = 0; i < 4; ++i) {
            writer.box(21 + i, 13 + i, 21 + i, 36 - i, 13 + i, 21 + i,
                       writer.light, writer.light);
            writer.box(21 + i, 13 + i, 36 - i, 36 - i, 13 + i, 36 - i,
                       writer.light, writer.light);
            writer.box(21 + i, 13 + i, 22 + i, 21 + i, 13 + i, 35 - i,
                       writer.light, writer.light);
            writer.box(36 - i, 13 + i, 22 + i, 36 - i, 13 + i, 35 - i,
                       writer.light, writer.light);
        }
        writer.box(25, 16, 25, 32, 16, 32, writer.gray, writer.gray);
        writer.box(25, 17, 25, 25, 19, 25, writer.light, writer.light);
        writer.box(32, 17, 25, 32, 19, 25, writer.light, writer.light);
        writer.box(25, 17, 32, 25, 19, 32, writer.light, writer.light);
        writer.box(32, 17, 32, 32, 19, 32, writer.light, writer.light);
        writer.set(26, 20, 26, writer.light);
        writer.set(27, 21, 27, writer.light);
        writer.set(27, 20, 27, writer.lamp);
        writer.set(26, 20, 31, writer.light);
        writer.set(27, 21, 30, writer.light);
        writer.set(27, 20, 30, writer.lamp);
        writer.set(31, 20, 31, writer.light);
        writer.set(30, 21, 30, writer.light);
        writer.set(30, 20, 30, writer.lamp);
        writer.set(31, 20, 26, writer.light);
        writer.set(30, 21, 27, writer.light);
        writer.set(30, 20, 27, writer.lamp);
        writer.box(28, 21, 27, 29, 21, 27, writer.gray, writer.gray);
        writer.box(27, 21, 28, 27, 21, 29, writer.gray, writer.gray);
        writer.box(28, 21, 30, 29, 21, 30, writer.gray, writer.gray);
        writer.box(30, 21, 28, 30, 21, 29, writer.gray, writer.gray);
    }

    if (writer.chunk_intersects(0, 21, 6, 58)) {
        writer.box(0, 0, 21, 6, 0, 57, writer.gray, writer.gray);
        writer.water_box(0, 1, 21, 6, 7, 57);
        writer.box(4, 4, 21, 6, 4, 53, writer.gray, writer.gray);
        for (jint i = 0; i < 4; ++i) {
            writer.box(i, i + 1, 21, i, i + 1, 57 - i, writer.light,
                       writer.light);
        }
        for (jint z = 23; z < 53; z += 3) {
            writer.set(5, 5, z, writer.light);
        }
        writer.set(5, 5, 52, writer.light);
        for (jint i = 0; i < 4; ++i) {
            writer.box(i, i + 1, 21, i, i + 1, 57 - i, writer.light,
                       writer.light);
        }
        writer.box(4, 1, 52, 6, 3, 52, writer.gray, writer.gray);
        writer.box(5, 1, 51, 5, 3, 53, writer.gray, writer.gray);
    }

    if (writer.chunk_intersects(51, 21, 58, 58)) {
        writer.box(51, 0, 21, 57, 0, 57, writer.gray, writer.gray);
        writer.water_box(51, 1, 21, 57, 7, 57);
        writer.box(51, 4, 21, 53, 4, 53, writer.gray, writer.gray);
        for (jint i = 0; i < 4; ++i) {
            writer.box(57 - i, i + 1, 21, 57 - i, i + 1, 57 - i, writer.light,
                       writer.light);
        }
        for (jint z = 23; z < 53; z += 3) {
            writer.set(52, 5, z, writer.light);
        }
        writer.set(52, 5, 52, writer.light);
        writer.box(51, 1, 52, 53, 3, 52, writer.gray, writer.gray);
        writer.box(52, 1, 51, 52, 3, 53, writer.gray, writer.gray);
    }

    if (writer.chunk_intersects(0, 51, 57, 57)) {
        writer.box(7, 0, 51, 50, 0, 57, writer.gray, writer.gray);
        writer.water_box(7, 1, 51, 50, 10, 57);
        for (jint i = 0; i < 4; ++i) {
            writer.box(i + 1, i + 1, 57 - i, 56 - i, i + 1, 57 - i,
                       writer.light, writer.light);
        }
    }

    if (writer.chunk_intersects(7, 21, 13, 50)) {
        writer.box(7, 0, 21, 13, 0, 50, writer.gray, writer.gray);
        writer.water_box(7, 1, 21, 13, 10, 50);
        writer.box(11, 8, 21, 13, 8, 53, writer.gray, writer.gray);
        for (jint i = 0; i < 4; ++i) {
            writer.box(i + 7, i + 5, 21, i + 7, i + 5, 54, writer.light,
                       writer.light);
        }
        for (jint z = 21; z <= 45; z += 3) {
            writer.set(12, 9, z, writer.light);
        }
    }

    if (writer.chunk_intersects(44, 21, 50, 54)) {
        writer.box(44, 0, 21, 50, 0, 50, writer.gray, writer.gray);
        writer.water_box(44, 1, 21, 50, 10, 50);
        writer.box(44, 8, 21, 46, 8, 53, writer.gray, writer.gray);
        for (jint i = 0; i < 4; ++i) {
            writer.box(50 - i, i + 5, 21, 50 - i, i + 5, 54, writer.light,
                       writer.light);
        }
        for (jint z = 21; z <= 45; z += 3) {
            writer.set(45, 9, z, writer.light);
        }
    }

    if (writer.chunk_intersects(8, 44, 49, 54)) {
        writer.box(14, 0, 44, 43, 0, 50, writer.gray, writer.gray);
        writer.water_box(14, 1, 44, 43, 10, 50);
        for (jint x = 12; x <= 45; x += 3) {
            writer.set(x, 9, 45, writer.light);
            writer.set(x, 9, 52, writer.light);
            if (x == 12 || x == 18 || x == 24 || x == 33 || x == 39 ||
                x == 45) {
                writer.set(x, 9, 47, writer.light);
                writer.set(x, 9, 50, writer.light);
                writer.set(x, 10, 45, writer.light);
                writer.set(x, 10, 46, writer.light);
                writer.set(x, 10, 51, writer.light);
                writer.set(x, 10, 52, writer.light);
                writer.set(x, 11, 47, writer.light);
                writer.set(x, 11, 50, writer.light);
                writer.set(x, 12, 48, writer.light);
                writer.set(x, 12, 49, writer.light);
            }
        }
        for (jint i = 0; i < 3; ++i) {
            writer.box(8 + i, 5 + i, 54, 49 - i, 5 + i, 54, writer.gray,
                       writer.gray);
        }
        writer.box(11, 8, 54, 46, 8, 54, writer.light, writer.light);
        writer.box(14, 8, 44, 43, 8, 53, writer.gray, writer.gray);
    }

    if (writer.chunk_intersects(14, 21, 20, 43)) {
        writer.box(14, 0, 21, 20, 0, 43, writer.gray, writer.gray);
        writer.water_box(14, 1, 22, 20, 14, 43);
        writer.box(18, 12, 22, 20, 12, 39, writer.gray, writer.gray);
        writer.box(18, 12, 21, 20, 12, 21, writer.light, writer.light);
        for (jint i = 0; i < 4; ++i) {
            writer.box(i + 14, i + 9, 21, i + 14, i + 9, 43 - i, writer.light,
                       writer.light);
        }
        for (jint z = 23; z <= 39; z += 3) {
            writer.set(19, 13, z, writer.light);
        }
    }

    if (writer.chunk_intersects(37, 21, 43, 43)) {
        writer.box(37, 0, 21, 43, 0, 43, writer.gray, writer.gray);
        writer.water_box(37, 1, 22, 43, 14, 43);
        writer.box(37, 12, 22, 39, 12, 39, writer.gray, writer.gray);
        writer.box(37, 12, 21, 39, 12, 21, writer.light, writer.light);
        for (jint i = 0; i < 4; ++i) {
            writer.box(43 - i, i + 9, 21, 43 - i, i + 9, 43 - i, writer.light,
                       writer.light);
        }
        for (jint z = 23; z <= 39; z += 3) {
            writer.set(38, 13, z, writer.light);
        }
    }

    if (writer.chunk_intersects(15, 37, 42, 43)) {
        writer.box(21, 0, 37, 36, 0, 43, writer.gray, writer.gray);
        writer.water_box(21, 1, 37, 36, 14, 43);
        writer.box(21, 12, 37, 36, 12, 39, writer.gray, writer.gray);
        for (jint i = 0; i < 4; ++i) {
            writer.box(15 + i, i + 9, 43 - i, 42 - i, i + 9, 43 - i,
                       writer.light, writer.light);
        }
        for (jint x = 21; x <= 36; x += 3) {
            writer.set(x, 13, 38, writer.light);
        }
    }

    for (jint pillar_x = 0; pillar_x < 7; ++pillar_x) {
        jint pillar_z = 0;
        while (pillar_z < 7) {
            if (pillar_z == 0 && pillar_x == 3) {
                pillar_z = 6;
            }
            const jint base_x = pillar_x * 9;
            const jint base_z = pillar_z * 9;
            for (jint w = 0; w < 4; ++w) {
                for (jint d = 0; d < 4; ++d) {
                    writer.set(base_x + w, 0, base_z + d, writer.light);
                    writer.fill_column_down(writer.light, base_x + w, -1,
                                            base_z + d);
                }
            }
            if (pillar_x != 0 && pillar_x != 6) {
                pillar_z += 6;
            } else {
                ++pillar_z;
            }
        }
    }

    for (jint i = 0; i < 5; ++i) {
        writer.water_box(-1 - i, 0 + i * 2, -1 - i, -1 - i, 23, 58 + i);
        writer.water_box(58 + i, 0 + i * 2, -1 - i, 58 + i, 23, 58 + i);
        writer.water_box(0 - i, 0 + i * 2, -1 - i, 57 + i, 23, -1 - i);
        writer.water_box(0 - i, 0 + i * 2, 58 + i, 57 + i, 23, 58 + i);
    }
}

} // namespace

void place_monument_piece(const structure_place_context &context,
                          const structure_piece_record &piece,
                          worldgen_random &random) {
    if (!piece.monument.has_value()) {
        return;
    }
    const monument_writer writer{context, piece};
    switch (piece.monument->kind) {
    case monument_piece_kind::building:
        place_building_shell(writer);
        for (const auto &child : piece.children) {
            if (child.max_x < writer.chunk_min_x ||
                child.min_x > writer.chunk_max_x ||
                child.max_z < writer.chunk_min_z ||
                child.min_z > writer.chunk_max_z) {
                continue;
            }
            place_monument_piece(context, child, random);
        }
        break;
    case monument_piece_kind::entry_room:
        place_entry_room(writer);
        break;
    case monument_piece_kind::core_room:
        place_core_room(writer);
        break;
    case monument_piece_kind::double_x_room:
        place_double_x_room(writer);
        break;
    case monument_piece_kind::double_xy_room:
        place_double_xy_room(writer);
        break;
    case monument_piece_kind::double_y_room:
        place_double_y_room(writer);
        break;
    case monument_piece_kind::double_yz_room:
        place_double_yz_room(writer);
        break;
    case monument_piece_kind::double_z_room:
        place_double_z_room(writer);
        break;
    case monument_piece_kind::simple_room:
        place_simple_room(writer, random);
        break;
    case monument_piece_kind::simple_top_room:
        place_simple_top_room(writer, random);
        break;
    case monument_piece_kind::wing_room:
        place_wing_room(writer, random);
        break;
    case monument_piece_kind::penthouse:
        place_penthouse(writer);
        break;
    }
}

} // namespace refrain::worldgen::structure
