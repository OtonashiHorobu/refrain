// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "entity_store.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace refrain::sim::detail {

namespace {

/// Flatten the components of one live entity into the read projection.
[[nodiscard]] auto project(const entt::registry &registry, entt::entity handle)
    -> entity_state {
    const auto &identity{registry.get<component::identity>(handle)};
    const auto &transform{registry.get<component::transform>(handle)};
    const auto &rotation{registry.get<component::rotation>(handle)};
    entity_state state{};
    state.id = identity.id;
    state.type = identity.type;
    state.x = transform.x;
    state.y = transform.y;
    state.z = transform.z;
    state.vx = transform.vx;
    state.vy = transform.vy;
    state.vz = transform.vz;
    state.yaw = rotation.yaw;
    state.pitch = rotation.pitch;
    state.on_ground = transform.on_ground;
    state.pinned = registry.all_of<component::pinned>(handle);
    return state;
}

} // namespace

auto entity_store::find(entity_id id) const noexcept -> entt::entity {
    const auto found{by_id_.find(id)};
    return found == by_id_.end() ? entt::null : found->second;
}

auto entity_store::create(entity_id id, std::string type, double x, double y,
                          double z, float yaw, float pitch, bool pinned,
                          std::optional<::refrain::world::chunk_pos> column)
    -> entt::entity {
    const auto handle{registry_.create()};
    registry_.emplace<component::identity>(
        handle, component::identity{.id = id, .type = std::move(type)});
    registry_.emplace<component::transform>(
        handle, component::transform{.x = x, .y = y, .z = z});
    registry_.emplace<component::rotation>(
        handle, component::rotation{.yaw = yaw, .pitch = pitch});
    if (pinned) {
        registry_.emplace<component::pinned>(handle);
    }
    if (column.has_value()) {
        registry_.emplace<component::column_origin>(
            handle, component::column_origin{.pos = *column});
    }
    by_id_.emplace(id, handle);
    order_.push_back(handle);
    return handle;
}

auto entity_store::destroy(entity_id id) -> bool {
    const auto found{by_id_.find(id)};
    if (found == by_id_.end()) {
        return false;
    }
    const auto handle{found->second};
    by_id_.erase(found);
    registry_.destroy(handle);
    const auto position{std::find(order_.begin(), order_.end(), handle)};
    if (position != order_.end()) {
        order_.erase(position);
    }
    return true;
}

void entity_store::destroy(std::span<const entity_id> ids) noexcept {
    auto removed{false};
    for (const auto id : ids) {
        const auto found{by_id_.find(id)};
        if (found == by_id_.end()) {
            continue;
        }
        registry_.destroy(found->second);
        by_id_.erase(found);
        removed = true;
    }
    if (!removed) {
        return;
    }
    // Removing entries from the sorted order keeps the relative order, so a
    // single pass compacting the dead handles is enough.
    order_.erase(std::remove_if(order_.begin(), order_.end(),
                                [this](entt::entity handle) {
                                    return !registry_.valid(handle);
                                }),
                 order_.end());
}

void entity_store::rebuild_order() {
    // A rebuild only re-sorts: `create` always appends and `destroy` always
    // removes, so the order holds exactly the live handles at every point.
    std::sort(order_.begin(), order_.end(),
              [this](entt::entity left, entt::entity right) {
                  return registry_.get<component::identity>(left).id <
                         registry_.get<component::identity>(right).id;
              });
}

auto entity_store::projection_at(std::size_t index) const -> entity_state {
    return project(registry_, order_[index]);
}

auto entity_store::projection(entity_id id) const
    -> std::optional<entity_state> {
    const auto handle{find(id)};
    if (handle == entt::null) {
        return std::nullopt;
    }
    return project(registry_, handle);
}

} // namespace refrain::sim::detail

namespace refrain::sim {

auto entity_view::size() const noexcept -> std::size_t {
    return store_ == nullptr ? 0 : store_->size();
}

auto entity_view::empty() const noexcept -> bool { return size() == 0; }

auto entity_view::at(std::size_t index) const -> std::optional<entity_state> {
    if (store_ == nullptr || index >= store_->size()) {
        return std::nullopt;
    }
    return store_->projection_at(index);
}

auto entity_view::find(entity_id id) const -> std::optional<entity_state> {
    if (store_ == nullptr) {
        return std::nullopt;
    }
    return store_->projection(id);
}

} // namespace refrain::sim
