// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace refrain::data {

/// `<store>/content/.lock` (a directory: `create_directory` is atomic on every
/// supported filesystem, unlike a create-then-write file protocol). The lock
/// serializes every writer of one content store: startup install/index/GC,
/// `content build`, `content gc --apply` and `mods install`.
inline constexpr std::string_view content_store_lock_directory_name{".lock"};
/// Owner record inside the lock directory (`pid=<n>\nstarted=<unix-seconds>`).
inline constexpr std::string_view content_store_lock_owner_name{"owner"};

/// RAII guard for `<store>/content/.lock`.
///
/// The lock is a directory so acquisition is a single atomic
/// `create_directory`. A lock whose owner process is gone — or, when no usable
/// pid can be read, whose owner record is older than one hour — is reclaimed
/// through a
/// `<lock>.reclaim` marker directory: only the marker owner removes the stale
/// lock, re-inspects it first, and concurrent reclaimers wait, so two processes
/// can never both end up holding the store. An owner-less lock (a crash between
/// `create_directory` and writing the owner record) is reclaimable after one
/// minute.
class store_lock {
  public:
    store_lock() = default;
    store_lock(const store_lock &) = delete;
    auto operator=(const store_lock &) -> store_lock & = delete;
    store_lock(store_lock &&other) noexcept;
    auto operator=(store_lock &&other) noexcept -> store_lock &;
    ~store_lock();

    [[nodiscard]] auto held() const noexcept -> bool { return held_; }
    /// The lock directory path (valid while `held()`).
    [[nodiscard]] auto path() const noexcept -> std::string_view {
        return path_;
    }

    /// Acquire the lock, reclaiming a stale one. A store that cannot be
    /// written at all is `permission_denied` (the caller may downgrade to
    /// scan-only); waiting longer than `timeout_ms` (0 = one attempt) is
    /// `io_error`. `log` receives stable `lock: ...` diagnostic lines; the
    /// caller may prefix them with its command name.
    [[nodiscard]] static auto
    acquire(std::string_view store_root, int timeout_ms,
            const std::function<void(std::string_view)> &log)
        -> result<store_lock>;

  private:
    void release() noexcept;

    std::string path_{};
    std::string owner_path_{};
    /// Exact bytes this instance wrote to the owner file. `release()` removes
    /// the lock only when the owner file still matches, so a lock that was
    /// reclaimed and re-acquired by another process is left untouched.
    std::string owner_record_{};
    /// False until the owner record was written; a partial acquire must be
    /// able to remove the lock directory it created.
    bool owner_written_{false};
    bool held_{false};
};

} // namespace refrain::data
