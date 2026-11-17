// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/world.hpp>

#include "entity_store.hpp"

#include <refrain/data/checksum.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/saved_data.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace refrain::sim {
namespace {

namespace world_ns = ::refrain::world;

[[nodiscard]] auto chunk_coord(std::int32_t value) noexcept -> std::int32_t {
    return value >= 0 ? value / world_ns::section_edge
                      : (value - (world_ns::section_edge - 1)) /
                            world_ns::section_edge;
}

[[nodiscard]] auto section_y_for(std::int32_t y) noexcept -> std::int32_t {
    return y >= 0 ? y / world_ns::section_edge
                  : (y - (world_ns::section_edge - 1)) / world_ns::section_edge;
}

/// Index inside a 16×16×16 section: `(y * 16 + z) * 16 + x`.
[[nodiscard]] auto block_index(std::int32_t y, std::int32_t z,
                               std::int32_t x) noexcept -> std::size_t {
    const auto local_y{y - section_y_for(y) * world_ns::section_edge};
    const auto local_z{z & (world_ns::section_edge - 1)};
    const auto local_x{x & (world_ns::section_edge - 1)};
    return static_cast<std::size_t>(
        (local_y * world_ns::section_edge + local_z) * world_ns::section_edge +
        local_x);
}

[[nodiscard]] auto all_same(const std::vector<block_state_id> &blocks) -> bool {
    if (blocks.empty()) {
        return true;
    }
    return std::all_of(blocks.begin() + 1, blocks.end(),
                       [first = blocks.front()](block_state_id value) {
                           return value == first;
                       });
}

} // namespace

block_registry::block_registry() {
    // Air is id 0 by contract; every unloaded/absent block returns it.
    (void)intern("minecraft:air");
}

block_registry::block_registry(block_registry &&other) noexcept {
    const std::unique_lock lock{other.mutex_};
    ids_ = std::move(other.ids_);
    keys_ = std::move(other.keys_);
}

auto block_registry::operator=(block_registry &&other) noexcept
    -> block_registry & {
    if (this == &other) {
        return *this;
    }
    const std::scoped_lock lock{mutex_, other.mutex_};
    ids_ = std::move(other.ids_);
    keys_ = std::move(other.keys_);
    return *this;
}

auto block_registry::intern(std::string key) -> block_state_id {
    const std::unique_lock lock{mutex_};
    const auto found{ids_.find(key)};
    if (found != ids_.end()) {
        return found->second;
    }
    const auto id{static_cast<block_state_id>(keys_.size())};
    keys_.push_back(std::move(key));
    try {
        ids_.emplace(keys_.back(), id);
    } catch (...) {
        keys_.pop_back();
        throw;
    }
    return id;
}

auto block_registry::find(std::string_view key) const
    -> std::optional<block_state_id> {
    const std::shared_lock lock{mutex_};
    const auto found{ids_.find(key)};
    if (found == ids_.end()) {
        return std::nullopt;
    }
    return found->second;
}

auto block_registry::key(block_state_id id) const -> std::string_view {
    const std::shared_lock lock{mutex_};
    if (id >= keys_.size()) {
        return {};
    }
    return keys_[id];
}

auto block_registry::size() const -> std::size_t {
    const std::shared_lock lock{mutex_};
    return keys_.size();
}

world::world(store_type store)
    : store_{std::move(store)},
      entities_{std::make_unique<detail::entity_store>()},
      events_{std::make_unique<core::events::bus>()}, light_{*this} {}

world::world(store_type &store, borrowed_tag)
    : store_{}, borrowed_{&store},
      entities_{std::make_unique<detail::entity_store>()},
      events_{std::make_unique<core::events::bus>()}, light_{*this} {}

world::world(world &&other) noexcept
    : store_{std::move(other.store_)}, borrowed_{other.borrowed_},
      registry_{std::move(other.registry_)}, chunks_{std::move(other.chunks_)},
      tickets_{std::move(other.tickets_)}, items_{std::move(other.items_)},
      light_table_{std::move(other.light_table_)},
      zstd_policy_{other.zstd_policy_}, entities_{std::move(other.entities_)},
      spawn_{other.spawn_}, difficulty_{other.difficulty_},
      hardcore_{other.hardcore_}, events_{std::move(other.events_)},
      timers_{std::move(other.timers_)}, next_timer_id_{other.next_timer_id_},
      rng_{other.rng_}, tick_{other.tick_},
      pending_edits_{std::move(other.pending_edits_)},
      pending_edits_head_{other.pending_edits_head_}, light_{*this},
      clock_{other.clock_} {
    other.borrowed_ = nullptr;
    light_.attach(*this);
}

auto world::operator=(world &&other) noexcept -> world & {
    if (this != &other) {
        store_ = std::move(other.store_);
        borrowed_ = other.borrowed_;
        other.borrowed_ = nullptr;
        registry_ = std::move(other.registry_);
        // The classification cache is derived from the registry keys; the
        // registry just changed, so every cached entry is stale.
        collisions_.clear();
        chunks_ = std::move(other.chunks_);
        tickets_ = std::move(other.tickets_);
        items_ = std::move(other.items_);
        light_table_ = std::move(other.light_table_);
        zstd_policy_ = other.zstd_policy_;
        entities_ = std::move(other.entities_);
        spawn_ = other.spawn_;
        difficulty_ = other.difficulty_;
        hardcore_ = other.hardcore_;
        events_ = std::move(other.events_);
        timers_ = std::move(other.timers_);
        next_timer_id_ = other.next_timer_id_;
        rng_ = other.rng_;
        tick_ = other.tick_;
        pending_edits_ = std::move(other.pending_edits_);
        pending_edits_head_ = other.pending_edits_head_;
        clock_ = other.clock_;
        light_ = std::move(other.light_);
        light_.attach(*this);
    }
    return *this;
}

world::~world() = default;

auto world::open(std::string_view path) -> result<world> {
    auto store{store_type::open(path)};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    world result{std::move(*store)};
    if (auto loaded{result.load_clock_state()}; !loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    return result;
}

auto world::attach(store_type &store) -> world {
    world result{store, borrowed_tag{}};
    // `attach` cannot report an error; malformed clock saved data leaves the
    // clock at its defaults. `open` is the validating entry point.
    static_cast<void>(result.load_clock_state());
    return result;
}

auto world::store_ref() noexcept -> store_type & {
    return borrowed_ != nullptr ? *borrowed_ : store_;
}

auto world::store_ref() const noexcept -> const store_type & {
    return borrowed_ != nullptr ? *borrowed_ : store_;
}

auto world::registry() noexcept -> block_registry & { return registry_; }

auto world::registry() const noexcept -> const block_registry & {
    return registry_;
}

auto world::decode_chunk(storage_chunk storage, const chunk_pos &pos)
    -> result<chunk> {
    if (auto valid{world_ns::validate(storage)}; !valid) {
        return std::unexpected{std::move(valid.error())};
    }
    chunk out{};
    out.pos = pos;
    out.y_pos = storage.y_pos;
    out.sections.reserve(storage.sections.size());
    for (auto &base : storage.sections) {
        section runtime{};
        runtime.y = base.y;
        // Keep only the non-block fields in the base; the block columns are
        // rebuilt from the runtime representation on flush.
        runtime.base.y = base.y;
        runtime.base.block_count = base.block_count;
        runtime.base.kind = ::refrain::storage::section_kind::single_value;
        runtime.base.bits_per_entry = 0;
        runtime.base.palette.clear();
        runtime.base.data.clear();
        runtime.base.biome_kind = base.biome_kind;
        runtime.base.biome_bits_per_entry = base.biome_bits_per_entry;
        // Copy, not move: `out.storage` keeps the decoded record intact so a
        // clean section can be reused verbatim by `encode_chunk()` on flush.
        runtime.base.biome_palette = base.biome_palette;
        runtime.base.biome_data = base.biome_data;

        if (base.palette.empty()) {
            runtime.uniform = true;
            runtime.value = air_block;
        } else if (base.kind ==
                   ::refrain::storage::section_kind::single_value) {
            runtime.uniform = true;
            runtime.value = registry_.intern(base.palette.front().key);
        } else {
            std::vector<std::uint32_t> indices(world_ns::section_volume);
            auto unpacked{world_ns::unpack_palette_indices(
                base.data, base.bits_per_entry, world_ns::section_volume,
                base.palette.size(), indices)};
            if (!unpacked) {
                return std::unexpected{std::move(unpacked.error())};
            }
            std::vector<block_state_id> palette{};
            palette.reserve(base.palette.size());
            for (const auto &entry : base.palette) {
                palette.push_back(registry_.intern(entry.key));
            }
            runtime.blocks.resize(indices.size());
            for (std::size_t index{0}; index < indices.size(); ++index) {
                const auto id{indices[index]};
                if (id >= palette.size()) {
                    return std::unexpected{make_error(
                        error_code::parse_error,
                        fmt::format("chunk palette index {} is out of range",
                                    id))};
                }
                runtime.blocks[index] = palette[id];
            }
            if (all_same(runtime.blocks)) {
                runtime.uniform = true;
                runtime.value = runtime.blocks.front();
                runtime.blocks.clear();
            } else {
                runtime.uniform = false;
                runtime.value = air_block;
            }
        }
        out.sections.push_back(std::move(runtime));
    }
    out.storage = std::move(storage);
    return out;
}

auto world::spawn_column_entities(chunk &column) -> status {
    return spawn_column_entities(column, {});
}

auto world::spawn_column_entities(chunk &column,
                                  std::span<const entity_id> entity_ids)
    -> status {
    if (!column.entity_ids.empty()) {
        // A column that already holds ids was spawned before: spawning again
        // would create a second copy of the same structure entities.
        return std::unexpected{make_error(
            error_code::already_exists,
            fmt::format("chunk ({}, {}) entities are already spawned",
                        column.pos.x, column.pos.z))};
    }
    const auto &records{column.storage.entities};
    if (!entity_ids.empty() && entity_ids.size() != records.size()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("chunk ({}, {}) stored {} entities but {} runtime ids "
                        "were preserved",
                        column.pos.x, column.pos.z, records.size(),
                        entity_ids.size()))};
    }
    if (records.empty()) {
        return status_ok();
    }
    column.entity_ids.reserve(records.size());
    for (std::size_t index{0}; index < records.size(); ++index) {
        const auto &record{records[index]};
        auto id{std::optional<entity_id>{}};
        if (entity_ids.empty()) {
            auto allocated{entities_->allocate_id()};
            if (!allocated) {
                // Roll back the ids allocated for this column so a failed load
                // leaves no orphaned entities behind.
                despawn_column_entities(column);
                return std::unexpected{std::move(allocated.error())};
            }
            id = *allocated;
        } else {
            id = entity_ids[index];
            if (*id == 0 || (index > 0 && *id <= entity_ids[index - 1])) {
                despawn_column_entities(column);
                return std::unexpected{make_error(
                    error_code::parse_error,
                    fmt::format("chunk ({}, {}) has out-of-order preserved "
                                "entity ids",
                                column.pos.x, column.pos.z))};
            }
        }
        static_cast<void>(entities_->create(*id, record.id, record.x, record.y,
                                            record.z, record.yaw, record.pitch,
                                            true, column.pos));
        column.entity_ids.push_back(*id);
    }
    // A load hands over a whole column at once. The ids were allocated in
    // record order, so the view is sorted by construction; rebuilding it here
    // keeps the order derived from the loaded ids instead of from the
    // registry's internal creation history.
    entities_->rebuild_order();
    return status_ok();
}

