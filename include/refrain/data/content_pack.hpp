// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/vfs.hpp>

#include <cstddef>
#include <memory>
#include <string_view>

namespace refrain::data {

// Read-only VFS pack backed by one composition of the content-addressed store.
//
// A content store has no self-describing container file: a composition is the
// pair of a global blob table (`<store>/content/blobs.idx`, RFBT) and a
// resolution index (`<store>/content/resolutions/<composition>.idx`, RFRX).
// `open_content_pack()` validates both and turns every *winner* entry of the
// resolution index into a pack entry:
//
//  - `path` is the resolution `location` (already a safe relative UTF-8 path,
//    enforced by the RFRX parser);
//  - `size` is the winner blob's `logical_size` from the blob table;
//  - entries are re-sorted by path bytes (RFRX is ordered by `location_hash`,
//    which is not a path order and cannot be exposed as the pack entry list);
//  - the pack exposes one winner per location, i.e. the runtime resolved view:
//    highest `priority`, then smallest `blob_index`, then lexicographically
//    smallest `source` (the same order as `resolve()`). A legacy/manual index
//    whose entries tie on priority therefore has a winner independent of the
//    on-disk record order. The remaining records are the fallback chain and
//    are not visible through the pack; `content where` still shows the full
//    layered chain from the on-disk index.
//
// `read()` resolves the path to its blob digest and asks the pack's own
// `content_store` for the logical bytes (raw or packed, digest-verified). The
// error mapping follows the `pack` contract: an unknown virtual path is
// `not_found`, a corrupt store blob keeps `parse_error`, and host filesystem
// failures surface as `io_error`. An empty resolution index is a valid empty
// pack; `vfs::mount()` still rejects empty packs, which is the caller's
// semantic decision.
//
// The store is opened (and its `content/blobs` directory created, as
// `content_store::open()` does) when the pack is opened. The pack is immutable
// afterwards, so `read()` may be called concurrently once mounted into a
// frozen `vfs`; it re-reads blob files on every call, so an externally damaged
// store surfaces as an error instead of stale bytes.
//
// `pack_format::content` cannot be opened from a bare path (`vfs::mount(
// const pack_source&)` rejects it): mount the returned pack through
// `vfs::mount(std::unique_ptr<pack>)`.

/// Maximum byte length of a composition id (also its index file name stem).
inline constexpr std::size_t max_content_composition_bytes{128};

/// True when `composition` is a safe resolution-index file stem: non-empty, at
/// most `max_content_composition_bytes`, only `[A-Za-z0-9._-]`, and not
/// starting with '.' (so `../`, absolute paths and hidden/dotted names are
/// rejected). Shared by the content-pack and content-stream open paths.
[[nodiscard]] auto
is_valid_content_composition(std::string_view composition) noexcept -> bool;

/// Open one composition of the content store at `store_root` as a read-only
/// pack.
///
/// `store_root` must be non-empty valid UTF-8. `composition` names
/// `<store_root>/content/resolutions/<composition>.idx` and must be a safe
/// file name: non-empty, at most `max_content_composition_bytes`, only
/// `[A-Za-z0-9._-]`, and not starting with '.' (so `../`, absolute paths and
/// hidden/dotted names are rejected with `invalid_argument`). `label` is
/// diagnostic only; empty defaults to `"content:" + composition`.
///
/// A missing blob table or resolution index is `not_found`; parse failures of
/// either index keep their code (`parse_error`, or `version_mismatch` for a
/// foreign format version) and carry the offending path in the message.
///
/// The returned pack is the resolved runtime view: one winner per location
/// (highest priority, then smallest `blob_index`, then lexicographically
/// smallest source). No separate `<composition>.resolved.idx` artifact is
/// read or written; the full layered chain stays available through
/// `content where` on the on-disk index.
[[nodiscard]] auto open_content_pack(std::string_view store_root,
                                     std::string_view composition,
                                     std::string_view label = {})
    -> result<std::unique_ptr<pack>>;

} // namespace refrain::data
