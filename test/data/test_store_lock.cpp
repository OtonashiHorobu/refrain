// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/store_lock.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace fs = std::filesystem;
namespace io = refrain::core::io;

using refrain::data::store_lock;

/// Unique scratch directory under the OS temp directory, removed on exit.
class temp_directory {
  public:
    temp_directory() {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        const auto base{fs::temp_directory_path(error)};
        if (error) {
            return;
        }
        for (int attempt{0}; attempt < 16; ++attempt) {
            const auto candidate{base / ("refrain-store-lock-" +
                                         std::to_string(sequence.fetch_add(
                                             1, std::memory_order_relaxed)))};
            if (fs::create_directories(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                return;
            }
            error.clear();
        }
    }
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    ~temp_directory() {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    [[nodiscard]] auto path() const -> std::string { return path_.string(); }
    [[nodiscard]] auto valid() const noexcept -> bool { return !path_.empty(); }

  private:
    fs::path path_{};
};

/// Pre-create a lock directory whose owner pid is certainly not running.
void make_stale_lock(const std::string &store_root) {
    const auto lock{fs::path{store_root} / "content" / ".lock"};
    std::error_code error;
    fs::create_directories(lock, error);
    expect(!error);
    const auto owner{lock / "owner"};
    expect(static_cast<bool>(
        io::write_text(owner.string(), "pid=999999999\nstarted=0\n")));
}

const suite store_lock_tests = [] {
    "store lock is exclusive and released by the destructor"_test = [] {
        temp_directory scratch;
        expect(scratch.valid());
        if (!scratch.valid()) {
            return;
        }
        {
            auto first{store_lock::acquire(scratch.path(), 0, {})};
            expect(first.has_value());
            if (!first) {
                return;
            }
            expect(first->held());
            auto second{store_lock::acquire(scratch.path(), 0, {})};
            expect(!second);
            if (!second) {
                expect(second.error().code == refrain::error_code::io_error);
            }
        }
        // Destruction removed the lock: a new acquisition succeeds.
        auto again{store_lock::acquire(scratch.path(), 0, {})};
        expect(again.has_value());
        if (again) {
            expect(again->held());
        }
    };

    "store lock reclaims a stale owner and logs the reclaim"_test = [] {
        temp_directory scratch;
        expect(scratch.valid());
        if (!scratch.valid()) {
            return;
        }
        make_stale_lock(scratch.path());
        std::vector<std::string> lines;
        auto acquired{store_lock::acquire(
            scratch.path(), 0,
            [&lines](std::string_view line) { lines.emplace_back(line); })};
        expect(acquired.has_value());
        if (!acquired) {
            return;
        }
        bool saw_stale{false};
        for (const auto &line : lines) {
            if (line.find("stale lock") != std::string::npos) {
                saw_stale = true;
            }
        }
        expect(saw_stale);
        // The reclaimed directory is gone; only the new lock remains.
        expect(fs::exists(fs::path{scratch.path()} / "content" / ".lock"));
    };

    "store lock never grants two holders during a stale-lock race"_test = [] {
        temp_directory scratch;
        expect(scratch.valid());
        if (!scratch.valid()) {
            return;
        }
        make_stale_lock(scratch.path());
        std::atomic<int> active{0};
        std::atomic<int> maximum{0};
        std::atomic<int> acquired_count{0};
        std::vector<std::thread> workers;
        constexpr int worker_count{6};
        workers.reserve(worker_count);
        for (int worker{0}; worker < worker_count; ++worker) {
            workers.emplace_back([&] {
                for (int attempt{0}; attempt < 20; ++attempt) {
                    auto lock{store_lock::acquire(scratch.path(), 200, {})};
                    if (!lock) {
                        continue;
                    }
                    const auto now{active.fetch_add(1) + 1};
                    int observed{maximum.load()};
                    while (now > observed &&
                           !maximum.compare_exchange_weak(observed, now)) {
                    }
                    ++acquired_count;
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    active.fetch_sub(1);
                }
            });
        }
        for (auto &worker : workers) {
            worker.join();
        }
        expect(acquired_count.load() > 0);
        expect(maximum.load() == 1);
        expect(active.load() == 0);
    };
};

} // namespace