void world::despawn_column_entities(chunk &column) noexcept {
    entities_->destroy(std::span<const entity_id>{column.entity_ids});
    column.entity_ids.clear();
}

void world::despawn_all_column_entities() noexcept {
    for (auto &[pos, column] : chunks_) {
        static_cast<void>(pos);
        despawn_column_entities(column);
    }
}

auto world::chunk_entities(const chunk_pos &pos) const
    -> std::vector<chunk_entity> {
    const auto *column{find_chunk(pos)};
    if (column == nullptr) {
        return {};
    }
    const auto &records{column->storage.entities};
    if (records.size() != column->entity_ids.size()) {
        return {};
    }
    std::vector<chunk_entity> out;
    out.reserve(records.size());
    for (std::size_t index{0}; index < records.size(); ++index) {
        out.push_back(chunk_entity{.id = column->entity_ids[index],
                                   .record = records[index]});
    }
    return out;
}

auto world::ensure_chunk(const chunk_pos &pos) -> result<chunk *> {
    return load_chunk(pos, {});
}

auto world::load_chunk(const chunk_pos &pos,
                       std::span<const entity_id> entity_ids)
    -> result<chunk *> {
    if (auto *found{find_chunk(pos)}; found != nullptr) {
        return found;
    }
    auto loaded{[this, &pos] {
        const std::lock_guard lock{store_mutex_};
        return store_ref().read_chunk(pos);
    }()};
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    auto decoded{decode_chunk(std::move(*loaded), pos)};
    if (!decoded) {
        return std::unexpected{std::move(decoded.error())};
    }
    auto [position, inserted]{chunks_.emplace(pos, std::move(*decoded))};
    static_cast<void>(inserted);
    if (auto spawned{spawn_column_entities(position->second, entity_ids)};
        !spawned) {
        chunks_.erase(position);
        return std::unexpected{std::move(spawned.error())};
    }
    light_.column_loaded(position->second);
    return &position->second;
}

auto world::insert_chunk(const chunk_pos &pos, storage_chunk generated)
    -> result<chunk *> {
    if (find_chunk(pos) != nullptr) {
        return std::unexpected{make_error(
            error_code::already_exists,
            fmt::format("chunk ({}, {}) is already loaded", pos.x, pos.z))};
    }
    auto decoded{decode_chunk(std::move(generated), pos)};
    if (!decoded) {
        return std::unexpected{std::move(decoded.error())};
    }
    decoded->dirty = true;
    auto [position, inserted]{chunks_.emplace(pos, std::move(*decoded))};
    static_cast<void>(inserted);
    if (auto spawned{spawn_column_entities(position->second)}; !spawned) {
        chunks_.erase(position);
        return std::unexpected{std::move(spawned.error())};
    }
    light_.column_loaded(position->second);
    return &position->second;
}

auto world::find_chunk(const chunk_pos &pos) noexcept -> chunk * {
    const auto found{chunks_.find(pos)};
    return found == chunks_.end() ? nullptr : &found->second;
}

auto world::find_chunk(const chunk_pos &pos) const noexcept -> const chunk * {
    const auto found{chunks_.find(pos)};
    return found == chunks_.end() ? nullptr : &found->second;
}

auto world::section_index(std::int32_t y) noexcept -> std::int32_t {
    return section_y_for(y);
}

auto world::local_index(std::int32_t y, std::int32_t z, std::int32_t x) noexcept
    -> std::size_t {
    return block_index(y, z, x);
}

auto world::block_at(const block_pos &pos) const noexcept -> block_state_id {
    const auto *column{
        find_chunk(chunk_pos{chunk_coord(pos.x), chunk_coord(pos.z)})};
    if (column == nullptr) {
        return air_block;
    }
    const auto section_y{section_y_for(pos.y)};
    const auto found{std::find_if(
        column->sections.begin(), column->sections.end(),
        [section_y](const section &entry) { return entry.y == section_y; })};
    if (found == column->sections.end()) {
        return air_block;
    }
    if (found->uniform) {
        return found->value;
    }
    return found->blocks[block_index(pos.y, pos.z, pos.x)];
}

auto world::set_block(const block_pos &pos, block_state_id value) -> status {
    // The runtime world serves exactly the section range the chunk packet
    // carries. The storage model accepts a wider, dimension-agnostic range, so
    // an edit above `build_max_y()` would be persisted and never sent; reject
    // it at the mutation instead.
    if (pos.y < build_min_y() || pos.y > build_max_y()) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("block y={} is outside the runtime build range {}..{}",
                        pos.y, build_min_y(), build_max_y()))};
    }
    auto column{
        ensure_chunk(chunk_pos{chunk_coord(pos.x), chunk_coord(pos.z)})};
    if (!column) {
        return std::unexpected{std::move(column.error())};
    }
    const auto section_y{section_y_for(pos.y)};
    auto found{std::find_if(
        (*column)->sections.begin(), (*column)->sections.end(),
        [section_y](const section &entry) { return entry.y == section_y; })};
    if (found == (*column)->sections.end()) {
        if (value == air_block) {
            return status_ok();
        }
        section created{};
        created.y = static_cast<std::int8_t>(section_y);
        created.base.y = created.y;
        // A new section still needs a valid biome encoding; copy the column's
        // existing biome layer (or fall back to plains for a sparse fixture).
        if (!(*column)->sections.empty()) {
            const auto &neighbour{(*column)->sections.front().base};
            created.base.biome_kind = neighbour.biome_kind;
            created.base.biome_bits_per_entry = neighbour.biome_bits_per_entry;
            created.base.biome_palette = neighbour.biome_palette;
            created.base.biome_data = neighbour.biome_data;
        }
        if (created.base.biome_palette.empty()) {
            created.base.biome_kind =
                ::refrain::storage::section_kind::single_value;
            created.base.biome_bits_per_entry = 0;
            created.base.biome_palette.push_back("minecraft:plains");
        }
        created.uniform = true;
        created.value = air_block;
        found = (*column)->sections.insert((*column)->sections.end(),
                                           std::move(created));
        std::sort((*column)->sections.begin(), (*column)->sections.end(),
                  [](const section &left, const section &right) {
                      return left.y < right.y;
                  });
        // The sort may have moved the inserted element; locate it again.
        found = std::find_if(
            (*column)->sections.begin(), (*column)->sections.end(),
            [section_y](const section &entry) { return entry.y == section_y; });
    }
    const auto index{block_index(pos.y, pos.z, pos.x)};
    const auto previous{found->uniform ? found->value : found->blocks[index]};
    if (found->uniform) {
        if (found->value == value) {
            return status_ok();
        }
        if (value == air_block && found->value == air_block) {
            return status_ok();
        }
        found->blocks.assign(world_ns::section_volume, found->value);
        found->uniform = false;
        found->value = air_block;
    }
    if (found->blocks[index] == value) {
        return status_ok();
    }
    found->blocks[index] = value;
    update_heightmaps(**column, pos, value);
    found->dirty = true;
    (*column)->dirty = true;
    ++(*column)->revision;
    light_.block_changed(pos);
    static_cast<void>(light_.run());
    record_block_edit(pos, previous, value);
    return status_ok();
}

auto world::set_blocks(std::span<const block_change> changes)
    -> result<std::size_t> {
    std::vector<block_change> applied{};
    applied.reserve(changes.size());
    for (const auto &change : changes) {
        const auto before{block_at(change.pos)};
        if (before == change.new_state) {
            continue;
        }
        if (auto status{set_block(change.pos, change.new_state)}; !status) {
            return std::unexpected{status.error()};
        }
        applied.push_back(block_change{.pos = change.pos,
                                       .old_state = before,
                                       .new_state = change.new_state});
    }
    if (!applied.empty()) {
        try {
            block_batch_event event{.changes = applied};
            events_->publish(event);
        } catch (...) {
            // A listener failure must not roll back applied block changes.
        }
    }
    return applied.size();
}

