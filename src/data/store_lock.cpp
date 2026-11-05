// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/store_lock.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/path.hpp>
#include <refrain/platform/process.hpp>

#include <fmt/format.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace refrain::data {
namespace {

namespace io = core::io;
namespace platform = refrain::platform;

/// A lock whose owner record is older than this is considered abandoned even
/// when its pid cannot be probed (portable fallback).
constexpr auto stale_lock_age{std::chrono::hours{1}};
/// Retry interval while another process holds the store lock.
constexpr auto lock_retry_interval{std::chrono::milliseconds{50}};

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

/// Prefix the diagnostic while keeping its code.
[[nodiscard]] auto annotate(error original, std::string_view context) -> error {
    if (original.message.find(context) != std::string::npos) {
        return original;
    }
    original.message = std::string{context} + ": " + original.message;
    return original;
}

/// Read-only store detection: these are the errors a read-only mount / missing
/// write permission produces.
[[nodiscard]] auto is_read_only_error(const std::error_code &error) -> bool {
    return error == std::errc::permission_denied ||
           error == std::errc::read_only_file_system ||
           error == std::errc::operation_not_permitted;
}

[[nodiscard]] auto lock_path(std::string_view store_root) -> std::string {
    return join_path(join_path(store_root, "content"),
                     content_store_lock_directory_name);
}

/// Parse `pid=<n>` from the lock owner record.
[[nodiscard]] auto parse_owner_pid(std::string_view text)
    -> std::optional<std::uint64_t> {
    constexpr std::string_view prefix{"pid="};
    if (!text.starts_with(prefix)) {
        return std::nullopt;
    }
    auto value{text.substr(prefix.size())};
    const auto end{value.find_first_of("\r\n")};
    if (end != std::string_view::npos) {
        value = value.substr(0, end);
    }
    std::uint64_t pid{0};
    const auto [pointer, error]{
        std::from_chars(value.data(), value.data() + value.size(), pid)};
    if (error != std::errc{} || pointer != value.data() + value.size() ||
        pid == 0U) {
        return std::nullopt;
    }
    return pid;
}

struct stale_lock {
    bool stale{false};
    std::string reason{};
};

/// A lock is stale when its owner pid is gone, or — when no usable pid can be
/// read (missing/truncated owner, platform without a liveness probe) — when
/// the owner record (falling back to the lock directory itself) is older than
/// `stale_lock_age`. A live pid always wins over the mtime window: a healthy
/// instance may legitimately hold the store for more than an hour.
[[nodiscard]] auto inspect_lock(std::string_view path, std::string_view owner)
    -> stale_lock {
    auto text{io::read_text(owner)};
    if (text) {
        const auto pid{parse_owner_pid(*text)};
        if (pid) {
            if (!platform::process_alive(*pid)) {
                return stale_lock{
                    .stale = true,
                    .reason = fmt::format("owner pid {} is not running", *pid)};
            }
            return stale_lock{};
        }
    }
    std::error_code time_error;
    auto modified{std::filesystem::last_write_time(
        std::filesystem::path{std::string{owner}}, time_error)};
    if (time_error) {
        time_error.clear();
        modified = std::filesystem::last_write_time(
            std::filesystem::path{std::string{path}}, time_error);
    }
    const auto age_limit{io::exists(owner) ? stale_lock_age
                                           : std::chrono::minutes{1}};
    if (!time_error &&
        modified < std::filesystem::file_time_type::clock::now() - age_limit) {
        return stale_lock{
            .stale = true,
            .reason = io::exists(owner)
                          ? fmt::format("owner is older than {} hour(s)",
                                        stale_lock_age.count())
                          : std::string{"owner record is older than 1 "
                                        "minute(s)"}};
    }
    return stale_lock{};
}

/// Serializes stale-lock reclaimers: `<lock>.reclaim` is itself a directory
/// created atomically, so only one process can be reclaiming at a time. Without
/// it, two processes that both judged the same lock stale could race between
/// "remove" and "create" and end up both holding the store (the reclaim
/// counterpart of an ABA). A crashed reclaimer leaves the marker behind; it is
/// considered abandoned after `stale_lock_age` like the lock itself.
class reclaim_guard {
  public:
    reclaim_guard() = default;
    reclaim_guard(const reclaim_guard &) = delete;
    auto operator=(const reclaim_guard &) -> reclaim_guard & = delete;
    reclaim_guard(reclaim_guard &&other) noexcept
        : path_{std::move(other.path_)}, held_{other.held_} {
        other.held_ = false;
    }
    auto operator=(reclaim_guard &&other) noexcept -> reclaim_guard & {
        if (this != &other) {
            release();
            path_ = std::move(other.path_);
            held_ = other.held_;
            other.held_ = false;
        }
        return *this;
    }
    ~reclaim_guard() { release(); }

