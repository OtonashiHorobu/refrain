// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_service.hpp>

#include <refrain/core/log.hpp>

#include <cstdint>
#include <utility>

namespace {

constexpr refrain::log::logger log_chunk_gen{"chunk-gen"};

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

void chunk_service::attach(play_gateway &gateway, chunk_packet_source source) {
    streamer_.emplace(gateway, source);
    streamer_->set_chunk_requester([this](std::int32_t x, std::int32_t z) {
        return generation_ != nullptr &&
               generation_->request(refrain::world::chunk_pos{x, z});
    });
}

void chunk_service::tick(refrain::sim::world &world,
                         refrain::session::chunk_tracker &views) {
    if (generation_) {
        for (auto &[pos, column] : generation_->take_results()) {
            auto inserted{world.insert_chunk(pos, std::move(column))};
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
}

} // namespace refrain::server