auto world::encode_chunk(const chunk &value) -> result<storage_chunk> {
    storage_chunk out{value.storage};
    out.y_pos = value.y_pos;
    out.sections.clear();
    out.sections.reserve(value.sections.size());
    // Clean sections are bit-identical to the record they were decoded from:
    // reuse that record instead of re-serializing its palette (dirty
    // section reuse). Only sections touched since the last successful flush
    // are rebuilt; the record as a whole is still written as one chunk.
    std::map<std::int32_t, const storage_section *> original{};
    for (const auto &section : value.storage.sections) {
        original.emplace(section.y, &section);
    }
    for (const auto &runtime : value.sections) {
        if (!runtime.dirty) {
            const auto found{original.find(runtime.y)};
            if (found != original.end()) {
                out.sections.push_back(*found->second);
                continue;
            }
        }
        storage_section record{runtime.base};
        record.y = runtime.y;
        record.kind = ::refrain::storage::section_kind::single_value;
        record.bits_per_entry = 0;
        record.palette.clear();
        record.data.clear();
        record.block_count = 0;
        if (record.biome_palette.empty()) {
            record.biome_kind = ::refrain::storage::section_kind::single_value;
            record.biome_bits_per_entry = 0;
            record.biome_palette.push_back("minecraft:plains");
        }
        if (runtime.uniform) {
            const auto key{registry_.key(runtime.value)};
            if (key.empty()) {
                return std::unexpected{make_error(
                    error_code::unknown,
                    fmt::format("unknown runtime block id {}", runtime.value))};
            }
            record.palette.push_back(world_ns::palette_entry{
                .key = std::string{key},
                .count = static_cast<std::uint16_t>(
                    runtime.value == air_block ? 0 : world_ns::section_volume),
                .state = runtime.value,
            });
            record.block_count = static_cast<std::uint16_t>(
                runtime.value == air_block ? 0 : world_ns::section_volume);
        } else {
            std::map<std::string, block_state_id> by_key{};
            std::map<block_state_id, std::uint16_t> counts{};
            for (const auto block : runtime.blocks) {
                const auto key{registry_.key(block)};
                if (key.empty()) {
                    return std::unexpected{make_error(
                        error_code::unknown,
                        fmt::format("unknown runtime block id {}", block))};
                }
                by_key.emplace(std::string{key}, block);
                ++counts[block];
            }
            std::map<block_state_id, std::uint32_t> palette_index{};
            for (const auto &[key, id] : by_key) {
                const auto index{
                    static_cast<std::uint32_t>(record.palette.size())};
                record.palette.push_back(world_ns::palette_entry{
                    .key = key, .count = counts[id], .state = id});
                palette_index.emplace(id, index);
            }
            std::vector<std::uint32_t> indices(world_ns::section_volume);
            std::uint16_t non_air{0};
            for (std::size_t index{0}; index < runtime.blocks.size(); ++index) {
                const auto block{runtime.blocks[index]};
                indices[index] = palette_index.at(block);
                if (block != air_block) {
                    ++non_air;
                }
            }
            record.block_count = non_air;
            record.bits_per_entry =
                world_ns::bits_for_palette(record.palette.size());
            if (record.palette.size() <= 1) {
                record.kind = ::refrain::storage::section_kind::single_value;
                record.bits_per_entry = 0;
            } else {
                record.kind = ::refrain::storage::section_kind::palette;
                auto packed{world_ns::pack_palette_indices(
                    indices, record.bits_per_entry, record.palette.size(),
                    record.data)};
                if (!packed) {
                    return std::unexpected{std::move(packed.error())};
                }
            }
        }
        out.sections.push_back(std::move(record));
    }
    if (auto valid{world_ns::validate(out)}; !valid) {
        return std::unexpected{std::move(valid.error())};
    }
    return out;
}

auto world::store_record(const chunk &value) -> result<storage_chunk> {
    // A column with revision zero has never been edited: its decoded record is
    // still the authoritative encoding, so a flush skips rebuilding TLV from
    // the runtime sections and writes the record back unchanged.
    if (value.revision == 0) {
        storage_chunk record{value.storage};
        record.y_pos = value.y_pos;
        return record;
    }
    return encode_chunk(value);
}

auto world::add_ticket(const chunk_pos &pos, ticket_kind /*kind*/) -> status {
    auto column{ensure_chunk(pos)};
    if (!column) {
        return std::unexpected{std::move(column.error())};
    }
    ++tickets_[pos];
    return status_ok();
}

auto world::hold_ticket(const chunk_pos &pos, ticket_kind /*kind*/) -> status {
    ++tickets_[pos];
    return status_ok();
}

auto world::remove_ticket(const chunk_pos &pos) -> status {
    const auto found{tickets_.find(pos)};
    if (found == tickets_.end() || found->second == 0) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("no ticket held for chunk ({}, {})", pos.x, pos.z))};
    }
    if (--found->second == 0) {
        tickets_.erase(found);
    }
    return status_ok();
}

auto world::ticket_count(const chunk_pos &pos) const noexcept -> std::uint32_t {
    const auto found{tickets_.find(pos)};
    return found == tickets_.end() ? 0 : found->second;
}

auto world::write_entries(std::span<const snapshot_entry> entries, bool commit)
    -> status {
    const std::lock_guard lock{store_mutex_};
    for (const auto &entry : entries) {
        auto written{
            store_ref().write_chunk(entry.pos, entry.chunk, entry.level)};
        if (!written) {
            return written;
        }
    }
    if (!commit) {
        return status_ok();
    }
    return store_ref().commit();
}

auto world::unload_chunk(const chunk_pos &pos, bool commit) -> status {
    const auto found{chunks_.find(pos)};
    if (found == chunks_.end()) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("chunk ({}, {}) is not loaded", pos.x, pos.z))};
    }
    const auto held{ticket_count(pos)};
    if (held != 0) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("chunk ({}, {}) still holds {} ticket(s)",
                                   pos.x, pos.z, held))};
    }
    if (found->second.dirty) {
        // The column is dropped, so its edits reach the store first. The level
        // is the fastest one: this write happens on the simulation thread.
        auto encoded{store_record(found->second)};
        if (!encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
        std::array<snapshot_entry, 1> batch{snapshot_entry{
            .pos = pos,
            .chunk = std::move(*encoded),
            .revision = found->second.revision,
            .level = eviction_zstd_level,
        }};
        if (auto written{write_entries(batch, commit)}; !written) {
            return written;
        }
    }
    despawn_column_entities(found->second);
    chunks_.erase(found);
    light_.column_unloaded(pos);
    return status_ok();
}

auto world::unload_chunks(std::span<const chunk_pos> positions, bool commit)
    -> status {
    // Validate the whole set before touching the store: a caller that passes
    // one pinned or unloaded column gets a single error and no partial unload.
    std::vector<const chunk *> victims{};
    victims.reserve(positions.size());
    for (const auto &pos : positions) {
        const auto found{chunks_.find(pos)};
        if (found == chunks_.end()) {
            return std::unexpected{make_error(
                error_code::not_found,
                fmt::format("chunk ({}, {}) is not loaded", pos.x, pos.z))};
        }
        const auto held{ticket_count(pos)};
        if (held != 0) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("chunk ({}, {}) still holds {} ticket(s)", pos.x,
                            pos.z, held))};
        }
        victims.push_back(&found->second);
    }
    std::vector<snapshot_entry> dirty{};
    dirty.reserve(victims.size());
    for (std::size_t index{0}; index < victims.size(); ++index) {
        const auto &column{*victims[index]};
        if (!column.dirty) {
            continue;
        }
        auto encoded{store_record(column)};
        if (!encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
        dirty.push_back(snapshot_entry{
            .pos = positions[index],
            .chunk = std::move(*encoded),
            .revision = column.revision,
            .level = eviction_zstd_level,
        });
    }
    if (!dirty.empty()) {
        if (auto written{write_entries(dirty, commit)}; !written) {
            return written;
        }
    }
    for (const auto &pos : positions) {
        if (auto *column{find_chunk(pos)}; column != nullptr) {
            despawn_column_entities(*column);
        }
        chunks_.erase(pos);
        light_.column_unloaded(pos);
    }
    return status_ok();
}

auto world::unticketed_chunks() const -> std::vector<resident_column> {
    std::vector<resident_column> candidates{};
    candidates.reserve(chunks_.size());
    for (const auto &[pos, column] : chunks_) {
        if (tickets_.contains(pos)) {
            continue;
        }
        candidates.push_back(
            resident_column{.pos = pos, .dirty = column.dirty});
    }
    return candidates;
}

auto world::chunk_positions() const -> std::vector<chunk_pos> {
    std::vector<chunk_pos> positions{};
    positions.reserve(chunks_.size());
    for (const auto &[pos, column] : chunks_) {
        static_cast<void>(column);
        positions.push_back(pos);
    }
    return positions;
}

auto world::set_zstd_policy(const zstd_policy &policy) -> status {
    const auto valid_level = [](int level) {
        return level >= ::refrain::world::rwf::min_zstd_level &&
               level <= ::refrain::world::rwf::max_zstd_level;
    };
    if (!valid_level(policy.near_level) || !valid_level(policy.mid_level) ||
        !valid_level(policy.far_level)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "zstd policy levels must be within [1, 19]")};
    }
    if (policy.near_radius < 0 || policy.mid_radius < policy.near_radius) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "zstd policy radii must satisfy 0 <= near <= mid")};
    }
    zstd_policy_ = policy;
    return status_ok();
}

auto world::compression_level_for(const chunk_pos &pos) const noexcept -> int {
    // Player positions are block coordinates; a chunk is 16 blocks. `floor`
    // (not truncation) keeps negative coordinates on the correct chunk. The
    // first player by entity id is the reference; with no players the chunk
    // distance is measured from the origin.
    const auto *primary{entities_->players().first()};
    const auto player_x{
        std::floor((primary == nullptr ? 0.0 : primary->x) /
                   static_cast<double>(world_ns::section_edge))};
    const auto player_z{
        std::floor((primary == nullptr ? 0.0 : primary->z) /
                   static_cast<double>(world_ns::section_edge))};
    // Doubles avoid an overflow-prone float→int cast for hostile positions;
    // the magnitudes here are exact (well below 2^53).
    const auto dx{std::fabs(player_x - static_cast<double>(pos.x))};
    const auto dz{std::fabs(player_z - static_cast<double>(pos.z))};
    const auto distance{dx > dz ? dx : dz};
    if (distance <= static_cast<double>(zstd_policy_.near_radius)) {
        return zstd_policy_.near_level;
    }
    if (distance <= static_cast<double>(zstd_policy_.mid_radius)) {
        return zstd_policy_.mid_level;
    }
    return zstd_policy_.far_level;
}

