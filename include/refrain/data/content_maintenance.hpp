// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

// Content-store maintenance shared by `refrain-tools content build|gc` and the
// startup content bootstrap.
//
// The data layer owns the publication and mark-sweep algorithms; it never
// depends on the mod layer. Receipts are a mod concept, so the caller parses
// them (mod::list_receipt_paths/read_receipt) and passes their file digests as
// `gc_options::extra_roots`). In the same way, `content build` keeps its
// directory scan / batch wiring in the CLI and only reuses the atomic RFRX
// publication primitive here.

/// Segment size hint used by `content build` and by compaction (the same soft
/// target as `content_store::batch`).
inline constexpr std::uint32_t content_segment_target_bytes{32U * 1024U *
                                                            1024U};

/// Upper bound of `gc_options::keep_days`. The cutoff is computed as
/// `now - hours(24 * keep_days)`; converting that to the filesystem clock's
/// nanosecond duration overflows `int64` above ~106,751 days (signed-overflow
/// UB, and the wrapped cutoff silently disables the retention window). The
/// library rejects larger values instead of clamping, so a typo cannot turn
/// into "delete everything"; 100 years is far beyond any real retention.
inline constexpr int max_gc_keep_days{36500};

/// One entry of the persistent pending-delete queue
/// (`<store>/content/gc/pending.json`).
///
/// `path` is store-relative and '/'-separated; the kind must agree with the
/// prefix (`content/blobs/...` = blob, `content/segments/...` = segment).
/// `size` is the physical size recorded at plan time (accounting only; the
/// drain does not stat it again) and `attempts` counts failed drain attempts.
struct pending_delete {
    enum class pending_delete_kind : std::uint8_t { blob, segment };

    std::string path{};
    pending_delete_kind kind{pending_delete_kind::blob};
    std::uint64_t size{0};
    std::uint32_t attempts{0};
};

/// Retry limit of one pending-delete entry. Once an entry reaches it, the
/// drain stops attempting it but keeps it in the queue (fail closed: a stuck
/// deletion is never silently dropped) and reports it through
/// `pending_delete_report::failed_stuck`; an operator repairs the filesystem or
/// removes the entry by hand. The queue length is capped separately by
/// `max_pending_delete_entries`.
inline constexpr std::uint32_t max_pending_delete_attempts{8};

/// Hard cap on the number of entries in the pending-delete queue. `enqueue`
/// fails with `invalid_argument` and `load` with `parse_error` above it, so a
/// corrupt/foreign queue cannot exhaust memory. The cap is ~100k paths, far
/// above a realistic sweep batch.
inline constexpr std::size_t max_pending_delete_entries{100000};

/// Counters of one `drain_pending_deletes` pass.
///
/// `deleted` includes entries whose target was already missing (the cleanup is
/// idempotent); `deferred` were not attempted because `budget_ms` expired;
/// `failed` hit an I/O error and stay queued with an incremented `attempts`.
/// `failed_stuck` counts entries at the retry limit after the pass (either this
/// pass's failure reached `max_pending_delete_attempts`, or the entry was
/// skipped because it was already there); they stay queued.
/// `remaining` is the queue size after the pass. The byte counters sum the
/// queued `pending_delete::size` values; they are not a fresh `stat()` of
/// whatever currently sits at the path. The per-kind counters exist so
/// `gc_report` and the stable CLI lines can keep their blob/segment split.
struct pending_delete_report {
    std::uint64_t deleted{0};
    std::uint64_t deleted_bytes{0};
    std::uint64_t deleted_blobs{0};
    std::uint64_t deleted_blob_bytes{0};
    std::uint64_t deleted_segments{0};
    std::uint64_t deleted_segment_bytes{0};
    std::uint64_t deferred{0};
    std::uint64_t deferred_bytes{0};
    std::uint64_t deferred_blobs{0};
    std::uint64_t deferred_blob_bytes{0};
    std::uint64_t deferred_segments{0};
    std::uint64_t deferred_segment_bytes{0};
    std::uint64_t failed{0};
    std::uint64_t failed_bytes{0};
    std::uint64_t failed_blobs{0};
    std::uint64_t failed_blob_bytes{0};
    std::uint64_t failed_segments{0};
    std::uint64_t failed_segment_bytes{0};
    std::uint64_t failed_stuck{0};
    /// Entries dropped because `blobs.idx` records the target again (a
    /// concurrent install/build resurrected the blob or segment after it was
    /// queued). They are not deleted and leave the queue.
    std::uint64_t skipped_referenced{0};
    std::uint64_t remaining{0};
    bool budget_exhausted{false};
};

