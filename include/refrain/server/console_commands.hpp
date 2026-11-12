// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Console command bodies. The service that owns the queue calls execute() on
// the simulation thread; the dependencies are the runtime objects a command
// reads or mutates.

#pragma once

#include <refrain/core/config.hpp>
#include <refrain/core/time.hpp>
#include <refrain/server/world_port.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace refrain::net {
class server;
} // namespace refrain::net

namespace refrain::server {

class maintenance_service;

/// Tick work samples the console `tps` command reports. The tick loop records
/// one sample per tick; the console reads the ring and the running totals.
struct tick_metrics {
    static constexpr std::size_t sample_capacity{2048};

    void record(std::uint64_t work_ns);

    std::array<std::uint32_t, sample_capacity> samples{};
    std::size_t count{0};
    std::uint64_t total_us{0};
    std::uint64_t max_us{0};
};

/// Runtime objects a console command may read or mutate. Pointers may be null
/// when the corresponding subsystem is unavailable; each command checks the
/// one it needs.
struct console_dependencies {
    world_port *world{nullptr};
    maintenance_service *maintenance{nullptr};
    refrain::net::server *listener{nullptr};
    refrain::core::config::store *config{nullptr};
    std::atomic<bool> *stop{nullptr};
};

/// Parses and executes one console line.
class console_commands {
  public:
    console_commands(console_dependencies dependencies, tick_metrics &metrics);

    void execute(std::string_view line, const refrain::core::tick_info &tick);

  private:
    console_dependencies dependencies_{};
    tick_metrics &metrics_;
};

} // namespace refrain::server
