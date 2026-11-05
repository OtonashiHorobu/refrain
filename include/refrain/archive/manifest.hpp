// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/archive/zip.hpp>
#include <refrain/core/error.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::archive {

// Data manifest produced by `refrain-tools extract`: a deterministic
// inventory of a user-local jar. It never contains Mojang payloads, only
// paths, sizes and CRCs, so it can be inspected without shipping game content.

/// Which archive entries the manifest/extraction covers.
enum class extract_filter : std::uint8_t {
    all = 0, ///< every regular file (data, assets, META-INF, …)
    data,    ///< `data/` only
    assets,  ///< `assets/` only
};

[[nodiscard]] auto filter_from_string(std::string_view name) noexcept
    -> extract_filter;
[[nodiscard]] auto to_string(extract_filter filter) noexcept
    -> std::string_view;

struct manifest_entry {
    std::string path{};
    std::uint64_t size{0};
    std::uint64_t compressed_size{0};
    std::uint32_t crc32{0};
    std::string category{}; ///< "data" | "assets" | "meta" | "other"
};

struct data_manifest {
    std::string schema{"refrain.data-manifest/1"};
    std::string source{}; ///< archive file name only, no host path
    std::uint64_t source_size{0};
    std::string generator{}; ///< refrain-tools version
    /// Every central-directory entry, directories included (the archive's
    /// inventory size, independent of `filter`).
    std::uint64_t total_entries{0};
    /// Regular files selected by `filter`; equals `entries.size()`.
    std::uint64_t total_files{0};
    /// Sum of `entries[*].size` (uncompressed bytes; checked for overflow).
    std::uint64_t total_bytes{0};
    std::vector<std::string> categories{}; ///< sorted, present only
    std::vector<manifest_entry> entries{}; ///< sorted by path
};

/// Build the manifest for the entries selected by `filter`.
///
/// Fails with `parse_error` on unsafe/unportable paths (zip-slip, Windows
/// device names, trailing dots/spaces), oversized entries and `total_bytes`
/// overflow; fails with `already_exists` when two entries collide exactly or
/// differ only by ASCII case.
///
/// The portability rules are unconditional: they apply on every host, not just
/// Windows/macOS. The manifest must describe a pack that extracts identically
/// everywhere, so a POSIX build rejects `CON`/`a.`/`Alpha.txt`+`alpha.txt`
/// entries too.
[[nodiscard]] auto build_manifest(const zip_reader &reader,
                                  extract_filter filter)
    -> result<data_manifest>;

/// Deterministic compact JSON (simdjson `string_builder`) for tooling/CI
/// inspection; field order and one-line layout are part of the output contract.
[[nodiscard]] auto manifest_to_json(const data_manifest &manifest)
    -> result<std::string>;

/// True when the entry is selected by `filter` (files only).
[[nodiscard]] auto matches_filter(std::string_view path,
                                  extract_filter filter) noexcept -> bool;

} // namespace refrain::archive