auto world::snapshot_dirty() -> result<std::vector<snapshot_entry>> {
    std::vector<snapshot_entry> snapshot{};
    for (const auto &[pos, column] : chunks_) {
        if (!column.dirty) {
            continue;
        }
        auto encoded{encode_chunk(column)};
        if (!encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
        snapshot.push_back(snapshot_entry{.pos = pos,
                                          .chunk = std::move(*encoded),
                                          .revision = column.revision,
                                          .level = compression_level_for(pos)});
    }
    return snapshot;
}

auto world::snapshot_pending() -> std::vector<pending_snapshot> {
    std::vector<pending_snapshot> snapshot{};
    for (const auto &[pos, column] : chunks_) {
        if (!column.dirty) {
            continue;
        }
        snapshot.push_back(
            pending_snapshot{.pos = pos,
                             .column = column,
                             .revision = column.revision,
                             .level = compression_level_for(pos)});
    }
    return snapshot;
}

auto world::encode_snapshot(const pending_snapshot &pending)
    -> result<snapshot_entry> {
    auto encoded{encode_chunk(pending.column)};
    if (!encoded) {
        return std::unexpected{std::move(encoded.error())};
    }
    return snapshot_entry{.pos = pending.pos,
                          .chunk = std::move(*encoded),
                          .revision = pending.revision,
                          .level = pending.level};
}

auto world::write_pending(std::span<const pending_snapshot> pending)
    -> result<std::vector<snapshot_entry>> {
    std::vector<snapshot_entry> encoded{};
    encoded.reserve(pending.size());
    for (const auto &entry : pending) {
        auto one{encode_snapshot(entry)};
        if (!one) {
            return std::unexpected{std::move(one.error())};
        }
        encoded.push_back(std::move(*one));
    }
    auto written{write_snapshot(encoded)};
    if (!written) {
        return std::unexpected{std::move(written.error())};
    }
    return encoded;
}

auto world::write_snapshot(std::span<const snapshot_entry> snapshot) -> status {
    const std::lock_guard lock{store_mutex_};
    if (snapshot.empty()) {
        return store_ref().commit();
    }
    for (const auto &entry : snapshot) {
        auto written{
            store_ref().write_chunk(entry.pos, entry.chunk, entry.level)};
        if (!written) {
            return written;
        }
    }
    return store_ref().commit();
}

auto world::compact_stores() -> result<::refrain::world::world_compact> {
    const std::lock_guard lock{store_mutex_};
    return store_ref().compact();
}

auto world::compact_regions(std::uint64_t min_bytes, unsigned min_percent)
    -> result<std::size_t> {
    const std::lock_guard lock{store_mutex_};
    return store_ref().compact_regions(min_bytes, min_percent);
}

auto world::rebuild_palette_in_place(unsigned threads, int zstd_level)
    -> result<::refrain::world::palette_maintenance_report> {
    if (borrowed_ != nullptr) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "palette maintenance replaces the store object, which a world "
            "built on a borrowed store does not own")};
    }
    if (zstd_level < ::refrain::world::rwf::min_zstd_level ||
        zstd_level > ::refrain::world::rwf::max_zstd_level) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("zstd level {} is outside [{}, {}]", zstd_level,
                        ::refrain::world::rwf::min_zstd_level,
                        ::refrain::world::rwf::max_zstd_level))};
    }
    auto flushed{flush()};
    if (!flushed) {
        return std::unexpected{std::move(flushed.error())};
    }
    const std::string root{store_ref().root()};
    if (root.empty()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    const int store_level{store_ref().zstd_level()};

    // The rebuilt world has a fresh palette table, so every runtime chunk must
    // be decoded again (its `section.base` would otherwise carry stale palette
    // ids). Reload the chunks that were loaded so the runtime view and
    // `state_hash()` survive the swap. Tickets are kept, but a ticket is a
    // residency pin rather than proof that the store holds the column: a
    // viewport pin covers columns that have not been generated yet, and those
    // must not turn the rebuild into a failure.
    std::vector<chunk_pos> positions;
    positions.reserve(chunks_.size());
    // The reload has to reuse the logical ids of generated column entities:
    // the rest of the simulation (clients, replay, the state hash) already
    // knows those ids, and allocating new ones would change `state_hash()`
    // even though nothing about the world changed.
    std::map<chunk_pos, std::vector<entity_id>, chunk_order> column_entities;
    for (const auto &[pos, column] : chunks_) {
        positions.push_back(pos);
        if (!column.entity_ids.empty()) {
            column_entities.emplace(pos, column.entity_ids);
        }
    }
    std::sort(positions.begin(), positions.end(), chunk_order{});

    // Release every region handle before the helper renames the directory.
    store_ = store_type{};
    despawn_all_column_entities();
    chunks_.clear();
    light_ = light_engine{*this};

    auto maintenance{::refrain::world::rebuild_palette_in_place(
        root, {.threads = threads, .zstd_level = zstd_level})};

    auto reopened{store_type::open(root)};
    if (!reopened) {
        // The world cannot be used any more; the caller must stop (documented
        // in the header). Do not hide the maintenance failure behind it.
        if (!maintenance) {
            return std::unexpected{make_error(
                maintenance.error().code,
                fmt::format("palette maintenance failed: {}; reopening '{}' "
                            "failed: {} (world store is closed; stop using "
                            "this world)",
                            maintenance.error().message, root,
                            reopened.error().message))};
        }
        return std::unexpected{make_error(
            reopened.error().code,
            fmt::format("cannot reopen '{}' after palette maintenance: {} "
                        "(world store is closed; stop using this world)",
                        root, reopened.error().message))};
    }
    store_ = std::move(*reopened);
    if (auto level{store_ref().set_zstd_level(store_level)}; !level) {
        return std::unexpected{std::move(level.error())};
    }
    for (const auto &pos : positions) {
        auto reused{std::span<const entity_id>{}};
        if (const auto found{column_entities.find(pos)};
            found != column_entities.end()) {
            reused = found->second;
        }
        auto loaded{load_chunk(pos, reused)};
        if (!loaded) {
            // A reload failure leaves a partial runtime view; the store itself
            // stays valid. Prefer the maintenance error when there is one.
            if (!maintenance) {
                return std::unexpected{std::move(maintenance.error())};
            }
            return std::unexpected{std::move(loaded.error())};
        }
    }
    if (!maintenance) {
        return std::unexpected{std::move(maintenance.error())};
    }
    return std::move(*maintenance);
}

auto world::flush() -> status {
    // The synchronous path shares `snapshot_dirty()` with the background
    // writer, so it uses the same adaptive per-entry zstd levels.
    auto snapshot{snapshot_dirty()};
    if (!snapshot) {
        return std::unexpected{std::move(snapshot.error())};
    }
    if (!snapshot->empty()) {
        auto written{write_snapshot(*snapshot)};
        if (!written) {
            return written;
        }
        mark_clean(*snapshot);
    }
    return store_clock_state();
}

void world::mark_clean(std::span<const snapshot_entry> snapshot) noexcept {
    for (const auto &entry : snapshot) {
        auto *column{find_chunk(entry.pos)};
        if (column != nullptr && column->revision == entry.revision) {
            column->dirty = false;
            for (auto &section : column->sections) {
                section.dirty = false;
            }
        }
    }
}

auto world::take_light_changes() -> std::vector<light_section_change> {
    return light_.take_changes();
}

auto world::take_block_edits() -> std::vector<block_change> {
    std::vector<block_change> edits{};
    edits.resize(pending_edits_.size());
    if (pending_edits_.size() < max_pending_edits) {
        std::copy(pending_edits_.begin(), pending_edits_.end(), edits.begin());
    } else {
        // Full buffer: the oldest entry lives at the head and the newest just
        // before it. Copy in application order so the caller sees the same
        // sequence an unbounded list would produce.
        for (std::size_t index{0}; index < edits.size(); ++index) {
            edits[index] = pending_edits_[(pending_edits_head_ + index) %
                                          max_pending_edits];
        }
    }
    // Keep the allocated storage: the next batch of edits reuses it instead of
    // growing a fresh vector every tick.
    pending_edits_.clear();
    pending_edits_head_ = 0;
    return edits;
}

auto world::record_block_edit(const block_pos &pos, block_state_id old_state,
                              block_state_id new_state) -> void {
    const block_change change{
        .pos = pos, .old_state = old_state, .new_state = new_state};
    if (pending_edits_.size() < max_pending_edits) {
        pending_edits_.push_back(change);
        return;
    }
    // A scripted fill can produce more edits than one tick should send. The
    // newest entries win: a client that missed a batch receives the column
    // again the next time it is streamed. Overwriting the oldest entry is
    // O(1); erasing the front would move the whole buffer per edit.
    pending_edits_[pending_edits_head_] = change;
    pending_edits_head_ = (pending_edits_head_ + 1) % max_pending_edits;
}

