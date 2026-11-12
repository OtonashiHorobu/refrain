// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace refrain::server {

/// Generates world columns off the simulation thread.
///
/// One worker owns the generator (the generator is not thread-safe). The
/// simulation thread requests positions and drains finished columns; the
/// worker never touches the world, so the only shared state is the request
/// queue and the result queue under one mutex.
class chunk_generation {
  public:
    struct config {
        std::string data_root{};
        std::string templates_root{};
        std::int64_t seed{0};
        /// Upper bound on queued positions; further requests are refused so a
        /// burst of viewport changes cannot grow the queue without bound.
        std::size_t max_pending{512};
    };

    [[nodiscard]] static auto start(config settings)
        -> result<std::unique_ptr<chunk_generation>>;
    ~chunk_generation();

    chunk_generation(const chunk_generation &) = delete;
    auto operator=(const chunk_generation &) -> chunk_generation & = delete;

    /// Queue one column. Returns false when it is already queued or the queue
    /// is full.
    auto request(refrain::world::chunk_pos pos) -> bool;

    /// Remove one column from the queued set (the caller no longer needs it).
    void forget(refrain::world::chunk_pos pos);

    /// Take every finished column. The simulation thread applies them.
    [[nodiscard]] auto take_results() -> std::vector<
        std::pair<refrain::world::chunk_pos, refrain::world::chunk>>;

    /// Stop the worker and join it.
    void stop();

  private:
    explicit chunk_generation(config settings);

    void run();

    config settings_{};
    std::mutex mutex_{};
    std::condition_variable wake_{};
    std::deque<refrain::world::chunk_pos> pending_{};
    std::unordered_set<std::uint64_t> queued_{};
    std::vector<std::pair<refrain::world::chunk_pos, refrain::world::chunk>>
        finished_{};
    bool stopping_{false};
    std::thread worker_{};
};

} // namespace refrain::server
