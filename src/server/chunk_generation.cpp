// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_generation.hpp>

#include <refrain/core/log.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include <fmt/format.h>

#include <string>
#include <utility>

namespace refrain::server {

namespace {

constexpr refrain::log::logger log_generation{"chunk-gen"};

[[nodiscard]] auto key_of(refrain::world::chunk_pos pos) -> std::uint64_t {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.x))
            << 32U) |
           static_cast<std::uint32_t>(pos.z);
}

} // namespace

chunk_generation::chunk_generation(config settings)
    : settings_(std::move(settings)) {}

auto chunk_generation::start(config settings)
    -> result<std::unique_ptr<chunk_generation>> {
    if (settings.data_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "chunk generation needs the datapack data root")};
    }
    auto service{std::unique_ptr<chunk_generation>{
        new chunk_generation(std::move(settings))}};
    service->worker_ =
        std::thread{[pointer = service.get()] { pointer->run(); }};
    return service;
}

chunk_generation::~chunk_generation() { stop(); }

auto chunk_generation::request(refrain::world::chunk_pos pos) -> bool {
    const std::lock_guard lock{mutex_};
    if (stopping_ || pending_.size() >= settings_.max_pending) {
        return false;
    }
    if (!queued_.insert(key_of(pos)).second) {
        return false;
    }
    pending_.push_back(pos);
    wake_.notify_one();
    return true;
}

void chunk_generation::forget(refrain::world::chunk_pos pos) {
    const std::lock_guard lock{mutex_};
    queued_.erase(key_of(pos));
}

auto chunk_generation::take_results() -> std::vector<
    std::pair<refrain::world::chunk_pos, refrain::world::chunk>> {
    const std::lock_guard lock{mutex_};
    return std::exchange(finished_, {});
}

void chunk_generation::stop() {
    {
        const std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void chunk_generation::run() {
    auto generator{
        worldgen::chunk_generator::create(worldgen::chunk_generator_config{
            .data_root = settings_.data_root,
            .templates_root = settings_.templates_root,
            .seed = settings_.seed,
            .kind = worldgen::dimension_kind::overworld,
            .target = worldgen::generation_target::full,
        })};
    if (!generator) {
        log_generation.error("generator unavailable: {}",
                             generator.error().message);
        return;
    }
    for (;;) {
        refrain::world::chunk_pos pos{};
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) {
                return;
            }
            pos = pending_.front();
            pending_.pop_front();
        }
        auto proto{(*generator)->generate(worldgen::chunk_pos{pos.x, pos.z})};
        if (!proto) {
            log_generation.debug("chunk ({}, {}) failed: {}", pos.x, pos.z,
                                 proto.error().message);
            forget(pos);
            continue;
        }
        auto column{refrain::world::from_proto_chunk(
            **proto, (*generator)->biome_names())};
        if (!column) {
            log_generation.debug("chunk ({}, {}) conversion failed: {}", pos.x,
                                 pos.z, column.error().message);
            forget(pos);
            continue;
        }
        {
            const std::lock_guard lock{mutex_};
            if (stopping_) {
                return;
            }
            finished_.emplace_back(pos, std::move(*column));
        }
    }
}

} // namespace refrain::server