/// `<store>/content/blobs.idx`.
[[nodiscard]] auto content_blob_table_path(std::string_view store_root)
    -> std::string;

/// `<store>/content/resolutions/<composition>.idx` (the composition is used
/// verbatim as the file stem; callers validate it before publishing).
[[nodiscard]] auto content_resolution_path(std::string_view store_root,
                                           std::string_view composition)
    -> std::string;

/// Sorted `<store>/content/resolutions/*.idx` paths. A missing directory (a
/// store that never published an index) is the empty list.
[[nodiscard]] auto list_resolution_paths(std::string_view store_root)
    -> result<std::vector<std::string>>;

/// Read and fully validate `<store>/content/blobs.idx`. A missing/unreadable
/// file keeps `io_error`; a damaged or foreign-version table is `parse_error`.
/// Both errors name the table path.
[[nodiscard]] auto load_blob_table(std::string_view store_root)
    -> result<blob_table_view>;

/// One parsed `<composition>.idx` together with its path.
struct resolution_index_file {
    std::string path{};
    resolution_index_view view{};
};

/// Parse every `<store>/content/resolutions/*.idx` except `skip_path` against
/// `table`. The binding is strict (RFRX stores the first 8 bytes of the table
/// BLAKE3), so a stale index fails before the caller touches any file. A
/// missing/unreadable index is `io_error` (wrapped with its path), damage is
/// `parse_error`.
[[nodiscard]] auto load_resolution_indexes(std::string_view store_root,
                                           const blob_table_view &table,
                                           std::string_view skip_path = {})
    -> result<std::vector<resolution_index_file>>;

/// One sealed `.rfseg` found on disk. `header_ok` records whether the 80-byte
/// header parsed (body hashes are only checked by `content verify --blobs`);
/// `modified` is used by the GC `--keep-days` window.
struct content_segment_file {
    std::uint64_t id{0};
    std::string path{};
    std::uint64_t file_size{0};
    std::uint64_t data_bytes{0};
    std::uint32_t blob_count{0};
    bool header_ok{false};
    std::filesystem::file_time_type modified{};
};

/// Sorted sealed segments under `<store>/content/segments`. A missing directory
/// is an empty set. A segment whose header cannot be parsed is still listed
/// (`header_ok == false`) so GC can classify it instead of silently skipping a
/// reference. Staging `.part` files and foreign names are ignored.
[[nodiscard]] auto scan_content_segments(std::string_view store_root)
    -> result<std::vector<content_segment_file>>;

/// Read and validate `<store>/content/gc/pending.json`.
///
/// A missing queue file is the empty list (success). The queue is treated as
/// untrusted input and fails closed: a truncated/damaged/foreign-schema file,
/// a non-regular file, duplicate or unsorted entries, or a path that is not a
/// safe store-relative path under `content/blobs/` (`kind == blob`) or
/// `content/segments/` (`kind == segment`) is `parse_error`. A damaged queue is
/// never silently dropped.
[[nodiscard]] auto load_pending_deletes(std::string_view store_root)
    -> result<std::vector<pending_delete>>;

