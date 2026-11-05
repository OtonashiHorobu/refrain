// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace refrain::data {

// Asynchronous block streaming and prefetch over one content-store composition.
//
// `open()` validates `<store>/content/blobs.idx` (RFBT) and
// `<store>/content/resolutions/<composition>.idx` (RFRX) fail-closed and keeps
// the winner map `location -> blob_location` (the same winner the content pack
// exposes). A missing table/index is `not_found`; a damaged or foreign-version
// index keeps `parse_error`/`version_mismatch`. Synchronous `read()` goes
// through `content_store::read(const blob_location&)`, so raw and packed blobs
// are decoded and digest-verified exactly like the pack path.
//
// `read_async()` posts the read to a private `core::async_runtime` and invokes
// the callback on one of its worker threads. It never blocks the caller. It
// returns false (and does not invoke the callback) for an unknown location, an
// empty callback, or a stopped runtime.
//
// `prefetch()` is fire-and-forget: it posts one task per blob that reads every
// packed-block window through `content_store::read_range()` into a scratch
// buffer and discards it. The default window is 256 KiB (four 64 KiB blocks);
// `prefetch_window_bytes` is rounded up to whole blocks. Windows are keyed by
// `(digest, window_index)`: while a window is in flight, a second prefetch of
// the same window is counted as `deduped` and not issued again. Raw blobs
// (file-backed or in a segment) are small (<= 64 KiB) and are prefetched as a
// single whole-blob read. Prefetch errors are intentionally ignored (the next
// real read reports them).
//
// There is deliberately no block cache (the default budget is 0 bytes) and no
// global cache: the mmap/read_file page cache is the OS's job. Cancellation,
// priorities and a persistent decompression worker pool are not implemented.
//
// Lifetime/threading: the handle is move-only. The destructor stops the
// runtime and joins its worker threads, so pending tasks are dropped and a
// running callback finishes before the destructor returns. The object must
// outlive every callback; do not destroy it from inside a callback (that would
// call `async_runtime::stop()` on a runtime thread). `read()`, `read_async()`,
// `prefetch()` and `stats()` are safe to call concurrently after `open()`;
// destroying the handle must not race with another member call.
struct content_stream_options {
    /// Worker threads of the private async runtime. Must be non-zero: unlike
    /// `core::async_runtime::open(0)`, `content_stream` exposes no `run()`, so
    /// a zero-worker runtime could never deliver an async callback.
    std::size_t worker_threads{1};
    /// Prefetch window in bytes; rounded up to whole 64 KiB packed blocks.
    /// Zero is rejected.
    std::uint64_t prefetch_window_bytes{256U * 1024U};
};

/// Snapshot returned by `content_stream::stats()`.
struct content_stream_stats {
    /// Successful synchronous + asynchronous reads.
    std::uint64_t reads{0};
    /// Prefetch windows actually issued (not deduplicated).
    std::uint64_t prefetches{0};
    /// Prefetch windows skipped because the same `(digest, window)` was
    /// already in flight.
    std::uint64_t deduped{0};
    /// Logical bytes fetched from the store by reads and prefetch windows
    /// (prefetched bytes are counted even though they are discarded).
    std::uint64_t bytes{0};
};

class content_stream {
  public:
    content_stream() noexcept;
    content_stream(const content_stream &) = delete;
    auto operator=(const content_stream &) -> content_stream & = delete;
    content_stream(content_stream &&other) noexcept;
    auto operator=(content_stream &&other) noexcept -> content_stream &;
    ~content_stream();

    /// Open one composition of the content store at `store_root`. `store_root`
    /// must be non-empty valid UTF-8; `composition` follows the content-pack
    /// rules (`is_valid_content_composition`). Options are validated before any
    /// work starts.
    [[nodiscard]] static auto open(std::string_view store_root,
                                   std::string_view composition,
                                   content_stream_options options = {})
        -> result<content_stream>;

    /// True when this handle was opened successfully (moved-from/default
    /// handles are inert).
    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Synchronous read of one resolution location. An unknown location is
    /// `not_found`; store decode/IO failures keep their code.
    [[nodiscard]] auto read(std::string_view location) const
        -> result<std::vector<std::byte>>;

    /// Queue an asynchronous read. The callback runs on a runtime worker
    /// thread and receives the same result `read()` would return. Returns
    /// false for an unknown location, an empty callback, or a stopped runtime.
    auto read_async(std::string_view location,
                    std::function<void(result<std::vector<std::byte>>)> done)
        -> bool;

    /// Queue a prefetch of `location` (all packed-block windows). Unknown
    /// locations are ignored. Safe to call repeatedly: identical in-flight
    /// windows are coalesced.
    auto prefetch(std::string_view location) -> void;

    /// Current counters (see `content_stream_stats`).
    [[nodiscard]] auto stats() const -> content_stream_stats;

  private:
    struct impl;
    explicit content_stream(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

} // namespace refrain::data