auto world::load_clock_state() -> status {
    if (!store_ref().valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    // A synthetic world (tests, tools) may exist without a level record; the
    // clock then starts at tick zero. The runtime validates the level
    // projection itself when it boots.
    if (auto projection{store_ref().level()}; projection) {
        clock_.set_game_time(projection->time);
        clock_.set_total_ticks(projection->time);
        // The weather draws are keyed by the world seed; opening a stored
        // world has to recover it before the first draw, otherwise a reloaded
        // world would draw a different weather sequence than the running one.
        clock_.seed(static_cast<std::uint64_t>(projection->seed));
    }
    return ::refrain::sim::read_world_clock(store_ref().objects(), clock_);
}

auto world::store_clock_state() -> status {
    const std::lock_guard lock{store_mutex_};
    if (!store_ref().valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    // A synthetic world may have no level record; only its chunks and objects
    // are persisted then.
    if (auto written{store_ref().write_level_time(clock_.game_time())};
        !written && written.error().code != error_code::not_found) {
        return written;
    }
    if (auto written{
            ::refrain::sim::write_world_clock(store_ref().objects(), clock_)};
        !written) {
        return written;
    }
    return store_ref().commit();
}

auto world::players() noexcept -> player_table & {
    return entities_->players();
}

auto world::players() const noexcept -> const player_table & {
    return entities_->players();
}

auto world::add_player(player_table::registration record) -> result<entity_id> {
    return entities_->players().add(std::move(record));
}

auto world::remove_player(std::uint64_t connection) -> bool {
    return entities_->players().remove(connection);
}

auto world::player(entity_id id) noexcept -> player_state * {
    return entities_->players().find(id);
}

auto world::player(entity_id id) const noexcept -> const player_state * {
    return entities_->players().find(id);
}

void world::set_player_position(player_state &player, double x, double y,
                                double z) noexcept {
    player.x = x;
    player.y = y;
    player.z = z;
    player.vx = 0.0;
    player.vy = 0.0;
    player.vz = 0.0;
    player.on_ground = false;
    player.ground_y = y;
    player.fall_distance = 0.0;
    player.has_move_request = false;
}

auto world::schedule_after(std::int64_t delay, std::function<void()> fn)
    -> timer_id {
    const auto id{next_timer_id_++};
    timers_.push_back(
        scheduled_task{.due = std::max<std::int64_t>(tick_, tick_ + delay),
                       .id = id,
                       .fn = std::move(fn)});
    return id;
}

auto world::cancel_timer(timer_id id) -> bool {
    const auto found{std::find_if(
        timers_.begin(), timers_.end(),
        [id](const scheduled_task &task) { return task.id == id; })};
    if (found == timers_.end()) {
        return false;
    }
    timers_.erase(found);
    return true;
}

void world::tick() noexcept {
    // Players tick in ascending entity id order; a player record's physics
    // depends only on its own state and the world blocks, so the order does not
    // couple the records but stays fixed for the state hash.
    entities_->players().for_each(
        [this](player_state &player) { tick_player(player); });
    tick_entities();
    ++tick_;
    (void)rng_.next_u64();
    // A block edit already drained its light update synchronously; this is the
    // backstop for work queued while the world was not ticking (tests, tools).
    static_cast<void>(light_.run());
    clock_.tick();
    // Notification listeners are mod/extension code: a throw must never take
    // down the sim tick, and the bus contract is single-threaded.
    try {
        tick_event event{.tick = tick_};
        events_->publish(event);
    } catch (...) {
        // The listener failed; the tick itself completed.
    }
    // Timers due at or before this tick run in (due, id) order. A timer that
    // schedules another zero-delay timer is picked up by the same loop; the
    // iteration bound keeps such a chain from holding the tick open forever.
    // Callbacks still due after the bound stay queued for the next tick, where
    // the same (due, id) order runs them first.
    for (std::size_t ran{0}; ran < max_timers_per_tick; ++ran) {
        auto next{timers_.end()};
        for (auto it{timers_.begin()}; it != timers_.end(); ++it) {
            if (it->due > tick_) {
                continue;
            }
            if (next == timers_.end() || it->due < next->due ||
                (it->due == next->due && it->id < next->id)) {
                next = it;
            }
        }
        if (next == timers_.end()) {
            break;
        }
        auto task{std::move(*next)};
        timers_.erase(next);
        try {
            if (task.fn) {
                task.fn();
            }
        } catch (...) {
            // A timer failure must not stop the tick loop.
        }
    }
}

void world::seed(std::uint64_t value) noexcept {
    rng_.seed(value);
    // Weather draws are keyed by the world seed, so the clock has to carry the
    // same seed the simulation does.
    clock_.seed(value);
}

auto world::random() noexcept -> rng & { return rng_; }

auto world::random() const noexcept -> const rng & { return rng_; }

auto world::add_item(player_state &player, std::string_view item,
                     std::uint32_t count) -> status {
    if (item.empty() || count == 0) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "inventory item/count is empty")};
    }
    const auto current{item_count(player, item)};
    if (static_cast<std::uint64_t>(current) + count >
        std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "inventory item count overflows")};
    }
    // The item table is content data: a malformed record could name a stack
    // larger than the protocol ceiling, so the limit is clamped to it. A zero
    // limit would make the append loop below never consume the count.
    const auto limit{
        std::clamp(items_.stack_limit(item), 1U, absolute_stack_size)};
    // Refuse a count the modeled inventory cannot hold before touching it, so a
    // failed add leaves the record exactly as the caller found it.
    std::uint64_t capacity{0};
    for (const auto &stack : player.inventory) {
        if (stack.item == item && stack.components.empty()) {
            capacity += limit - std::min(limit, stack.count);
        } else if (sim::stack_empty(stack)) {
            capacity += limit;
        }
    }
    if (player.inventory.size() < modeled_inventory_slots) {
        capacity += static_cast<std::uint64_t>(modeled_inventory_slots -
                                               player.inventory.size()) *
                    limit;
    }
    if (count > capacity) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("inventory cannot hold {} x{}: the modeled {} slots "
                        "have room for {} more",
                        count, item, modeled_inventory_slots, capacity))};
    }
    auto remaining{count};
    // Fill the matching partial stacks that carry no components: a stack with a
    // damage or enchantment component is a different slot identity and is never
    // merged into.
    for (auto &stack : player.inventory) {
        if (remaining == 0) {
            break;
        }
        if (stack.item != item || !stack.components.empty() ||
            stack.count >= limit) {
            continue;
        }
        const auto space{limit - stack.count};
        const auto added{std::min(space, remaining)};
        stack.count += added;
        remaining -= added;
    }
    // An emptied slot keeps its index (see `remove_item`), so reuse it before
    // appending: the record never keeps an empty slot and a used one at once.
    for (auto &stack : player.inventory) {
        if (remaining == 0) {
            break;
        }
        if (!sim::stack_empty(stack)) {
            continue;
        }
        const auto added{std::min(limit, remaining)};
        stack = item_stack{.item = std::string{item}, .count = added};
        remaining -= added;
    }
    while (remaining != 0 &&
           player.inventory.size() < modeled_inventory_slots) {
        const auto added{std::min(limit, remaining)};
        player.inventory.push_back(
            item_stack{.item = std::string{item}, .count = added});
        remaining -= added;
    }
    return status_ok();
}

auto world::damage_item(player_state &player, std::size_t index,
                        std::uint32_t amount) -> damage_result {
    if (index >= player.inventory.size()) {
        return damage_result::unchanged;
    }
    return damage_stack(player.inventory[index], items_, amount);
}

void world::set_item_table(item_table items) { items_ = std::move(items); }

void world::set_block_light_table(block_light_table table) {
    light_table_ = std::move(table);
    light_.reload_properties();
    // Recompute every loaded column under the new table: the stored light
    // arrays were computed with the table that was installed before.
    for (const auto &pos : chunk_positions()) {
        auto *column{find_chunk(pos)};
        if (column != nullptr) {
            light_.column_loaded(*column);
        }
    }
}

auto world::remove_item(player_state &player, std::string_view item,
                        std::uint32_t count) -> bool {
    if (count == 0) {
        return true;
    }
    // Only a plain stack is the identity the item name denotes: a damaged,
    // enchanted or named stack carries its own component state and is a
    // different slot, so it is neither counted nor consumed here.
    std::uint64_t removable{0};
    for (const auto &stack : player.inventory) {
        if (stack.item == item && stack.components.empty()) {
            removable += stack.count;
        }
    }
    if (removable < count) {
        return false;
    }
    auto remaining{count};
    for (auto &stack : player.inventory) {
        if (remaining == 0) {
            break;
        }
        if (stack.item != item || !stack.components.empty()) {
            continue;
        }
        const auto removed{std::min(stack.count, remaining)};
        stack.count -= removed;
        remaining -= removed;
        if (stack.count == 0) {
            // Clear the slot in place rather than erasing it: the menu and the
            // container packets address stacks by index, so an erase would
            // shift every later slot under an open menu.
            stack.item.clear();
            stack.components.clear();
        }
    }
    return true;
}

auto world::item_count(const player_state &player,
                       std::string_view item) const noexcept -> std::uint32_t {
    std::uint64_t total{0};
    for (const auto &stack : player.inventory) {
        if (stack.item == item) {
            total += stack.count;
        }
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        total, std::numeric_limits<std::uint32_t>::max()));
}

auto world::spawn_entity(std::string_view type, double x, double y, double z)
    -> result<entity_id> {
    if (type.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "entity type must not be empty")};
    }
    auto id{entities_->allocate_id()};
    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    static_cast<void>(
        entities_->create(*id, std::string{type}, x, y, z, 0.0F, 0.0F, false));
    return *id;
}

auto world::remove_entity(entity_id id) -> bool {
    const auto handle{entities_->find(id)};
    if (handle == entt::null) {
        return false;
    }
    // A generated entity is owned by its column: removing it from the store
    // also takes it out of the column record, so a later unload writes a record
    // that does not spawn it again and no client is announced a stale id.
    const auto *origin{
        entities_->registry().try_get<detail::component::column_origin>(
            handle)};
    if (origin != nullptr) {
        if (auto *column{find_chunk(origin->pos)}; column != nullptr) {
            const auto found{std::find(column->entity_ids.begin(),
                                       column->entity_ids.end(), id)};
            if (found != column->entity_ids.end()) {
                const auto index{static_cast<std::size_t>(
                    std::distance(column->entity_ids.begin(), found))};
                column->entity_ids.erase(found);
                if (index < column->storage.entities.size()) {
                    column->storage.entities.erase(
                        column->storage.entities.begin() +
                        static_cast<std::ptrdiff_t>(index));
                }
                column->dirty = true;
            }
        }
    }
    return entities_->destroy(id);
}

auto world::entities() const noexcept -> entity_view {
    return entity_view{entities_.get()};
}

auto world::entity(entity_id id) const -> std::optional<entity_state> {
    return entities_->projection(id);
}

void world::tick_entities() noexcept {
    constexpr double gravity{0.08};
    constexpr double vertical_drag{0.98};
    constexpr double horizontal_drag{0.91};
    auto &entity_registry{entities_->registry()};
    // The two component pools are fetched once per tick: the per-entity step
    // then only pays the sparse-set index, not a component-id lookup.
    const auto &pinned_entities{
        entity_registry.storage<detail::component::pinned>()};
    auto &transforms{entity_registry.storage<detail::component::transform>()};
    // The ascending order is the simulation contract: the state hash and the
    // clientbound stream depend on it, and an entity's step reads only its own
    // state and the world blocks, so the order does not couple the entities.
    for (const auto handle : entities_->order()) {
        if (pinned_entities.contains(handle)) {
            // A generated entity is held where its structure placed it: this
            // build has no entity AI or living-entity physics, so applying the
            // point-entity gravity would only drag a shulker or an item frame
            // off its structure.
            continue;
        }
        auto &value{transforms.get(handle)};
        value.vy = (value.vy - gravity) * vertical_drag;
        value.vx *= horizontal_drag;
        value.vz *= horizontal_drag;
        const auto before_y{value.y};
        value.x += value.vx;
        value.z += value.vz;
        value.y += value.vy;
        if (value.vy <= 0.0) {
            // Collision: the entity is a point at its feet; clamp to
            // the first solid block crossed in this tick (no AABB sweep yet).
            // Both scan bounds are derived through the clamped block helper:
            // the block model only covers the build range, and an entity can
            // be spawned (or fall) far outside it. Deriving the rows from a
            // clamped Y makes the narrowing conversion defined and bounds the
            // downward scan to the build height instead of the whole distance
            // between two unclamped coordinates.
            const auto start{static_cast<std::int32_t>(block_y_of(before_y))};
            const auto end{static_cast<std::int32_t>(block_y_of(value.y))};
            for (auto block_y{start}; block_y >= end; --block_y) {
                const auto block{block_at(
                    block_pos{static_cast<std::int32_t>(std::floor(value.x)),
                              static_cast<std::int16_t>(block_y),
                              static_cast<std::int32_t>(std::floor(value.z))})};
                if (!block_solid(block)) {
                    continue;
                }
                const auto top{static_cast<double>(block_y) + 1.0};
                if (before_y >= top && value.y < top) {
                    value.y = top;
                    value.vy = 0.0;
                    value.on_ground = true;
                    break;
                }
            }
        }
        if (value.vy != 0.0) {
            value.on_ground = false;
        }
    }
}