/// Merge `entries` into `<store>/content/gc/pending.json` and atomically
/// replace the file (temporary sibling + fsync + rename + parent-directory
/// fsync, via `core::io::atomic_write_text`).
///
/// Entries are deduplicated by path; a duplicate keeps the larger `attempts`
/// and `size`, so a re-plan can never reset the retry counter of an entry that
/// is already queued. The written list is sorted by path bytes and carries no
/// timestamps, so equal inputs produce byte-identical files. Every incoming
/// path is validated with the same rules as `load_pending_deletes`
/// (`invalid_argument`). A merged queue above `max_pending_delete_entries`
/// fails with `invalid_argument`. An existing but corrupt queue is
/// `parse_error` and is left untouched (never overwritten). An empty `entries`
/// span is a no-op (it never creates the file).
[[nodiscard]] auto
enqueue_pending_deletes(std::string_view store_root,
                        std::span<const pending_delete> entries) -> status;

/// Delete queued targets until `budget_ms` expires (0 = unlimited).
///
/// The budget is checked before every unlink; a single `remove` may still block
/// (POSIX/Windows give no portable interrupt), which is exactly why the server
/// drains this queue on its own `gc-delete` thread instead of the tick thread.
/// Each successful deletion (or an already missing target) removes its entry;
/// an I/O failure increments `attempts` (saturating) and keeps the entry for
/// the next pass; an entry at `max_pending_delete_attempts` is skipped and
/// counted in `failed_stuck`; deferred entries stay untouched. An empty
/// resulting queue removes the queue file and fsyncs its directory. A missing
/// queue file is a successful empty pass. Repeated drains are idempotent.
///
/// `log`, when set, receives stable lines without the `gc: ` prefix and
/// without a trailing newline: one `failed to delete '...': ...` per
/// failure, a retry-limit failure line, then the `deleted`/`deferred`/`failed`
/// summary lines.
///
/// Callers must serialise concurrent `enqueue`/`drain` calls for one store
/// (the server shares one mutex between planning and the worker).
[[nodiscard]] auto
drain_pending_deletes(std::string_view store_root, std::uint64_t budget_ms,
                      const std::function<void(std::string_view)> &log = {})
    -> result<pending_delete_report>;

/// Input of `publish_resolution`: the target composition and the complete set
/// of entries of its replacement index (`location/source/priority/blob`).
struct content_publish_options {
    std::string composition{};
    std::vector<resolution_input> entries{};
};

/// Build a resolution index from `options.entries` against the current
/// `<store>/content/blobs.idx`, then atomically publish it as
/// `<store>/content/resolutions/<composition>.idx` (temporary sibling + fsync +
/// rename). `blobs.idx` is only read: refcounts are hints and stay the GC's
/// responsibility.
///
/// A missing or damaged table and any entry whose digest is absent from it are
/// errors; nothing is written in that case. The composition must be a safe
/// single path segment (non-empty, at most 200 UTF-8 bytes, not "."/"..", no
/// '/'/'\'/control bytes) so it cannot escape the resolutions directory.
[[nodiscard]] auto publish_resolution(std::string_view store_root,
                                      const content_publish_options &options)
    -> status;

/// Atomically publish already-built RFRX bytes at `index_path` (parent
/// directories are created). This is the shared low-level primitive used by
/// `publish_resolution`, `content build` and `content gc`, which pre-build
/// every replacement index in memory before publishing a new blobs.idx (the
/// table's BLAKE3 prefix binds all indexes, so they must all be ready first).
/// The bytes are not re-validated here; the caller built them with
/// `build_resolution_index`.
[[nodiscard]] auto publish_resolution_bytes(std::string_view index_path,
                                            std::span<const std::byte> bytes)
    -> status;

