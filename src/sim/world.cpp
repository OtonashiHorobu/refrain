// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/world.hpp>

#include <refrain/data/checksum.hpp>
#include <refrain/world/model.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
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

world::world(store_type store) : store_{std::move(store)} {}

world::world(world &&other) noexcept
    : store_{std::move(other.store_)}, registry_{std::move(other.registry_)},
      chunks_{std::move(other.chunks_)}, tickets_{std::move(other.tickets_)},
      player_{std::move(other.player_)}, entities_{std::move(other.entities_)},
      next_entity_id_{other.next_entity_id_}, rng_{other.rng_},
      tick_{other.tick_} {}

auto world::operator=(world &&other) noexcept -> world & {
    if (this != &other) {
        store_ = std::move(other.store_);
        registry_ = std::move(other.registry_);
        chunks_ = std::move(other.chunks_);
        tickets_ = std::move(other.tickets_);
        player_ = std::move(other.player_);
        entities_ = std::move(other.entities_);
        next_entity_id_ = other.next_entity_id_;
        rng_ = other.rng_;
        tick_ = other.tick_;
    }
    return *this;
}

world::~world() = default;

auto world::open(std::string_view path) -> result<world> {
    auto store{store_type::open(path)};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    return world{std::move(*store)};
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

auto world::ensure_chunk(const chunk_pos &pos) -> result<chunk *> {
    if (auto *found{find_chunk(pos)}; found != nullptr) {
        return found;
    }
    auto loaded{[this, &pos] {
        const std::lock_guard lock{store_mutex_};
        return store_.read_chunk(pos);
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
    if (pos.y < world_ns::min_section_y * world_ns::section_edge ||
        pos.y > (world_ns::max_section_y + 1) * world_ns::section_edge - 1) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("block y={} is outside the runtime section range",
                        pos.y))};
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
    found->dirty = true;
    (*column)->dirty = true;
    ++(*column)->revision;
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
            events_.publish(event);
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

auto world::add_ticket(const chunk_pos &pos, ticket_kind /*kind*/) -> status {
    auto column{ensure_chunk(pos)};
    if (!column) {
        return std::unexpected{std::move(column.error())};
    }
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
    // (not truncation) keeps negative coordinates on the correct chunk.
    const auto player_x{
        std::floor(player_.x / static_cast<double>(world_ns::section_edge))};
    const auto player_z{
        std::floor(player_.z / static_cast<double>(world_ns::section_edge))};
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
        return store_.commit();
    }
    for (const auto &entry : snapshot) {
        auto written{store_.write_chunk(entry.pos, entry.chunk, entry.level)};
        if (!written) {
            return written;
        }
    }
    return store_.commit();
}

auto world::compact_stores() -> result<::refrain::world::world_compact> {
    const std::lock_guard lock{store_mutex_};
    return store_.compact();
}

auto world::compact_regions(std::uint64_t min_bytes, unsigned min_percent)
    -> result<std::size_t> {
    const std::lock_guard lock{store_mutex_};
    return store_.compact_regions(min_bytes, min_percent);
}

auto world::rebuild_palette_in_place(unsigned threads, int zstd_level)
    -> result<::refrain::world::palette_maintenance_report> {
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
    const std::string root{store_.root()};
    if (root.empty()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    const int store_level{store_.zstd_level()};

    // The rebuilt world has a fresh palette table, so every runtime chunk must
    // be decoded again (its `section.base` would otherwise carry stale palette
    // ids). Reload the previously loaded chunks and every ticketed position so
    // the runtime view and `state_hash()` survive the swap.
    std::vector<chunk_pos> positions;
    positions.reserve(chunks_.size() + tickets_.size());
    for (const auto &[pos, column] : chunks_) {
        static_cast<void>(column);
        positions.push_back(pos);
    }
    for (const auto &[pos, count] : tickets_) {
        static_cast<void>(count);
        if (!chunks_.contains(pos)) {
            positions.push_back(pos);
        }
    }
    const auto order = [](const chunk_pos &left, const chunk_pos &right) {
        if (left.x != right.x) {
            return left.x < right.x;
        }
        return left.z < right.z;
    };
    std::sort(positions.begin(), positions.end(), order);

    // Release every region handle before the helper renames the directory.
    store_ = store_type{};
    chunks_.clear();

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
    if (auto level{store_.set_zstd_level(store_level)}; !level) {
        return std::unexpected{std::move(level.error())};
    }
    for (const auto &pos : positions) {
        auto loaded{ensure_chunk(pos)};
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
    if (snapshot->empty()) {
        const std::lock_guard lock{store_mutex_};
        return store_.commit();
    }
    auto written{write_snapshot(*snapshot)};
    if (!written) {
        return written;
    }
    mark_clean(*snapshot);
    return status_ok();
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

auto world::player() noexcept -> player_state & { return player_; }

auto world::player() const noexcept -> const player_state & { return player_; }

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
    tick_player();
    tick_entities();
    ++tick_;
    (void)rng_.next_u64();
    // Notification listeners are mod/extension code: a throw must never take
    // down the sim tick, and the bus contract is single-threaded.
    try {
        tick_event event{.tick = tick_};
        events_.publish(event);
    } catch (...) {
        // The listener failed; the tick itself completed.
    }
    // Timers due at or before this tick run in (due, id) order. A timer that
    // schedules another zero-delay timer is picked up by the same loop, so the
    // ordering stays deterministic and bounded by the tasks themselves.
    while (true) {
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

void world::seed(std::uint64_t value) noexcept { rng_.seed(value); }

auto world::random() noexcept -> rng & { return rng_; }

auto world::random() const noexcept -> const rng & { return rng_; }

void world::set_player_position(double x, double y, double z) noexcept {
    player_.x = x;
    player_.y = y;
    player_.z = z;
    player_.vx = 0.0;
    player_.vy = 0.0;
    player_.vz = 0.0;
    player_.on_ground = false;
}

auto world::add_item(std::string_view item, std::uint32_t count) -> status {
    if (item.empty() || count == 0) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "inventory item/count is empty")};
    }
    const auto current{item_count(item)};
    if (static_cast<std::uint64_t>(current) + count >
        std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "inventory item count overflows")};
    }
    auto remaining{count};
    for (auto &stack : player_.inventory) {
        if (remaining == 0) {
            break;
        }
        if (stack.item != item || stack.count >= max_stack_size) {
            continue;
        }
        const auto space{max_stack_size - stack.count};
        const auto added{std::min(space, remaining)};
        stack.count += added;
        remaining -= added;
    }
    while (remaining != 0) {
        const auto added{std::min(max_stack_size, remaining)};
        player_.inventory.push_back(
            item_stack{.item = std::string{item}, .count = added});
        remaining -= added;
    }
    return status_ok();
}