namespace {
constexpr double player_half_width = 0.3;
constexpr double player_height = 1.8;
constexpr double collision_epsilon = 1.0e-7;

/// Name part of a canonical block-state key (`minecraft:oak_slab[type=top]`
/// -> `minecraft:oak_slab`).
[[nodiscard]] auto state_name(std::string_view key) -> std::string_view {
    return key.substr(0, key.find('['));
}

/// State part of a canonical block-state key, brackets included; empty when
/// the state has no properties.
[[nodiscard]] auto state_properties(std::string_view key) -> std::string_view {
    const auto open{key.find('[')};
    return open == std::string_view::npos ? std::string_view{}
                                          : key.substr(open);
}

/// Vanilla heightmap packing: nine bits per column entry, seven entries per
/// word, 256 entries per chunk. The raw value is the first cell above the
/// column's surface relative to the chunk's minimum Y.
constexpr std::uint8_t heightmap_bits = 9;
constexpr std::size_t heightmap_entries = 256;
constexpr std::size_t heightmap_per_word = 64U / heightmap_bits;
constexpr std::size_t heightmap_words =
    (heightmap_entries + heightmap_per_word - 1) / heightmap_per_word;

[[nodiscard]] auto heightmap_value(std::span<const std::uint64_t> words,
                                   std::size_t index) noexcept
    -> std::uint32_t {
    const auto shift{(index % heightmap_per_word) * heightmap_bits};
    return static_cast<std::uint32_t>(
        (words[index / heightmap_per_word] >> shift) &
        ((std::uint64_t{1} << heightmap_bits) - 1U));
}

void heightmap_set(std::vector<std::uint64_t> &words, std::size_t index,
                   std::uint32_t value) noexcept {
    const auto shift{(index % heightmap_per_word) * heightmap_bits};
    const auto mask{((std::uint64_t{1} << heightmap_bits) - 1U) << shift};
    auto &word{words[index / heightmap_per_word]};
    word =
        (word & ~mask) | ((static_cast<std::uint64_t>(value) << shift) & mask);
}

/// Which vanilla heightmap a stored record holds. The storage model carries no
/// block tags, so the four predicates are derived from the collision
/// classification: a solid block blocks motion and counts as ground, a fluid
/// blocks motion but is not ground, and a leaf block is a motion-blocking block
/// that the leaf-less map excludes.
enum class heightmap_kind : std::uint8_t {
    world_surface,
    ocean_floor,
    motion_blocking,
    motion_blocking_no_leaves,
};

[[nodiscard]] auto heightmap_kind_for(std::string_view name) noexcept
    -> std::optional<heightmap_kind> {
    if (name == "WORLD_SURFACE") {
        return heightmap_kind::world_surface;
    }
    if (name == "OCEAN_FLOOR") {
        return heightmap_kind::ocean_floor;
    }
    if (name == "MOTION_BLOCKING") {
        return heightmap_kind::motion_blocking;
    }
    if (name == "MOTION_BLOCKING_NO_LEAVES") {
        return heightmap_kind::motion_blocking_no_leaves;
    }
    return std::nullopt;
}

/// Wrap a rotation into (-180, 180], the range the movement packet reports.
[[nodiscard]] auto wrap_degrees(float value) noexcept -> float {
    auto wrapped{std::fmod(value, 360.0F)};
    if (wrapped > 180.0F) {
        wrapped -= 360.0F;
    } else if (wrapped <= -180.0F) {
        wrapped += 360.0F;
    }
    return wrapped;
}

/// Collision shape heights the runtime understands besides a full cube. The
/// storage model has no `VoxelShape` table, so the classification is derived
/// from the block-state key and covers the shapes step-up exists for.
constexpr double snow_layer_height = 0.125;
constexpr double carpet_height = 0.0625;

[[nodiscard]] auto is_passable_block(std::string_view name) noexcept -> bool {
    for (const auto *passable : {"minecraft:air",
                                 "minecraft:cave_air",
                                 "minecraft:void_air",
                                 "minecraft:water",
                                 "minecraft:lava",
                                 "minecraft:short_grass",
                                 "minecraft:tall_grass",
                                 "minecraft:fern",
                                 "minecraft:dead_bush",
                                 "minecraft:torch",
                                 "minecraft:wall_torch",
                                 "minecraft:soul_torch",
                                 "minecraft:sapling",
                                 "minecraft:oak_sapling",
                                 "minecraft:birch_sapling",
                                 "minecraft:spruce_sapling",
                                 "minecraft:jungle_sapling",
                                 "minecraft:acacia_sapling",
                                 "minecraft:dark_oak_sapling",
                                 "minecraft:red_mushroom",
                                 "minecraft:brown_mushroom",
                                 "minecraft:sugar_cane",
                                 "minecraft:kelp",
                                 "minecraft:kelp_plant",
                                 "minecraft:seagrass",
                                 "minecraft:tall_seagrass",
                                 "minecraft:lever",
                                 "minecraft:ladder",
                                 "minecraft:vine",
                                 "minecraft:scaffolding",
                                 "minecraft:cobweb",
                                 "minecraft:tripwire",
                                 "minecraft:tripwire_hook",
                                 "minecraft:rail",
                                 "minecraft:torchflower",
                                 "minecraft:dandelion",
                                 "minecraft:poppy",
                                 "minecraft:blue_orchid",
                                 "minecraft:allium",
                                 "minecraft:azure_bluet",
                                 "minecraft:oxeye_daisy",
                                 "minecraft:cornflower",
                                 "minecraft:lily_of_the_valley"}) {
        if (name == passable) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto is_climbable_block(std::string_view name) noexcept -> bool {
    for (const auto *climbable :
         {"minecraft:ladder", "minecraft:vine", "minecraft:scaffolding",
          "minecraft:twisting_vines", "minecraft:weeping_vines",
          "minecraft:cave_vines", "minecraft:cave_vines_plant"}) {
        if (name == climbable) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto is_fluid_block(std::string_view name) noexcept
    -> world::fluid_kind {
    if (name == "minecraft:water" || name == "minecraft:flowing_water") {
        return world::fluid_kind::water;
    }
    if (name == "minecraft:lava" || name == "minecraft:flowing_lava") {
        return world::fluid_kind::lava;
    }
    return world::fluid_kind::none;
}

/// Binary-searched single-axis sweep. `collides` answers whether an AABB at a
/// position overlaps a collision box. Returns true when the axis was blocked.
template <typename Collides>
[[nodiscard]] auto sweep_coordinate(double &x, double &y, double &z,
                                    double delta, int axis, Collides &&collides)
    -> bool {
    if (delta == 0.0) {
        return false;
    }
    auto &coordinate{axis == 0 ? x : (axis == 1 ? y : z)};
    const auto original{coordinate};
    coordinate += delta;
    if (!collides(x, y, z)) {
        return false;
    }
    // The residual gap is deterministic and far below a block; 40 iterations
    // reach double resolution on the 1-block scale the sweep works on.
    double low{0.0};
    double high{1.0};
    for (int iteration{0}; iteration < 40; ++iteration) {
        const auto middle{(low + high) * 0.5};
        coordinate = original + delta * middle;
        if (collides(x, y, z)) {
            high = middle;
        } else {
            low = middle;
        }
    }
    coordinate = original + delta * low;
    return true;
}
} // namespace

void world::update_heightmaps(chunk &column, const block_pos &pos,
                              block_state_id value) noexcept {
    if (column.storage.heightmaps.empty()) {
        // A column that never carried heightmaps is sent with the encoder's
        // computed fallback; there is no stored map to move.
        return;
    }
    const auto blocks = [this](heightmap_kind kind, block_state_id state) {
        const auto name{state_name(registry_.key(state))};
        const auto air{name == "minecraft:air" ||
                       name == "minecraft:cave_air" ||
                       name == "minecraft:void_air"};
        if (kind == heightmap_kind::world_surface) {
            return !air;
        }
        const auto &collision{collision_of(state)};
        const auto fluid{collision.fluid != fluid_kind::none};
        const auto solid{!collision.passable};
        const auto leaf{name.size() >= 7 && name.ends_with("_leaves")};
        switch (kind) {
        case heightmap_kind::ocean_floor:
            // The material predicate counts blocks that block motion; a fluid
            // is not in the tag and the world surface ignores it here.
            return solid;
        case heightmap_kind::motion_blocking:
            return solid || fluid;
        case heightmap_kind::motion_blocking_no_leaves:
            return !leaf && (solid || fluid);
        case heightmap_kind::world_surface:
            break;
        }
        return false;
    };
    const auto edge{world_ns::section_edge};
    const auto local_x{static_cast<std::size_t>(pos.x - column.pos.x * edge)};
    const auto local_z{static_cast<std::size_t>(pos.z - column.pos.z * edge)};
    if (local_x >= static_cast<std::size_t>(edge) ||
        local_z >= static_cast<std::size_t>(edge)) {
        return;
    }
    const auto index{local_z * static_cast<std::size_t>(edge) + local_x};
    const auto min_y{column.y_pos * edge};
    const auto y{static_cast<std::int32_t>(pos.y)};
    for (auto &map : column.storage.heightmaps) {
        const auto kind{heightmap_kind_for(map.name)};
        if (!kind.has_value() || map.bits_per_entry != heightmap_bits ||
            map.data.size() != heightmap_words) {
            continue;
        }
        // Raw values are relative to the chunk's minimum Y, exactly like the
        // reference heightmap storage.
        const auto first_available{
            static_cast<std::int32_t>(heightmap_value(map.data, index)) +
            min_y};
        const auto becomes_blocking{blocks(*kind, value)};
        if (y <= first_available - 2) {
            // Too far below the surface to move it in either direction.
            continue;
        }
        if (becomes_blocking) {
            if (y >= first_available) {
                heightmap_set(map.data, index,
                              static_cast<std::uint32_t>(y + 1 - min_y));
            }
            continue;
        }
        if (first_available - 1 != y) {
            // The edited cell was not the surface block.
            continue;
        }
        // The top block became transparent: rescan the column downwards for
        // the next blocking cell, exactly like the reference update.
        auto new_first{min_y};
        for (auto below{y - 1}; below >= min_y; --below) {
            const auto below_state{block_at(
                block_pos{pos.x, static_cast<std::int16_t>(below), pos.z})};
            if (blocks(*kind, below_state)) {
                new_first = below + 1;
                break;
            }
        }
        heightmap_set(map.data, index,
                      static_cast<std::uint32_t>(new_first - min_y));
    }
}

auto world::collision_of(block_state_id id) const noexcept
    -> const block_collision & {
    if (id >= collisions_.size()) {
        collisions_.resize(static_cast<std::size_t>(id) + 1U);
    }
    auto &entry{collisions_[id]};
    if (entry.resolved) {
        return entry;
    }
    entry.resolved = true;
    entry.passable = true;
    entry.box_min_y = 0.0;
    entry.box_max_y = 1.0;
    entry.fluid = fluid_kind::none;
    entry.climbable = false;
    entry.friction = 0.6;
    const auto key{registry_.key(id)};
    const auto name{state_name(key)};
    const auto properties{state_properties(key)};
    entry.fluid = is_fluid_block(name);
    entry.climbable = is_climbable_block(name);
    if (entry.fluid != fluid_kind::none || is_passable_block(name)) {
        return entry;
    }
    if (name == "minecraft:snow") {
        // Snow layers collide up to the layer height; the state encodes the
        // layer count in `layers`, which the approximation ignores.
        entry.passable = false;
        entry.box_max_y = snow_layer_height;
        return entry;
    }
    const auto slab{name.size() >= 5 &&
                    name.substr(name.size() - 5) == "_slab"};
    if (slab) {
        entry.passable = false;
        entry.box_min_y = 0.0;
        entry.box_max_y = 0.5;
        if (properties.find("type=top") != std::string_view::npos) {
            entry.box_min_y = 0.5;
            entry.box_max_y = 1.0;
        }
        return entry;
    }
    const auto carpet{name.size() >= 7 &&
                      name.substr(name.size() - 7) == "_carpet"};
    if (carpet) {
        entry.passable = false;
        entry.box_max_y = carpet_height;
        return entry;
    }
    if (name == "minecraft:ice" || name == "minecraft:packed_ice" ||
        name == "minecraft:blue_ice" || name == "minecraft:frosted_ice") {
        entry.passable = false;
        entry.friction = 0.98;
        return entry;
    }
    // Everything else is a full collision cube.
    // TODO: derive the collision boxes from the block-state table; the
    // approximation only over-restricts movement (fences, walls and stairs
    // collide as full blocks) and never lets a player through a solid block.
    entry.passable = false;
    return entry;
}

auto world::block_solid(block_state_id id) const noexcept -> bool {
    return !collision_of(id).passable;
}

auto world::player_overlaps_solid(const player_state &player) const noexcept
    -> bool {
    return player_box_collides(player_box(player.x, player.y, player.z));
}

auto world::block_fluid(block_state_id id) const noexcept -> fluid_kind {
    return collision_of(id).fluid;
}

auto world::block_climbable(block_state_id id) const noexcept -> bool {
    return collision_of(id).climbable;
}

auto world::block_friction(block_state_id id) const noexcept -> double {
    return collision_of(id).friction;
}

auto world::player_box(double x, double y, double z) noexcept -> player_aabb {
    return player_aabb{.min_x = x - player_half_width,
                       .min_y = y,
                       .min_z = z - player_half_width,
                       .max_x = x + player_half_width,
                       .max_y = y + player_height,
                       .max_z = z + player_half_width};
}

auto world::player_box_collides(const player_aabb &box) const noexcept -> bool {
    // Vertical body coordinates can lie outside the modeled build range (the
    // void, a console command, a modified client). Block lookups only exist
    // inside that range and the narrowing conversion of a huge double is
    // undefined, so the Y rows are derived through the clamped block helper: a
    // body above or below the range overlaps no block.
    const auto min_y{static_cast<std::int32_t>(block_y_of(box.min_y))};
    const auto max_y{
        static_cast<std::int32_t>(block_y_of(box.max_y - collision_epsilon))};
    const auto min_x{static_cast<std::int32_t>(std::floor(box.min_x))};
    const auto max_x{
        static_cast<std::int32_t>(std::floor(box.max_x - collision_epsilon))};
    const auto min_z{static_cast<std::int32_t>(std::floor(box.min_z))};
    const auto max_z{
        static_cast<std::int32_t>(std::floor(box.max_z - collision_epsilon))};
    for (std::int32_t y{min_y}; y <= max_y; ++y) {
        for (std::int32_t z{min_z}; z <= max_z; ++z) {
            for (std::int32_t x{min_x}; x <= max_x; ++x) {
                const auto &shape{collision_of(
                    block_at(block_pos{x, static_cast<std::int16_t>(y), z}))};
                if (shape.passable) {
                    continue;
                }
                if (box.min_x < static_cast<double>(x) + 1.0 &&
                    box.max_x > static_cast<double>(x) &&
                    box.min_y < static_cast<double>(y) + shape.box_max_y &&
                    box.max_y > static_cast<double>(y) + shape.box_min_y &&
                    box.min_z < static_cast<double>(z) + 1.0 &&
                    box.max_z > static_cast<double>(z)) {
                    return true;
                }
            }
        }
    }
    return false;
}

auto world::player_box_grounded(const player_aabb &box) const noexcept -> bool {
    const auto min_x{static_cast<std::int32_t>(std::floor(box.min_x))};
    const auto max_x{
        static_cast<std::int32_t>(std::floor(box.max_x - collision_epsilon))};
    const auto min_z{static_cast<std::int32_t>(std::floor(box.min_z))};
    const auto max_z{
        static_cast<std::int32_t>(std::floor(box.max_z - collision_epsilon))};
    const auto support_y{
        static_cast<std::int32_t>(block_y_of(box.min_y - collision_epsilon))};
    const auto top{static_cast<double>(support_y)};
    for (std::int32_t z{min_z}; z <= max_z; ++z) {
        for (std::int32_t x{min_x}; x <= max_x; ++x) {
            const auto &shape{collision_of(block_at(
                block_pos{x, static_cast<std::int16_t>(support_y), z}))};
            if (shape.passable) {
                continue;
            }
            const auto block_top{top + shape.box_max_y};
            if (block_top > box.min_y - 1.0e-4 &&
                block_top <= box.min_y + 1.0e-4) {
                return true;
            }
        }
    }
    return false;
}

auto world::resolve_movement(double x, double y, double z, double dx, double dy,
                             double dz, bool was_on_ground) const noexcept
    -> movement_resolution {
    movement_resolution out{.x = x, .y = y, .z = z};
    const auto collides = [this](double box_x, double box_y, double box_z) {
        return player_box_collides(player_box(box_x, box_y, box_z));
    };
    out.blocked_y = sweep_coordinate(out.x, out.y, out.z, dy, 1, collides);
    out.blocked_x = sweep_coordinate(out.x, out.y, out.z, dx, 0, collides);
    out.blocked_z = sweep_coordinate(out.x, out.y, out.z, dz, 2, collides);

    // Step-up: a blocked horizontal move retries from a raised position when
    // the player stands on the ground (or just landed). The candidate heights
    // are the partial collision shapes the block classification knows.
    const auto on_ground_after{was_on_ground || (out.blocked_y && dy <= 0.0)};
    if (on_ground_after && (out.blocked_x || out.blocked_z)) {
        const auto base_progress{std::hypot(out.x - x, out.z - z)};
        const auto remaining_x{dx - (out.x - x)};
        const auto remaining_z{dz - (out.z - z)};
        for (const auto lift :
             {player_step_height, snow_layer_height, carpet_height}) {
            double lifted_x{out.x};
            double lifted_y{out.y + lift};
            double lifted_z{out.z};
            if (collides(lifted_x, lifted_y, lifted_z)) {
                continue;
            }
            const auto lifted_blocked_x{sweep_coordinate(
                lifted_x, lifted_y, lifted_z, remaining_x, 0, collides)};
            const auto lifted_blocked_z{sweep_coordinate(
                lifted_x, lifted_y, lifted_z, remaining_z, 2, collides)};
            static_cast<void>(lifted_blocked_x);
            static_cast<void>(lifted_blocked_z);
            const auto progress{std::hypot(lifted_x - out.x, lifted_z - out.z)};
            if (progress <= base_progress + 1.0e-4) {
                continue;
            }
            // Settle down onto whatever the raised move found, so stepping
            // onto a slab leaves the feet on its top and not above it.
            static_cast<void>(sweep_coordinate(lifted_x, lifted_y, lifted_z,
                                               -lift, 1, collides));
            out.x = lifted_x;
            out.y = lifted_y;
            out.z = lifted_z;
            out.blocked_x = false;
            out.blocked_z = false;
            break;
        }
    }
    out.grounded = player_box_grounded(player_box(out.x, out.y, out.z));
    return out;
}

auto world::resolve_player_move(const player_state &player, double dx,
                                double dy, double dz) const noexcept
    -> player_collision_result {
    const auto resolved{resolve_movement(player.x, player.y, player.z, dx, dy,
                                         dz, player.on_ground)};
    return player_collision_result{
        .x = resolved.x,
        .y = resolved.y,
        .z = resolved.z,
        .collided = resolved.blocked_x || resolved.blocked_y ||
                    resolved.blocked_z || resolved.grounded,
    };
}

auto world::move_player(player_state &player, double dx, double dy, double dz)
    -> bool {
    const auto start_x{player.x};
    const auto start_y{player.y};
    const auto start_z{player.z};
    const auto resolved{resolve_movement(player.x, player.y, player.z, dx, dy,
                                         dz, player.on_ground)};
    player.x = resolved.x;
    player.y = resolved.y;
    player.z = resolved.z;
    if (resolved.blocked_y) {
        player.vy = 0.0;
        if (dy < 0.0) {
            player.on_ground = true;
        }
    }
    if (resolved.blocked_x) {
        player.vx = 0.0;
    }
    if (resolved.blocked_z) {
        player.vz = 0.0;
    }
    if (resolved.grounded) {
        player.on_ground = true;
    } else if (dy > 0.0 || resolved.blocked_y) {
        player.on_ground = false;
    } else if (dy != 0.0) {
        player.on_ground = false;
    }
    return player.x != start_x || player.y != start_y || player.z != start_z;
}

auto world::submit_player_move(player_state &player,
                               const player_move_request &request)
    -> player_move_verdict {
    if (player.dead) {
        // A dead client keeps sending movement while the death screen is up;
        // the record stays where it died, so the request is dropped without a
        // correction packet.
        return player_move_verdict::accepted;
    }
    if (!std::isfinite(request.x) || !std::isfinite(request.y) ||
        !std::isfinite(request.z) || !std::isfinite(request.yaw) ||
        !std::isfinite(request.pitch)) {
        return player_move_verdict::corrected;
    }
    // Anchor the report on the position of the previous accepted report when
    // one is still pending. A late server tick or a batched connection can
    // deliver several one-tick reports in the same tick; each one describes
    // the movement since the report before it, so comparing it with the
    // record (which only advances once per tick) would turn a legitimate
    // batched flight into teleport-sized displacements.
    const auto anchor_x{player.has_move_request ? player.move_x : player.x};
    const auto anchor_y{player.has_move_request ? player.move_y : player.y};
    const auto anchor_z{player.has_move_request ? player.move_z : player.z};
    const auto dx{request.x - anchor_x};
    const auto dy{request.y - anchor_y};
    const auto dz{request.z - anchor_z};
    const auto distance{std::sqrt(dx * dx + dy * dy + dz * dz)};
    if (distance > player_max_move_distance) {
        return player_move_verdict::corrected;
    }
    const auto spectator{player.game_mode == game_mode_spectator};
    if (!spectator) {
        // The reported body itself must be collision-free. The sweep
        // comparison below only bounds how far the report may end from the
        // collision-resolved position, so a request that stops exactly at the
        // tolerance boundary inside a wall would be accepted and become the
        // next anchor; repeating it walks the body through the wall. A
        // request whose own box overlaps a solid block is never a position the
        // server can stand at.
        if (player_box_collides(player_box(request.x, request.y, request.z))) {
            return player_move_verdict::corrected;
        }
        const auto feet{block_at(
            block_pos{static_cast<std::int32_t>(std::floor(anchor_x)),
                      block_y_of(anchor_y + 0.1),
                      static_cast<std::int32_t>(std::floor(anchor_z))})};
        const auto body{block_at(
            block_pos{static_cast<std::int32_t>(std::floor(anchor_x)),
                      block_y_of(anchor_y + 0.9),
                      static_cast<std::int32_t>(std::floor(anchor_z))})};
        const auto in_fluid{block_fluid(feet) != fluid_kind::none ||
                            block_fluid(body) != fluid_kind::none};
        const auto climbing{block_climbable(feet) || block_climbable(body)};
        if (!player.flying && !in_fluid) {
            if (std::hypot(dx, dz) > player_max_horizontal_per_tick) {
                return player_move_verdict::corrected;
            }
        }
        if (!player.flying && !in_fluid && !climbing &&
            dy > player_max_upward_per_tick) {
            return player_move_verdict::corrected;
        }
        if (!player.flying && !in_fluid && !climbing && dy > 0.0 &&
            request.y > player.ground_y + player_max_jump_height) {
            return player_move_verdict::corrected;
        }
        // The collision sweep must be able to carry the player to the
        // reported position. A request that ends inside a wall or past the
        // step height is a modified client, not movement.
        const auto resolved{resolve_movement(anchor_x, anchor_y, anchor_z, dx,
                                             dy, dz, player.on_ground)};
        const auto miss_x{resolved.x - request.x};
        const auto miss_y{resolved.y - request.y};
        const auto miss_z{resolved.z - request.z};
        if (std::sqrt(miss_x * miss_x + miss_y * miss_y + miss_z * miss_z) >
            player_move_tolerance) {
            return player_move_verdict::corrected;
        }
    }
    player.has_move_request = true;
    player.move_x = request.x;
    player.move_y = request.y;
    player.move_z = request.z;
    player.move_on_ground = request.on_ground;
    player.yaw = wrap_degrees(request.yaw);
    player.pitch = std::clamp(request.pitch, -90.0F, 90.0F);
    return player_move_verdict::accepted;
}

auto world::damage_player(player_state &player, damage_source source,
                          float amount, std::uint64_t attacker) -> bool {
    return sim::damage_player(*this, player, source, amount, attacker);
}

void world::respawn_player(player_state &player, double x, double y, double z,
                           float yaw, float pitch) noexcept {
    sim::respawn_player(*this, player, x, y, z, yaw, pitch);
}

void world::set_spawn_point(spawn_point value) noexcept { spawn_ = value; }

void world::set_difficulty(std::uint32_t value) noexcept {
    difficulty_ = std::min<std::uint32_t>(value, 3U);
}

void world::set_hardcore(bool value) noexcept { hardcore_ = value; }

void world::tick_player(player_state &player) noexcept {
    sim::tick_player(*this, player);
}

void world::jump(player_state &player) noexcept {
    if (!player.on_ground) {
        return;
    }
    player.vy = player_jump_velocity;
    player.on_ground = false;
    player.exhaustion = std::min(player.exhaustion + 0.05F, 40.0F);
}

auto world::state_hash() const -> data::digest32 {
    std::vector<std::byte> bytes{};
    bytes.reserve(64 + chunks_.size() * (32 + world_ns::section_volume * 2));
    const auto append_u32 = [&bytes](std::uint32_t value) {
        for (unsigned shift{0}; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    };
    const auto append_u64 = [&bytes](std::uint64_t value) {
        for (unsigned shift{0}; shift < 64; shift += 8) {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    };
    const auto append_key = [this, &bytes, &append_u32](block_state_id id) {
        const auto key{registry_.key(id)};
        append_u32(static_cast<std::uint32_t>(key.size()));
        for (const char character : key) {
            bytes.push_back(static_cast<std::byte>(character));
        }
    };
    append_u64(static_cast<std::uint64_t>(tick_));
    append_u64(rng_.state0());
    append_u64(rng_.state1());
    const auto append_double = [&append_u64](double value) {
        append_u64(std::bit_cast<std::uint64_t>(value));
    };
    // Every player record in ascending entity id order. The key is the entity
    // id, not the insertion order; identity fields are excluded because they
    // are not simulation state.
    append_u64(static_cast<std::uint64_t>(entities_->players().size()));
    for (const auto &[id, player] : entities_->players().records()) {
        append_u64(id);
        append_double(player.x);
        append_double(player.y);
        append_double(player.z);
        append_double(player.vx);
        append_double(player.vy);
        append_double(player.vz);
        append_u32(std::bit_cast<std::uint32_t>(player.yaw));
        append_u32(std::bit_cast<std::uint32_t>(player.pitch));
        append_u32(player.on_ground ? 1U : 0U);
        append_u32(player.game_mode);
        append_u32(player.selected_slot);
        append_u32(std::bit_cast<std::uint32_t>(player.health));
        append_u32(static_cast<std::uint32_t>(player.food_level));
        append_u32(std::bit_cast<std::uint32_t>(player.saturation));
        append_u32(std::bit_cast<std::uint32_t>(player.exhaustion));
        append_u32(static_cast<std::uint32_t>(player.air_supply));
        append_u32(static_cast<std::uint32_t>(player.experience_level));
        append_u32(static_cast<std::uint32_t>(player.total_experience));
        append_u32(player.dead ? 1U : 0U);
        append_u64(std::bit_cast<std::uint64_t>(player.fall_distance));
        append_u32(static_cast<std::uint32_t>(player.input));
        append_u32(player.flying ? 1U : 0U);
        append_u32(player.client_driven ? 1U : 0U);
        append_u32(static_cast<std::uint32_t>(player.age));
        append_u32(static_cast<std::uint32_t>(player.inventory.size()));
        for (const auto &stack : player.inventory) {
            append_u32(static_cast<std::uint32_t>(stack.item.size()));
            for (const char character : stack.item) {
                bytes.push_back(static_cast<std::byte>(character));
            }
            append_u32(stack.count);
            append_u32(static_cast<std::uint32_t>(stack.components.size()));
            for (const auto &[kind, value] : stack.components) {
                append_u32(static_cast<std::uint32_t>(kind));
                std::visit(
                    [&bytes, &append_u32](const auto &component) {
                        using type = std::decay_t<decltype(component)>;
                        if constexpr (std::is_same_v<type, component_damage>) {
                            append_u32(component.amount);
                        } else if constexpr (std::is_same_v<
                                                 type, component_custom_name>) {
                            append_u32(static_cast<std::uint32_t>(
                                component.text.size()));
                            for (const char character : component.text) {
                                bytes.push_back(
                                    static_cast<std::byte>(character));
                            }
                        } else {
                            append_u32(static_cast<std::uint32_t>(
                                component.entries.size()));
                            for (const auto &entry : component.entries) {
                                append_u32(static_cast<std::uint32_t>(
                                    entry.enchantment.size()));
                                for (const char character : entry.enchantment) {
                                    bytes.push_back(
                                        static_cast<std::byte>(character));
                                }
                                append_u32(entry.level);
                            }
                        }
                    },
                    value);
            }
        }
    }
    // Every runtime entity in ascending logical id order. The registry's own
    // creation order never reaches the hash.
    const auto &entity_registry{entities_->registry()};
    append_u64(static_cast<std::uint64_t>(entities_->size()));
    for (const auto handle : entities_->order()) {
        const auto &identity{
            entity_registry.get<detail::component::identity>(handle)};
        const auto &value{
            entity_registry.get<detail::component::transform>(handle)};
        const auto &body_rotation{
            entity_registry.get<detail::component::rotation>(handle)};
        append_u64(identity.id);
        append_u32(static_cast<std::uint32_t>(identity.type.size()));
        for (const char character : identity.type) {
            bytes.push_back(static_cast<std::byte>(character));
        }
        append_double(value.x);
        append_double(value.y);
        append_double(value.z);
        append_double(value.vx);
        append_double(value.vy);
        append_double(value.vz);
        append_u32(std::bit_cast<std::uint32_t>(body_rotation.yaw));
        append_u32(std::bit_cast<std::uint32_t>(body_rotation.pitch));
        append_u32(value.on_ground ? 1U : 0U);
    }
    append_u64(static_cast<std::uint64_t>(chunks_.size()));
    for (const auto &[pos, column] : chunks_) {
        append_u32(static_cast<std::uint32_t>(pos.x));
        append_u32(static_cast<std::uint32_t>(pos.z));
        append_u32(static_cast<std::uint32_t>(column.sections.size()));
        for (const auto &section : column.sections) {
            append_u32(static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(section.y)));
            append_u32(section.uniform ? 1U : 0U);
            if (section.uniform) {
                append_key(section.value);
            } else {
                append_u32(static_cast<std::uint32_t>(section.blocks.size()));
                for (const auto block : section.blocks) {
                    append_key(block);
                }
            }
        }
    }
    return data::blake3(bytes);
}

void world::unload_all() noexcept {
    despawn_all_column_entities();
    chunks_.clear();
    light_ = light_engine{*this};
}

auto world::store() noexcept -> store_type & { return store_ref(); }

auto world::store() const noexcept -> const store_type & { return store_ref(); }

} // namespace refrain::sim
