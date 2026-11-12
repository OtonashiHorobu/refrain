// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/console_service.hpp>

#include <refrain/core/log.hpp>

#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace {

constexpr std::size_t max_console_commands = 1024;

} // namespace

namespace refrain::server {

struct console_service::command_queue {
    std::mutex mutex{};
    std::vector<std::string> commands{};
};

console_service::console_service(command_handler handler)
    : handler_(std::move(handler)), queue_{std::make_shared<command_queue>()} {}

void console_service::start_stdin(std::shared_ptr<std::atomic<bool>> stop) {
    auto shared{queue_};
    std::thread console{[shared, stop = std::move(stop)] {
        refrain::log::set_thread_name("console");
        std::string line{};
        try {
            while (!stop->load(std::memory_order_acquire) &&
                   std::getline(std::cin, line)) {
                if (line.empty()) {
                    continue;
                }
                const std::lock_guard lock{shared->mutex};
                if (shared->commands.size() < max_console_commands) {
                    shared->commands.push_back(std::move(line));
                }
            }
        } catch (...) {
            // The console is diagnostics: a failed read must not terminate the
            // server; the tick loop can still be stopped by `--ticks`/signal.
        }
    }};
    // A blocking stdin read cannot be woken portably. The console thread only
    // touches the shared queue, so it is safe to detach at shutdown and let
    // process exit reclaim it.
    console.detach();
}

void console_service::push_command(std::string line) {
    if (line.empty()) {
        return;
    }
    const std::lock_guard lock{queue_->mutex};
    if (queue_->commands.size() < max_console_commands) {
        queue_->commands.push_back(std::move(line));
    }
}

void console_service::push_text(std::string_view text) {
    std::size_t start{0};
    while (start <= text.size()) {
        const auto newline{text.find('\n', start)};
        const auto end{newline == std::string_view::npos ? text.size()
                                                         : newline};
        auto line{std::string{text.substr(start, end - start)}};
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        push_command(std::move(line));
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
}

void console_service::load_replay(
    std::vector<refrain::sim::replay_step> steps) {
    replay_ = std::move(steps);
    replay_index_ = 0;
}

void console_service::tick(const refrain::core::tick_info &tick,
                           std::int64_t ticks_run,
                           const std::atomic<bool> &stop) {
    std::vector<std::string> drained{};
    {
        const std::lock_guard lock{queue_->mutex};
        drained.swap(queue_->commands);
    }
    for (const auto &line : drained) {
        handler_(line, tick);
    }
    while (replay_index_ < replay_.size() &&
           replay_[replay_index_].tick == ticks_run) {
        handler_(replay_[replay_index_].command, tick);
        if (stop.load(std::memory_order_acquire)) {
            break;
        }
        ++replay_index_;
    }
}

} // namespace refrain::server
