// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/content_stream.hpp>

#include <refrain/core/async.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

constexpr std::uint64_t content_block_bytes{65536};
constexpr std::uint64_t max_content_blocks{1U << 20};

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

/// Prefix the diagnostic while keeping the error code (parse failures must
/// stay `parse_error`/`version_mismatch` for callers to branch on).
[[nodiscard]] auto wrap_error(error failure, std::string_view context)
    -> error {
    failure.message = std::string{context} + ": " + failure.message;
    return failure;
}

/// Read one index file. A missing file is `not_found` (the composition was
/// never published, or the store is empty), matching `open_content_pack`;
/// other failures keep their code and name the path.
[[nodiscard]] auto read_index_file(std::string_view path)
    -> result<std::vector<std::byte>> {
    auto bytes{core::io::read_file(path)};
    if (bytes) {
        return bytes;
    }
    if (!core::io::exists(path)) {
        return std::unexpected{fail(error_code::not_found,
                                    "content stream index does not exist: '" +
                                        std::string{path} + "'")};
    }
    return std::unexpected{wrap_error(std::move(bytes.error()),
                                      "failed to read content stream index '" +
                                          std::string{path} + "'")};
}

} // namespace

struct content_stream::impl {
    content_store store{};
    blob_table_view table{};
    std::map<std::string, blob_location, std::less<>> winners{};
    content_stream_options options{};
    std::uint64_t window_blocks{1};

    mutable std::mutex stats_mutex{};
    content_stream_stats counters{};

    std::mutex inflight_mutex{};
    std::unordered_set<std::string> inflight{};

    // Declared last so it is destroyed first: the runtime joins its worker
    // threads before the state used by their tasks goes away.
    core::async_runtime runtime;

    impl(core::async_runtime async, content_stream_options opts)
        : options{opts}, runtime{std::move(async)} {
        const auto blocks{
            options.prefetch_window_bytes / content_block_bytes +
            (options.prefetch_window_bytes % content_block_bytes != 0U ? 1U
                                                                       : 0U)};
        window_blocks =
            std::clamp<std::uint64_t>(blocks, 1U, max_content_blocks);
    }

    [[nodiscard]] auto find_winner(std::string_view location) const
        -> const blob_location * {
        const auto found{winners.find(location)};
        return found == winners.end() ? nullptr : &found->second;
    }

    /// Read one location and update the counters. The async path captures this
    /// `impl` pointer (not the owning `content_stream`), so moving the handle
    /// while a callback is queued does not invalidate the task.
    [[nodiscard]] auto read_location(std::string_view location)
        -> result<std::vector<std::byte>> {
        const auto *found{find_winner(location)};
        if (found == nullptr) {
            return std::unexpected{
                fail(error_code::not_found, "content stream has no location '" +
                                                std::string{location} + "'")};
        }
        auto bytes{store.read(*found)};
        if (bytes) {
            record_read(bytes->size());
        }
        return bytes;
    }

    void record_read(std::size_t size) {
        std::lock_guard lock{stats_mutex};
        ++counters.reads;
        counters.bytes += static_cast<std::uint64_t>(size);
    }

    void record_prefetch_bytes(std::size_t size) {
        std::lock_guard lock{stats_mutex};
        counters.bytes += static_cast<std::uint64_t>(size);
    }

    void count_prefetch() {
        std::lock_guard lock{stats_mutex};
        ++counters.prefetches;
    }

    void count_dedup() {
        std::lock_guard lock{stats_mutex};
        ++counters.deduped;
    }

    /// Insert an in-flight window key. False when the same window is already
    /// in flight (the caller counts it as deduplicated).
    [[nodiscard]] auto mark_inflight(const std::string &key) -> bool {
        std::lock_guard lock{inflight_mutex};
        return inflight.insert(key).second;
    }

    void erase_inflight(const std::string &key) {
        std::lock_guard lock{inflight_mutex};
        inflight.erase(key);
    }

    struct prefetch_window {
        std::string key{};
        std::uint64_t offset{0};
        std::size_t size{0};
    };

    void schedule_prefetch(const blob_location &location) {
        const auto digest{to_hex(location.digest)};
        if (location.kind == blob_kind::raw) {
            auto key{digest + ":raw"};
            if (!mark_inflight(key)) {
                count_dedup();
                return;
            }
            count_prefetch();
            runtime.post(
                [state = this, location, key = std::move(key)]() mutable {
                    auto bytes{state->store.read(location)};
                    if (bytes) {
                        state->record_prefetch_bytes(bytes->size());
                    }
                    state->erase_inflight(key);
                });
            return;
        }

        std::vector<prefetch_window> windows;
        const auto block_count{
            static_cast<std::uint64_t>(location.block_count)};
        const auto blocks_per_window{window_blocks};
        const auto window_count{(block_count + blocks_per_window - 1U) /
                                blocks_per_window};
        windows.reserve(static_cast<std::size_t>(window_count));
        for (std::uint64_t window{0}; window < window_count; ++window) {
            const auto offset{window * blocks_per_window * content_block_bytes};
            if (offset >= location.logical_size) {
                break;
            }
            const auto remaining{location.logical_size - offset};
            const auto size{
                std::min(blocks_per_window * content_block_bytes, remaining)};
            if (size > std::numeric_limits<std::size_t>::max()) {
                break;
            }
            auto key{digest + ':' + std::to_string(window)};
            if (!mark_inflight(key)) {
                count_dedup();
                continue;
            }
            count_prefetch();
            windows.push_back(prefetch_window{
                .key = std::move(key),
                .offset = offset,
                .size = static_cast<std::size_t>(size),
            });
        }
        if (windows.empty()) {
            return;
        }
        runtime.post(
            [state = this, location, windows = std::move(windows)]() mutable {
                for (std::size_t index{0}; index < windows.size(); ++index) {
                    std::vector<std::byte> scratch(windows[index].size);
                    auto copied{state->store.read_range(
                        location, windows[index].offset, scratch)};
                    state->erase_inflight(windows[index].key);
                    if (!copied) {
                        // A failed window does not invalidate the others; drop
                        // the remaining keys so a later prefetch can retry.
                        for (std::size_t rest{index + 1U};
                             rest < windows.size(); ++rest) {
                            state->erase_inflight(windows[rest].key);
                        }
                        return;
                    }
                    state->record_prefetch_bytes(*copied);
                }
            });
    }
};