auto world::remove_item(std::string_view item, std::uint32_t count) -> bool {
    if (count == 0) {
        return true;
    }
    if (item_count(item) < count) {
        return false;
    }
    auto remaining{count};
    for (auto iterator{player_.inventory.begin()};
         iterator != player_.inventory.end();) {
        if (iterator->item != item) {
            ++iterator;
            continue;
        }
        const auto removed{std::min(iterator->count, remaining)};
        iterator->count -= removed;
        remaining -= removed;
        if (iterator->count == 0) {
            iterator = player_.inventory.erase(iterator);
        } else {
            ++iterator;
        }
        if (remaining == 0) {
            break;
        }
    }
    return true;
}

auto world::item_count(std::string_view item) const noexcept -> std::uint32_t {
    std::uint64_t total{0};
    for (const auto &stack : player_.inventory) {
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
    if (next_entity_id_ == std::numeric_limits<entity_id>::max()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "entity id space exhausted")};
    }
    const auto id{next_entity_id_++};
    entities_.emplace(
        id, entity_state{
                .id = id, .type = std::string{type}, .x = x, .y = y, .z = z});
    return id;
}

auto world::remove_entity(entity_id id) -> bool {
    return entities_.erase(id) != 0U;
}

auto world::entity(entity_id id) noexcept -> entity_state * {
    const auto found{entities_.find(id)};
    return found == entities_.end() ? nullptr : &found->second;
}

auto world::entity(entity_id id) const noexcept -> const entity_state * {
    const auto found{entities_.find(id)};
    return found == entities_.end() ? nullptr : &found->second;
}

