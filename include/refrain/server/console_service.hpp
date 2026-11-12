// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Console input scheduling. The stdin thread and a replay script only queue
// lines; the simulation thread drains them at the tick boundary and hands each
// one to the command handler, so no game logic runs on the console thread.

#pragma once

#include <refrain/core/time.hpp>
#include <refrain/sim/replay.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::server {

class console_service {
  public:
    /// `line` is borrowed for the duration of the call and `tick` describes
    /// the tick the command runs on.
    using command_handler = std::function<void(
        std::string_view line, const refrain::core::tick_info &tick)>;

    explicit console_service(command_handler handler);

    /// Read stdin on a detached thread and queue one command per line. The
    /// flag is shared with the thread so it can outlive the service.
    void start_stdin(std::shared_ptr<std::atomic<bool>> stop);

    void push_command(std::string line);
    void push_text(std::string_view text);

    /// Replace the replay script. Commands run when the tick index matches.
    void load_replay(std::vector<refrain::sim::replay_step> steps);

    /// Drain the queued lines, then the replay steps due at `ticks_run`. A
    /// command that requests shutdown stops the replay at that step.
    void tick(const refrain::core::tick_info &tick, std::int64_t ticks_run,
              const std::atomic<bool> &stop);

  private:
    struct command_queue;

    command_handler handler_;
    std::shared_ptr<command_queue> queue_;
    std::vector<refrain::sim::replay_step> replay_{};
    std::size_t replay_index_{0};
};

} // namespace refrain::server