content_stream::content_stream() noexcept = default;

content_stream::content_stream(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

content_stream::content_stream(content_stream &&other) noexcept = default;

auto content_stream::operator=(content_stream &&other) noexcept
    -> content_stream & = default;

content_stream::~content_stream() = default;

auto content_stream::open(std::string_view store_root,
                          std::string_view composition,
                          content_stream_options options)
    -> result<content_stream> {
    if (store_root.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "content stream store root must not be empty")};
    }
    if (!is_valid_utf8(store_root)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "content stream store root is not valid UTF-8")};
    }
    if (!is_valid_content_composition(composition)) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "content stream composition must be a safe file name of at most " +
                std::to_string(max_content_composition_bytes) +
                " bytes ([A-Za-z0-9._-], not starting with '.')")};
    }
    if (options.worker_threads == 0U) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "content stream needs at least one async worker thread")};
    }
    if (options.prefetch_window_bytes == 0U) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "content stream prefetch window must be non-zero")};
    }

    auto store{content_store::open(store_root)};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }

    // RFBT and RFRX are validated as a pair: the resolution index binds itself
    // to the exact table bytes (BLAKE3 prefix), so a stale index fails closed.
    const auto table_path{content_blob_table_path(store_root)};
    auto table_bytes{read_index_file(table_path)};
    if (!table_bytes) {
        return std::unexpected{std::move(table_bytes.error())};
    }
    auto table{parse_blob_table(*table_bytes)};
    if (!table) {
        return std::unexpected{
            wrap_error(std::move(table.error()),
                       "content blob table '" + table_path + "'")};
    }

    const auto index_path{content_resolution_path(store_root, composition)};
    auto index_bytes{read_index_file(index_path)};
    if (!index_bytes) {
        return std::unexpected{std::move(index_bytes.error())};
    }
    auto index{parse_resolution_index(*index_bytes, *table)};
    if (!index) {
        return std::unexpected{
            wrap_error(std::move(index.error()),
                       "content resolution index '" + index_path + "'")};
    }

    // One winner per location via the shared winner order
    // (`resolution_precedes`: priority desc, then blob_index asc, then source
    // asc). The RFRX sort key does not include the blob, so a legacy/manual
    // same-priority tie may list either blob first; this keeps the stream's
    // winner independent of the record order. `current` holds pointers into
    // `index`, which outlives the conversion below.
    std::map<std::string, const resolution_entry *, std::less<>> current;
    for (const auto &entry : index->entries) {
        if (entry.blob_index >= table->entries.size()) {
            return std::unexpected{
                fail(error_code::parse_error,
                     "content resolution index '" + index_path +
                         "' references a missing blob table record")};
        }
        const auto found{current.find(entry.location)};
        if (found == current.end()) {
            current.emplace(entry.location, &entry);
        } else if (resolution_precedes(entry, *found->second)) {
            found->second = &entry;
        }
    }
    std::map<std::string, blob_location, std::less<>> winners;
    for (const auto &[location, record] : current) {
        winners.emplace(location, blob_location_from_table_entry(
                                      table->entries[record->blob_index]));
    }

    auto runtime{core::async_runtime::open(options.worker_threads)};
    if (!runtime) {
        return std::unexpected{std::move(runtime.error())};
    }
    auto state{std::make_unique<impl>(std::move(*runtime), options)};
    state->store = std::move(*store);
    state->table = std::move(*table);
    state->winners = std::move(winners);
    return content_stream{std::move(state)};
}

auto content_stream::valid() const noexcept -> bool { return impl_ != nullptr; }

auto content_stream::read(std::string_view location) const
    -> result<std::vector<std::byte>> {
    if (impl_ == nullptr) {
        return std::unexpected{
            fail(error_code::not_initialized, "content stream is not open")};
    }
    return impl_->read_location(location);
}

auto content_stream::read_async(
    std::string_view location,
    std::function<void(result<std::vector<std::byte>>)> done) -> bool {
    if (impl_ == nullptr || !done || !impl_->runtime.valid()) {
        return false;
    }
    if (impl_->find_winner(location) == nullptr) {
        return false;
    }
    impl_->runtime.post([state = impl_.get(), location = std::string{location},
                         done = std::move(done)]() mutable {
        done(state->read_location(location));
    });
    return true;
}

auto content_stream::prefetch(std::string_view location) -> void {
    if (impl_ == nullptr || !impl_->runtime.valid()) {
        return;
    }
    const auto *found{impl_->find_winner(location)};
    if (found == nullptr) {
        return;
    }
    impl_->schedule_prefetch(*found);
}

auto content_stream::stats() const -> content_stream_stats {
    if (impl_ == nullptr) {
        return {};
    }
    std::lock_guard lock{impl_->stats_mutex};
    return impl_->counters;
}

} // namespace refrain::data
