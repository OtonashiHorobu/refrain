// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/light.hpp>

#include <refrain/data/vanilla.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace refrain::sim {

auto block_light_table::from_tables(const data::vanilla_tables &tables)
    -> block_light_table {
    block_light_table table{};
    if (!tables.has_light()) {
        return table;
    }
    const auto shape_count{tables.light_shape_count()};
    std::vector<const block_light_shape *> shapes(shape_count, nullptr);
    for (std::uint32_t index{0}; index < shape_count; ++index) {
        const auto view{tables.light_shape_at(index)};
        block_light_shape shape{};
        for (std::size_t face{0}; face < shape.faces.size(); ++face) {
            const auto box_count{
                view.face_boxes[static_cast<std::size_t>(face)]};
            auto &boxes{shape.faces[face]};
            boxes.reserve(box_count);
            for (std::uint16_t box{0}; box < box_count; ++box) {
                const auto entry{tables.light_box(
                    index, static_cast<std::uint8_t>(face), box)};
                boxes.push_back(block_light_box{.bounds = entry.bounds});
            }
        }
        shapes[index] = table.append_shape(std::move(shape));
    }
    table.properties_.reserve(tables.state_count() + tables.block_count());
    for (std::uint32_t state{0}; state < tables.state_count(); ++state) {
        const auto light{tables.light_at(state)};
        block_light_property property{};
        property.emission = light.emission;
        property.dampening = light.dampening;
        if (light.shape != data::no_light_shape) {
            property.shape = shapes[light.shape];
        }
        table.properties_.emplace(std::string{tables.state_at(state).key},
                                  property);
    }
    // A stored palette writes the default state of a block as the bare block
    // name (a datapack does the same), so the table answers that spelling with
    // the default state's record. A named state key is never abbreviated this
    // way: the canonical key carries every property.
    for (std::uint32_t block{0}; block < tables.block_count(); ++block) {
        const auto view{tables.block_at(static_cast<std::uint16_t>(block))};
        if (view.state_count == 0) {
            continue;
        }
        const auto light{tables.light_at(view.default_state)};
        block_light_property property{};
        property.emission = light.emission;
        property.dampening = light.dampening;
        if (light.shape != data::no_light_shape) {
            property.shape = shapes[light.shape];
        }
        table.properties_.insert_or_assign(std::string{view.name}, property);
    }
    return table;
}

auto block_light_table::find(std::string_view key) const
    -> const block_light_property * {
    const auto found{properties_.find(key)};
    return found == properties_.end() ? nullptr : &found->second;
}

auto block_light_table::append_shape(block_light_shape shape)
    -> const block_light_shape * {
    shapes_.push_back(std::move(shape));
    return &shapes_.back();
}

void block_light_table::insert(std::string key, block_light_property property) {
    properties_.insert_or_assign(std::move(key), property);
}

} // namespace refrain::sim