/// Options of `collect_garbage`.
struct gc_options {
    /// Delete and republish; false only plans and never writes.
    bool apply{false};
    /// Rewrite segments at least 50% dead (only with `apply`; a dry run just
    /// reports the plan).
    bool compact{false};
    /// Keep unreferenced blobs/segments newer than this many days.
    int keep_days{0};
    /// Wall-time budget of the synchronous deletion drain in milliseconds
    /// (0 = unlimited), passed to `drain_pending_deletes`. Ignored with
    /// `defer_deletes` (the background worker owns its own budget). The clock
    /// starts after the rebuilt table/indexes are published and the plan is
    /// enqueued, right before the first unlink, so planning, marking,
    /// re-encoding and compaction publication are never bounded. When the
    /// budget expires, the remaining queued entries stay in the queue and on
    /// disk, are reported as deferred (`gc_report::deferred_*`), and the next
    /// drain (or the worker) retries them. Only `apply` deletes, so a dry run
    /// ignores this field.
    std::uint64_t budget_ms{0};
    /// Additional GC roots supplied by the caller. Receipt files live in the
    /// mod layer, so the caller parses them and appends every `files[].blake3`
    /// entry here (duplicates count as separate references). The library only
    /// ever sees digests.
    std::vector<digest32> extra_roots{};
    /// With `apply`, persist the planned blobs/segments into
    /// `<store>/content/gc/pending.json` and leave the physical deletions to
    /// `drain_pending_deletes`. False drains the queue
    /// synchronously before returning (foreground CLI/startup); true returns
    /// right after the enqueue and lets the `gc-delete` worker drain it
    /// off-thread. A dry run ignores this field. Default false preserves the
    /// synchronous CLI/startup behaviour (the unlink still runs only in the
    /// queue drain, never inline in the planner).
    bool defer_deletes{false};
};

/// Statistics of one `collect_garbage` run. The `root_*`/plan fields are
/// filled in both modes; `deleted_*`/`compacted_*` only change with `apply`.
struct gc_report {
    /// True when the run applied (wrote a table and/or enqueued deletions).
    bool applied{false};
    /// RFRX entries (every entry is one reference; duplicates included).
    std::uint64_t root_index_references{0};
    /// References supplied through `gc_options::extra_roots` (for the CLI:
    /// the `files[].blake3` records of every install receipt).
    std::uint64_t root_extra{0};
    /// Distinct blobs in the root set.
    std::uint64_t root_blobs{0};
    /// Blobs in `blobs.idx` after the run (a dry run reports the current
    /// table).
    std::uint64_t table_blobs{0};
    /// Physical standalone blobs outside the root set (plan; before deletion).
    std::uint64_t unreferenced_blobs{0};
    std::uint64_t unreferenced_bytes{0};
    /// Segments not referenced by the current table (plan; before deletion,
    /// after the `keep_days` window).
    std::uint64_t orphan_segments{0};
    std::uint64_t orphan_bytes{0};
    /// Segments selected for compaction (>= 50% dead bytes).
    std::uint64_t compact_candidates{0};
    std::uint64_t compact_live_bytes{0};
    std::uint64_t compact_dead_bytes{0};
    /// Candidate segments actually compacted and the new segments sealed.
    std::uint64_t compacted_segments{0};
    std::uint64_t new_segments{0};
    /// Physical deletions performed by the queue drain (`apply` only; includes
    /// entries left by earlier runs whose target was already missing).
    std::uint64_t deleted_blobs{0};
    std::uint64_t deleted_blob_bytes{0};
    std::uint64_t deleted_segments{0};
    std::uint64_t deleted_segment_bytes{0};
    /// Queued items not attempted because the deletion budget expired (apply
    /// only). They stay in the persistent queue (and on disk); the next drain
    /// retries them. The rebuilt table is already published, so the store is
    /// consistent in between.
    std::uint64_t deferred_blobs{0};
    std::uint64_t deferred_blob_bytes{0};
    std::uint64_t deferred_segments{0};
    std::uint64_t deferred_segment_bytes{0};
    /// True when `gc_options::budget_ms` stopped the deletion drain with at
    /// least one queued item deferred (not an error).
    bool budget_exhausted{false};

