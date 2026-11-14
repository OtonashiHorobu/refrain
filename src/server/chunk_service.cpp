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
                log_chunk_gen.debug("failed to insert chunk ({}, {}): {}",
                                    pos.x, pos.z, inserted.error().message);
                continue;
            }
            if (streamer_) {
                streamer_->chunk_ready(pos.x, pos.z);
            }
        }
        apply_generation_failures();
    }
    if (streamer_) {
        streamer_->tick(views);
    }
    enforce_residency(world);
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

void chunk_service::apply_generation_failures() {
    if (!generation_) {
        return;
    }
    const auto edge{generation_->tile_size()};
    for (const auto &failure : generation_->take_failures()) {
        // The first distinct message is operator-visible; a generator that
        // keeps failing the same way is only debug detail after that.
        if (failure.message == logged_failure_message_) {
            log_chunk_gen.debug("failed to generate chunk ({}, {}): {}",
                                failure.tile.x, failure.tile.z,
                                failure.message);
        } else {
            logged_failure_message_ = failure.message;
            log_chunk_gen.warn("failed to generate chunk ({}, {}): {}",
                               failure.tile.x, failure.tile.z, failure.message);
        }
        if (!streamer_) {
            continue;
        }
        // The tile is the unit that failed, so every column of it is free to
        // be requested again; the streamer keeps the ones a connection is
        // waiting for out of the request set until the attempt bound is hit.
        for (std::int32_t dz{0}; dz < edge; ++dz) {
            for (std::int32_t dx{0}; dx < edge; ++dx) {
                streamer_->chunk_failed(failure.tile.x + dx,
                                        failure.tile.z + dz);
            }
        }
    }
}

auto chunk_service::process_resident_bytes() -> std::uint64_t {
    return resident_bytes();
}

void chunk_service::enforce_residency(world_port &world) {
    if (!world.controls_residency()) {
        return;
    }
    // Pin sync runs only when a viewport moved. Between moves the pin set is
    // still exact, so a still camera costs nothing beyond the sweep below.
    auto views{streamer_ ? streamer_->viewports()
                         : std::vector<chunk_streamer::viewport>{}};
    if (views != viewports_) {
        viewports_ = std::move(views);
        // The resident set the viewports ask for: the union of their squares.
        std::set<sim::chunk_pos, chunk_order> wanted{};
        for (const auto &view : viewports_) {
            for (std::int32_t dz{-view.radius}; dz <= view.radius; ++dz) {
                for (std::int32_t dx{-view.radius}; dx <= view.radius; ++dx) {
                    wanted.insert(
                        sim::chunk_pos{view.chunk_x + dx, view.chunk_z + dz});
                }
            }
        }
        // Release first, then pin: a column that left one viewport but is
        // inside another keeps exactly one ticket, and every step walks a
        // sorted set.
        for (auto ticket = view_tickets_.begin();
             ticket != view_tickets_.end();) {
            if (wanted.contains(*ticket)) {
                ++ticket;
                continue;
            }
            if (auto released{world.remove_ticket(*ticket)}; !released) {
                log_chunk_gen.debug(
                    "failed to release the ticket for chunk ({}, {}): {}",
                    ticket->x, ticket->z, released.error().message);
            }
            ticket = view_tickets_.erase(ticket);
        }
        for (const auto &pos : wanted) {
            if (view_tickets_.contains(pos)) {
                continue;
            }
            if (auto held{world.hold_ticket(pos, sim::ticket_kind::player)};
                !held) {
                log_chunk_gen.debug("failed to pin chunk ({}, {}): {}", pos.x,
                                    pos.z, held.error().message);
                continue;
            }
            view_tickets_.insert(pos);
        }
    }
    // Everything loaded and unpinned is eligible. The positions come back in
    // ascending order, which is the order the batch unloads them, so a replay
    // drops the same columns at the same ticks.
    std::vector<sim::chunk_pos> clean{};
    std::vector<sim::chunk_pos> dirty{};
    for (const auto &entry : world.unticketed_chunks()) {
        if (entry.dirty) {
            dirty.push_back(entry.pos);
        } else {
            clean.push_back(entry.pos);
        }
    }
    std::size_t unloaded{0};
    const auto unload_batch{[&](std::vector<sim::chunk_pos> &victims) {
        if (victims.empty()) {
            return;
        }
        auto dropped{world.unload_chunks(victims, false)};
        if (dropped) {
            unloaded += victims.size();
            return;
        }
        // The batch is all-or-nothing: a failed flush keeps every column
        // resident and dirty for the next pass.
        log_chunk_gen.warn("failed to unload chunks: {}",
                           dropped.error().message);
    }};
    // A clean column needs no store traffic: drop it without touching the
    // writer or the disk.
    unload_batch(clean);
    if (!dirty.empty()) {
        if (dirty.size() > max_dirty_unloads_per_tick) {
            // The rest waits for the next pass; the prefix is the ascending
            // order, so the choice is deterministic.
            dirty.resize(max_dirty_unloads_per_tick);
        }
        // An older snapshot of a flushed column may still be in flight on the
        // writer thread; let it land first so this newer write is the last one
        // for the column.
        if (unload_barrier_) {
            unload_barrier_();
        }
        unload_batch(dirty);
    }
    residency_.unloaded_chunks += unloaded;
    residency_.resident_chunks = world.loaded_chunks();
    residency_.peak_resident_chunks =
        std::max(residency_.peak_resident_chunks, residency_.resident_chunks);
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
