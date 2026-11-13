// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_service.hpp>

#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_chunk_gen{"chunk-gen"};

/// Chunk coordinate of a world position, rounding towards negative infinity so
/// a position just west of zero stays in chunk -1.
[[nodiscard]] auto chunk_of(double coordinate) -> std::int32_t {
    const auto block{static_cast<std::int64_t>(std::floor(coordinate))};
    return static_cast<std::int32_t>(block >= 0 ? block / 16
                                                : (block - 15) / 16);
}

/// Resident set of this process, in bytes, from `VmRSS`. Zero means the host
/// has no procfs to report it and the controller ignores the memory input.
[[nodiscard]] auto resident_bytes() -> std::uint64_t {
    const auto status{refrain::core::io::read_text(
        "/proc/self/status",
        {.max_bytes = 64ULL * 1024ULL, .allow_symlink = true})};
    if (!status) {
        return 0;
    }
    const auto label{status->find("VmRSS:")};
    if (label == std::string::npos) {
        return 0;
    }
    const auto end{status->find('\n', label)};
    std::string_view field{*status};
    field = field.substr(label + 6,
                         end == std::string::npos ? end : end - label - 6);
    field.remove_prefix(std::min(field.find_first_not_of(" \t"), field.size()));
    std::uint64_t kib{0};
    const auto parsed{
        std::from_chars(field.data(), field.data() + field.size(), kib)};
    if (parsed.ec != std::errc{}) {
        return 0;
    }
    return kib * 1024ULL;
}

[[nodiscard]] auto total_backlog(const refrain::server::play_gateway &gateway)
    -> std::size_t {
    std::size_t total{0};
    for (const auto connection : gateway.active_connections()) {
        total += gateway.backlog(connection);
    }
    return total;
}

} // namespace

namespace refrain::server {

chunk_service::chunk_service() = default;

chunk_service::~chunk_service() = default;

auto chunk_service::start_generation(chunk_generation::config settings)
    -> status {
    auto started{chunk_generation::start(std::move(settings))};
    if (!started) {
        return std::unexpected{started.error()};
    }
    generation_ = std::move(*started);
    return status_ok();
}

void chunk_service::attach(play_gateway &gateway, chunk_packet_source source,
                           chunk_streamer::backlog_limits backlog) {
    gateway_ = &gateway;
    backlog_ = backlog.clamped();
    streamer_.emplace(gateway, source, backlog_);
    streamer_->set_chunk_requester([this](std::int32_t x, std::int32_t z) {
        return generation_ != nullptr &&
               generation_->request(refrain::world::chunk_pos{x, z});
    });
}

void chunk_service::tick(world_port &world,
                         refrain::session::chunk_tracker &views,
                         std::uint64_t tick_start_ns) {
    if (generation_) {
        for (auto &[pos, column] : generation_->take_results()) {
            auto inserted{world.chunk_insert(pos, std::move(column))};
            if (!inserted) {
                log_chunk_gen.debug("insert ({}, {}) failed: {}", pos.x, pos.z,
                                    inserted.error().message);
                continue;
            }
            if (streamer_) {
                streamer_->chunk_ready(pos.x, pos.z);
            }
        }
    }
    if (streamer_) {
        streamer_->tick(views);
    }
    if (!generation_) {
        return;
    }
    // Distance priority: the nearest player column wins the credits, so a
    // walking player's viewport fills before terrain nobody is looking at.
    std::vector<refrain::world::chunk_pos> centres{};
    if (auto *players = world.players(); players != nullptr) {
        centres.reserve(players->size());
        players->for_each([&centres](const refrain::sim::player_state &player) {
            centres.push_back(refrain::world::chunk_pos{chunk_of(player.x),
                                                        chunk_of(player.z)});
        });
    }
    generation_->set_priority_centres(centres);
    // Hard pause: a client backlog above the bound means the stream cannot
    // drain what is already queued, so generating more would only grow it.
    if (gateway_ != nullptr &&
        total_backlog(*gateway_) > backlog_.max_total_bytes) {
        return;
    }
    generation_->dispatch(refrain::core::now_ns() - tick_start_ns);
}

void chunk_service::observe_tick(std::uint64_t work_ns) {
    if (!generation_) {
        return;
    }
    tick_samples_[tick_sample_next_] = work_ns;
    tick_sample_next_ = (tick_sample_next_ + 1) % tick_samples_.size();
    ++ticks_since_update_;
    if (ticks_since_update_ < controller_window_ticks) {
        return;
    }
    ticks_since_update_ = 0;
    auto sorted{tick_samples_};
    std::sort(sorted.begin(), sorted.end());
    // Same p99 definition the console `tps` command reports.
    const auto index{static_cast<std::size_t>(
        static_cast<double>(sorted.size() - 1) * 0.99)};
    generation_->update_admission(admission_sample{
        .queue_depth = generation_->queue_depth(),
        .tick_p99_ms = static_cast<double>(sorted[index]) / 1'000'000.0,
        .resident_bytes = resident_bytes(),
    });
}

} // namespace refrain::server