    // Fields appended after the original report layout: adding them at the end
    // keeps the existing offset/ABI of the original counters.
    /// Items planned by this run and persisted to the pending-delete queue
    /// (`apply` only). With `defer_deletes` they remain for the worker; with a
    /// synchronous drain they are usually deleted again before the call
    /// returns (a budget/failure may leave them queued).
    std::uint64_t queued_blobs{0};
    std::uint64_t queued_blob_bytes{0};
    std::uint64_t queued_segments{0};
    std::uint64_t queued_segment_bytes{0};
    /// Queued items whose unlink failed (apply only). They stay queued with an
    /// incremented `attempts`; `failed_stuck` counts the entries that reached
    /// `max_pending_delete_attempts` and are no longer retried automatically.
    std::uint64_t failed_blobs{0};
    std::uint64_t failed_blob_bytes{0};
    std::uint64_t failed_segments{0};
    std::uint64_t failed_segment_bytes{0};
    std::uint64_t failed_stuck{0};
    /// Queued entries dropped without deleting because the current `blobs.idx`
    /// references them again (stale deletion intents).
    std::uint64_t skipped_referenced{0};
};

/// Plan or apply content-store mark-sweep.
///
/// Roots are every RFRX reference plus `options.extra_roots`. The rebuilt table
/// keeps exactly the root digests (with recomputed refcounts; the stored value
/// is only a hint), then all RFRX files are re-encoded against the new table
/// because its BLAKE3 prefix and every `blob_index` may change. Zero-reference
/// table records are dropped; a blob inside a segment is reclaimed when the
/// segment loses its last live record; orphan segments, unreferenced standalone
/// blobs and (with `compact`) segments at least 50% dead are all reclaimed.
///
/// Fail-closed (apply only, checked before any write): a root digest whose
/// record has no physical blob (standalone file missing, segment missing/
/// unreadable/out-of-bounds) or a non-empty table with an empty root set aborts
/// without publishing or deleting anything. The caller is responsible for
/// loading receipts strictly first; a corrupt receipt never reaches this
/// function.
///
/// When `log` is non-empty, every stable report line is emitted without the
/// `gc: ` prefix and without a trailing newline, in CLI order: the roots line
/// first, then (dry run) plan and failure lines, or (apply) `blobs.idx`,
/// compaction and deletion lines. This is what `content gc` forwards verbatim
/// and what the startup bootstrap logs.
///
/// Apply never unlinks inline. After publishing the rebuilt `blobs.idx` and all
/// RFRX, the planned blobs/segments are merged into
/// `<store>/content/gc/pending.json`, and the unlink path is always
/// `drain_pending_deletes`. With
/// `options.defer_deletes` the call returns right after the enqueue (the
/// server's `gc-delete` worker owns the deletions); otherwise it drains the
/// queue synchronously and maps the drain counters into `gc_report`.
/// `options.budget_ms` bounds that synchronous drain (0 = unlimited): the
/// `steady_clock` starts after the table/index publication and enqueue, right
/// before the first unlink, and every iteration checks the elapsed time before
/// calling `std::filesystem::remove`. Once the budget is exhausted the
/// remaining queued entries are reported through `gc_report::deferred_*`, stay
/// on disk and in the queue, `budget_exhausted` is set, and the run still
/// succeeds (deferral is not an I/O error). The store is consistent at that
/// point because the new table is already published.
///
/// Deletion failures are not returned as an error: the drain keeps the entry,
/// increments `attempts` (up to `max_pending_delete_attempts`), emits a failure
/// line through `log`, and reports the counters through `gc_report::failed_*` /
/// `failed_stuck`. A later drain (or the worker) retries; a stuck entry stays
/// queued for manual repair. A corrupt/foreign pending queue fails the apply
/// with `parse_error` before any table is published.
[[nodiscard]] auto
collect_garbage(std::string_view store_root, const gc_options &options,
                const std::function<void(std::string_view)> &log = {})
    -> result<gc_report>;

} // namespace refrain::data