void world::tick_entities() noexcept {
    constexpr double gravity{0.08};
    constexpr double vertical_drag{0.98};
    constexpr double horizontal_drag{0.91};
    for (auto &[id, value] : entities_) {
        static_cast<void>(id);
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
            const auto start{static_cast<std::int32_t>(std::floor(before_y))};
            const auto end{static_cast<std::int32_t>(std::floor(value.y))};
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

auto world::block_solid(block_state_id id) const noexcept -> bool {
    const auto key{registry_.key(id)};
    const auto name{key.substr(0, key.find('['))};
    // Approximation: air/fluids and the common zero-collision plants are
    // passable; every other state is solid. Full VoxelShape/collision tables
    // are not implemented yet and only affect player movement, not storage.
    for (const auto *passable :
         {"minecraft:air", "minecraft:cave_air", "minecraft:void_air",
          "minecraft:water", "minecraft:lava", "minecraft:short_grass",
          "minecraft:tall_grass", "minecraft:fern", "minecraft:dead_bush",
          "minecraft:torch"}) {
        if (name == passable) {
            return false;
        }
    }
    return true;
}

namespace {
constexpr double player_half_width = 0.3;
constexpr double player_height = 1.8;
constexpr double collision_epsilon = 1.0e-7;

struct aabb {
    double min_x;
    double min_y;
    double min_z;
    double max_x;
    double max_y;
    double max_z;
};

[[nodiscard]] auto player_box(double x, double y, double z) noexcept -> aabb {
    return aabb{x - player_half_width, y,
                z - player_half_width, x + player_half_width,
                y + player_height,     z + player_half_width};
}
} // namespace

auto world::move_player(double dx, double dy, double dz) -> bool {
    bool moved{false};
    const auto collides = [this](const aabb &box) {
        const auto min_x{static_cast<std::int32_t>(std::floor(box.min_x))};
        const auto max_x{static_cast<std::int32_t>(
            std::floor(box.max_x - collision_epsilon))};
        const auto min_y{static_cast<std::int32_t>(std::floor(box.min_y))};
        const auto max_y{static_cast<std::int32_t>(
            std::floor(box.max_y - collision_epsilon))};
        const auto min_z{static_cast<std::int32_t>(std::floor(box.min_z))};
        const auto max_z{static_cast<std::int32_t>(
            std::floor(box.max_z - collision_epsilon))};
        for (std::int32_t y{min_y}; y <= max_y; ++y) {
            for (std::int32_t z{min_z}; z <= max_z; ++z) {
                for (std::int32_t x{min_x}; x <= max_x; ++x) {
                    if (block_solid(block_at(
                            block_pos{x, static_cast<std::int16_t>(y), z}))) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    enum class axis { y, x, z };
    const auto move_axis = [this, &collides, &moved](double &position,
                                                     double delta, axis which) {
        if (delta == 0.0) {
            return;
        }
        const auto original{position};
        position += delta;
        auto box{player_box(player_.x, player_.y, player_.z)};
        if (!collides(box)) {
            moved = moved || position != original;
            return;
        }
        // Binary search for the furthest collision-free fraction. The small
        // residual gap is deterministic and well below a block.
        double low{0.0};
        double high{1.0};
        for (int iteration{0}; iteration < 32; ++iteration) {
            const auto middle{(low + high) * 0.5};
            position = original + delta * middle;
            box = player_box(player_.x, player_.y, player_.z);
            if (collides(box)) {
                high = middle;
            } else {
                low = middle;
            }
        }
        position = original + delta * low;
        if (which == axis::y) {
            player_.vy = 0.0;
            if (delta < 0.0) {
                player_.on_ground = true;
            }
        } else if (which == axis::x) {
            player_.vx = 0.0;
        } else {
            player_.vz = 0.0;
        }
        moved = moved || position != original;
    };

    move_axis(player_.y, dy, axis::y);
    move_axis(player_.x, dx, axis::x);
    move_axis(player_.z, dz, axis::z);
    return moved;
}

void world::tick_player() noexcept {
    // Gravity: vanilla-shaped enough for a deterministic player walk, not
    // a full LivingEntity/fluid model. The RNG draw in tick() is unrelated.
    constexpr double gravity{0.08};
    constexpr double vertical_drag{0.98};
    // Always probe downward: standing on a block is represented by the
    // collision clamp, so walking off an edge starts falling without an extra
    // "supported" flag.
    player_.vy = (player_.vy - gravity) * vertical_drag;
    const auto before{player_.y};
    (void)move_player(0.0, player_.vy, 0.0);
    if (player_.vy <= 0.0 && player_.y == before) {
        player_.on_ground = true;
    } else if (player_.vy != 0.0) {
        player_.on_ground = false;
    }
    player_.vx *= 0.91;
    player_.vz *= 0.91;
    if (player_.vx != 0.0 || player_.vz != 0.0) {
        (void)move_player(player_.vx, 0.0, player_.vz);
    }
}

void world::jump() noexcept {
    if (player_.on_ground) {
        player_.vy = 0.42;
        player_.on_ground = false;
    }
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
    append_double(player_.x);
    append_double(player_.y);
    append_double(player_.z);
    append_double(player_.vx);
    append_double(player_.vy);
    append_double(player_.vz);
    append_u32(std::bit_cast<std::uint32_t>(player_.yaw));
    append_u32(std::bit_cast<std::uint32_t>(player_.pitch));
    append_u32(player_.on_ground ? 1U : 0U);
    append_u32(static_cast<std::uint32_t>(player_.inventory.size()));
    for (const auto &stack : player_.inventory) {
        append_u32(static_cast<std::uint32_t>(stack.item.size()));
        for (const char character : stack.item) {
            bytes.push_back(static_cast<std::byte>(character));
        }
        append_u32(stack.count);
    }
    append_u64(static_cast<std::uint64_t>(entities_.size()));
    for (const auto &[id, value] : entities_) {
        append_u64(id);
        append_u32(static_cast<std::uint32_t>(value.type.size()));
        for (const char character : value.type) {
            bytes.push_back(static_cast<std::byte>(character));
        }
        append_double(value.x);
        append_double(value.y);
        append_double(value.z);
        append_double(value.vx);
        append_double(value.vy);
        append_double(value.vz);
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

void world::unload_all() noexcept { chunks_.clear(); }

auto world::store() noexcept -> store_type & { return store_; }

auto world::store() const noexcept -> const store_type & { return store_; }

} // namespace refrain::sim