    [[nodiscard]] static auto
    acquire(std::string_view path,
            std::chrono::steady_clock::time_point deadline) -> reclaim_guard {
        for (;;) {
            std::error_code error;
            if (std::filesystem::create_directory(
                    std::filesystem::path{std::string{path}}, error)) {
                reclaim_guard guard;
                guard.path_ = std::string{path};
                guard.held_ = true;
                return guard;
            }
            if (error) {
                return {};
            }
            // The marker already exists. Reclaim it when abandoned (the
            // process that created it died mid-reclaim).
            std::error_code time_error;
            const auto modified{std::filesystem::last_write_time(
                std::filesystem::path{std::string{path}}, time_error)};
            if (!time_error &&
                modified < std::filesystem::file_time_type::clock::now() -
                               stale_lock_age) {
                std::error_code remove_error;
                std::filesystem::remove_all(
                    std::filesystem::path{std::string{path}}, remove_error);
                if (!remove_error) {
                    continue;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return {};
            }
            std::this_thread::sleep_for(lock_retry_interval);
        }
    }

    [[nodiscard]] auto held() const noexcept -> bool { return held_; }

  private:
    /// Must not throw (called from the destructor and a `noexcept` move
    /// assignment) but building the path allocates: contain a hard OOM so
    /// teardown never terminates the process.
    void release() noexcept {
        if (!held_) {
            return;
        }
        held_ = false;
        try {
            std::error_code error;
            std::filesystem::remove_all(std::filesystem::path{path_}, error);
        } catch (...) {
        }
    }

    std::string path_{};
    bool held_{false};
};

} // namespace

store_lock::store_lock(store_lock &&other) noexcept
    : path_{std::move(other.path_)}, owner_path_{std::move(other.owner_path_)},
      owner_record_{std::move(other.owner_record_)},
      owner_written_{other.owner_written_}, held_{other.held_} {
    other.held_ = false;
}

auto store_lock::operator=(store_lock &&other) noexcept -> store_lock & {
    if (this != &other) {
        release();
        path_ = std::move(other.path_);
        owner_path_ = std::move(other.owner_path_);
        owner_record_ = std::move(other.owner_record_);
        owner_written_ = other.owner_written_;
        held_ = other.held_;
        other.held_ = false;
    }
    return *this;
}

store_lock::~store_lock() { release(); }

auto store_lock::acquire(std::string_view store_root, int timeout_ms,
                         const std::function<void(std::string_view)> &log)
    -> result<store_lock> {
    const auto emit = [&log](std::string line) {
        if (log) {
            log(line);
        }
    };
    const auto path{lock_path(store_root)};
    const auto owner{join_path(path, content_store_lock_owner_name)};
    const auto content_directory{join_path(store_root, "content")};

    std::error_code create_error;
    std::filesystem::create_directories(
        std::filesystem::path{content_directory}, create_error);
    if (create_error) {
        if (is_read_only_error(create_error)) {
            return std::unexpected{
                fail(error_code::permission_denied,
                     fmt::format("content store '{}' is read-only: {}",
                                 store_root, create_error.message()))};
        }
        return std::unexpected{
            fail(error_code::io_error,
                 fmt::format("cannot create '{}': {}", content_directory,
                             create_error.message()))};
    }

    const auto deadline{std::chrono::steady_clock::now() +
                        std::chrono::milliseconds{timeout_ms}};
    bool reclaimed{false};
    for (;;) {
        std::error_code error;
        const bool created{std::filesystem::create_directory(
            std::filesystem::path{path}, error)};
        if (created) {
            store_lock lock;
            lock.path_ = path;
            lock.owner_path_ = owner;
            lock.held_ = true;
            const auto started{
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()};
            lock.owner_record_ = fmt::format("pid={}\nstarted={}\n",
                                             platform::process_id(), started);
            auto written{io::write_text(owner, lock.owner_record_)};
            if (!written) {
                lock.release();
                return std::unexpected{
                    annotate(std::move(written.error()),
                             fmt::format("lock owner '{}'", owner))};
            }
            lock.owner_written_ = true;
            return lock;
        }
        if (error) {
            if (is_read_only_error(error)) {
                return std::unexpected{
                    fail(error_code::permission_denied,
                         fmt::format("content store '{}' is read-only: cannot "
                                     "create '{}': {}",
                                     store_root, path, error.message()))};
            }
            return std::unexpected{fail(
                error_code::io_error, fmt::format("cannot create lock '{}': {}",
                                                  path, error.message()))};
        }

        // The directory already exists: reclaim it when abandoned. Reclaim is
        // serialized through `<lock>.reclaim`, so two processes can never both
        // remove/create the same lock (that race ended with two holders). Only
        // the marker owner deletes the stale directory; everyone else waits
        // and then re-inspects the new lock.
        const auto state{inspect_lock(path, owner)};
        if (state.stale) {
            if (!reclaimed) {
                emit(fmt::format("lock: warning: reclaiming stale lock '{}' "
                                 "({})",
                                 path, state.reason));
                reclaimed = true;
            }
            const auto marker{fmt::format("{}.reclaim", path)};
            auto guard{reclaim_guard::acquire(marker, deadline)};
            if (guard.held()) {
                // Still the stale lock we inspected: no other reclaimer can
                // have replaced it while we hold the marker.
                const auto again{inspect_lock(path, owner)};
                if (again.stale) {
                    std::error_code remove_error;
                    std::filesystem::remove(
                        std::filesystem::path{std::string{owner}},
                        remove_error);
                    remove_error.clear();
                    std::filesystem::remove(std::filesystem::path{path},
                                            remove_error);
                    if (remove_error) {
                        emit(fmt::format("lock: warning: cannot reclaim '{}': "
                                         "{}",
                                         path, remove_error.message()));
                    }
                }
                continue; // guard releases the marker at the end of scope
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            auto owner_text{io::read_text(owner)};
            const auto pid{owner_text ? parse_owner_pid(*owner_text)
                                      : std::nullopt};
            return std::unexpected{fail(
                error_code::io_error,
                fmt::format("content store '{}' is locked by '{}'{}; another "
                            "refrain instance may be using it. Wait for it to "
                            "exit or mount the store read-only",
                            store_root, path,
                            pid ? fmt::format(" (owner pid {})", *pid)
                                : std::string{}))};
        }
        std::this_thread::sleep_for(lock_retry_interval);
    }
}

void store_lock::release() noexcept {
    // Teardown must not throw (destructor + `noexcept` move assignment), but
    // reading the owner record and building the paths allocate: contain a
    // hard OOM instead of terminating.
    try {
        if (!held_) {
            return;
        }
        held_ = false;
        // A failed acquire may leave the lock directory without an owner file;
        // we still own it, so remove it instead of wedging the store until the
        // stale-lock grace expires.
        if (!owner_written_) {
            std::error_code error;
            static_cast<void>(std::filesystem::remove(
                std::filesystem::path{owner_path_}, error));
            error.clear();
            static_cast<void>(
                std::filesystem::remove(std::filesystem::path{path_}, error));
            return;
        }
        // Only remove the lock when the owner record is still ours: a lock that
        // was reclaimed (owner process presumed dead) and re-acquired by
        // another instance must not be deleted by this destructor.
        auto current{io::read_text(owner_path_)};
        if (!current || *current != owner_record_) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(std::filesystem::path{owner_path_}, error);
        error.clear();
        std::filesystem::remove(std::filesystem::path{path_}, error);
    } catch (...) {
    }
}

} // namespace refrain::data
