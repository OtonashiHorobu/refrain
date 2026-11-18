// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/proto_cache.hpp>

#include <algorithm>
#include <vector>

namespace refrain::worldgen {

namespace {

/// Locks the cache mutex when the cache is shared. The guard owns a reference
/// to a mutex the cache does not own, so it stays trivial to construct.
class cache_lock {
  public:
    explicit cache_lock(std::mutex *lock) noexcept : lock_{lock} {
        if (lock_ != nullptr) {
            lock_->lock();
        }
    }
    cache_lock(const cache_lock &) = delete;
    auto operator=(const cache_lock &) -> cache_lock & = delete;
    cache_lock(cache_lock &&) = delete;
    auto operator=(cache_lock &&) -> cache_lock & = delete;
    ~cache_lock() {
        if (lock_ != nullptr) {
            lock_->unlock();
        }
    }

  private:
    std::mutex *lock_{nullptr};
};

} // namespace

auto proto_stage_name(proto_stage stage) -> std::string_view {
    switch (stage) {
    case proto_stage::terrain:
        return "terrain";
    case proto_stage::decorated:
        return "decorated";
    }
    return "terrain";
}

proto_chunk_cache::proto_chunk_cache(std::size_t capacity,
                                     std::mutex *lock) noexcept
    : capacity_{std::max<std::size_t>(capacity, 1U)}, lock_{lock} {}

auto proto_chunk_cache::stats() const noexcept -> proto_cache_stats {
    // Every field below is a counter read under the cache mutex. The stage,
    // sealed and written flags are written under the same mutex by
    // `mark_decorated`, `seal` and `mark_written`, never through a `find`
    // pointer, so a poll from another thread can see an earlier state but never
    // a torn or unsynchronised one. The remaining entry fields are only touched
    // by the ordered lane that owns the job, so a TSan run of the scheduler
    // tests needs no suppression for this path.
    const cache_lock guard{lock_};
    return proto_cache_stats{
        .entries = entries_.size(),
        .terrain_entries = terrain_entries_,
        .pristine_entries = pristine_entries_,
        .capacity = capacity_,
        .peak_entries = peak_entries_,
        .peak_terrain_entries = peak_terrain_entries_,
        .peak_pristine_entries = peak_pristine_entries_,
        .hits = hits_,
        .misses = misses_,
        .evictions = evictions_,
        .unloads = unloads_,
    };
}

void proto_chunk_cache::clear() {
    const cache_lock guard{lock_};
    entries_.clear();
    insertion_order_.clear();
    candidates_.clear();
    pins_.clear();
    terrain_entries_ = 0;
    pristine_entries_ = 0;
}

void proto_chunk_cache::refresh_candidate(const chunk_pos &pos) {
    const auto found = entries_.find(pos);
    if (found == entries_.end() ||
        found->second.stage != proto_stage::terrain || found->second.sealed ||
        found->second.written || pins_.contains(pos)) {
        candidates_.erase(pos);
        return;
    }
    candidates_.insert(pos);
}

void proto_chunk_cache::erase_entry(const chunk_pos &pos) {
    const auto found = entries_.find(pos);
    if (found == entries_.end()) {
        return;
    }
    if (found->second.stage == proto_stage::terrain && !found->second.sealed) {
        --terrain_entries_;
        if (!found->second.written) {
            --pristine_entries_;
        }
    }
    entries_.erase(found);
    candidates_.erase(pos);
    std::erase(insertion_order_, pos);
}

void proto_chunk_cache::evict_pristine_over_capacity() {
    while (pristine_entries_ > capacity_ && !candidates_.empty()) {
        const chunk_pos victim{*candidates_.begin()};
        erase_entry(victim);
        ++evictions_;
    }
}

void proto_chunk_cache::evict_over_capacity() {
    while (entries_.size() >= capacity_ && !insertion_order_.empty()) {
        erase_entry(insertion_order_.front());
        ++evictions_;
    }
}

auto proto_chunk_cache::unload_if(
    const std::function<bool(const chunk_pos &)> &drop) -> std::size_t {
    const cache_lock guard{lock_};
    std::vector<chunk_pos> victims;
    victims.reserve(entries_.size());
    for (const auto &[position, entry] : entries_) {
        (void)entry;
        if (!pins_.contains(position) && drop(position)) {
            victims.push_back(position);
        }
    }
    std::ranges::sort(victims);
    for (const auto &position : victims) {
        erase_entry(position);
    }
    unloads_ += victims.size();
    return victims.size();
}

void proto_chunk_cache::pin(std::span<const chunk_pos> positions) {
    const cache_lock guard{lock_};
    for (const auto &position : positions) {
        ++pins_[position];
        candidates_.erase(position);
    }
}

void proto_chunk_cache::unpin(std::span<const chunk_pos> positions) {
    const cache_lock guard{lock_};
    for (const auto &position : positions) {
        const auto found = pins_.find(position);
        if (found == pins_.end()) {
            continue;
        }
        if (--found->second == 0U) {
            pins_.erase(found);
            refresh_candidate(position);
        }
    }
}

void proto_chunk_cache::mark_written(std::span<const chunk_pos> positions) {
    const cache_lock guard{lock_};
    for (const auto &position : positions) {
        const auto found = entries_.find(position);
        if (found == entries_.end() || found->second.written) {
            continue;
        }
        if (found->second.stage == proto_stage::terrain &&
            !found->second.sealed) {
            --pristine_entries_;
        }
        found->second.written = true;
        candidates_.erase(position);
    }
}

auto proto_chunk_cache::mark_decorated(const chunk_pos &pos) -> bool {
    const cache_lock guard{lock_};
    const auto found = entries_.find(pos);
    if (found == entries_.end()) {
        return false;
    }
    if (found->second.stage == proto_stage::terrain) {
        found->second.stage = proto_stage::decorated;
        if (!found->second.sealed) {
            --terrain_entries_;
            if (!found->second.written) {
                --pristine_entries_;
            }
        }
    }
    candidates_.erase(pos);
    return true;
}

auto proto_chunk_cache::seal(const chunk_pos &pos) -> bool {
    const cache_lock guard{lock_};
    const auto found = entries_.find(pos);
    if (found == entries_.end()) {
        return false;
    }
    if (!found->second.sealed) {
        found->second.sealed = true;
        if (found->second.stage == proto_stage::terrain) {
            --terrain_entries_;
            if (!found->second.written) {
                --pristine_entries_;
            }
        }
    }
    candidates_.erase(pos);
    return true;
}

auto proto_chunk_cache::find(const chunk_pos &pos) -> proto_cache_entry * {
    const cache_lock guard{lock_};
    const auto found = entries_.find(pos);
    if (found == entries_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    return &found->second;
}

auto proto_chunk_cache::find(const chunk_pos &pos) const
    -> const proto_cache_entry * {
    const cache_lock guard{lock_};
    const auto found = entries_.find(pos);
    return found == entries_.end() ? nullptr : &found->second;
}

auto proto_chunk_cache::contains(const chunk_pos &pos) const noexcept -> bool {
    const cache_lock guard{lock_};
    return entries_.contains(pos);
}

auto proto_chunk_cache::insert(const chunk_pos &pos,
                               std::unique_ptr<proto_chunk> chunk)
    -> proto_cache_entry * {
    const cache_lock guard{lock_};
    if (lock_ == nullptr) {
        // A private cache keeps its bound by dropping the oldest chunk; the
        // chunk's writes are reproducible on the single-generator paths that
        // use it, so insertion-order eviction is safe there.
        evict_over_capacity();
        insertion_order_.push_back(pos);
    }
    const auto [found, inserted] =
        entries_.try_emplace(pos, proto_cache_entry{.chunk = std::move(chunk)});
    if (inserted) {
        ++terrain_entries_;
        ++pristine_entries_;
        if (lock_ != nullptr) {
            refresh_candidate(pos);
            evict_pristine_over_capacity();
        }
    }
    peak_entries_ = std::max(peak_entries_, entries_.size());
    peak_terrain_entries_ = std::max(peak_terrain_entries_, terrain_entries_);
    peak_pristine_entries_ =
        std::max(peak_pristine_entries_, pristine_entries_);
    const auto inserted_entry = entries_.find(pos);
    // A shared insertion can evict its own entry again when the bound is
    // smaller than the pinned dependency window; the caller must not use a
    // pointer into a chunk the cache dropped.
    return inserted_entry == entries_.end() ? nullptr : &inserted_entry->second;
}

auto proto_chunk_cache::erase(const chunk_pos &pos) -> bool {
    const cache_lock guard{lock_};
    if (pins_.contains(pos)) {
        return false;
    }
    if (!entries_.contains(pos)) {
        return false;
    }
    erase_entry(pos);
    return true;
}

} // namespace refrain::worldgen
