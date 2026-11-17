// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/proto_cache.hpp>

#include <algorithm>

namespace refrain::worldgen {

auto proto_stage_name(proto_stage stage) -> std::string_view {
    switch (stage) {
    case proto_stage::terrain:
        return "terrain";
    case proto_stage::decorated:
        return "decorated";
    }
    return "terrain";
}

proto_chunk_cache::proto_chunk_cache(std::size_t capacity) noexcept
    : capacity_{std::max<std::size_t>(capacity, 1U)} {}

auto proto_chunk_cache::stats() const noexcept -> proto_cache_stats {
    return proto_cache_stats{
        .entries = entries_.size(),
        .capacity = capacity_,
        .hits = hits_,
        .misses = misses_,
        .evictions = evictions_,
    };
}

void proto_chunk_cache::clear() {
    entries_.clear();
    insertion_order_.clear();
}

auto proto_chunk_cache::find(const chunk_pos &pos) -> proto_cache_entry * {
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
    const auto found = entries_.find(pos);
    return found == entries_.end() ? nullptr : &found->second;
}

auto proto_chunk_cache::contains(const chunk_pos &pos) const noexcept -> bool {
    return entries_.contains(pos);
}

auto proto_chunk_cache::insert(const chunk_pos &pos,
                               std::unique_ptr<proto_chunk> chunk)
    -> proto_cache_entry & {
    evict_over_capacity();
    insertion_order_.push_back(pos);
    auto [found, inserted] =
        entries_.try_emplace(pos, proto_cache_entry{.chunk = std::move(chunk)});
    return found->second;
}

auto proto_chunk_cache::erase(const chunk_pos &pos) -> bool {
    const auto erased = entries_.erase(pos);
    if (erased == 0U) {
        return false;
    }
    std::erase(insertion_order_, pos);
    return true;
}

void proto_chunk_cache::evict_over_capacity() {
    while (entries_.size() >= capacity_ && !insertion_order_.empty()) {
        entries_.erase(insertion_order_.front());
        insertion_order_.pop_front();
        ++evictions_;
    }
}

} // namespace refrain::worldgen
