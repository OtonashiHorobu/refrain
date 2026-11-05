// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::archive {

// Read-only ZIP reader used by `refrain-tools extract` to import data/assets
// from a user-local Minecraft jar. This is a thin, safe facade over libzip that
// the resource pipeline builds on. It never ships Mojang content: the
// repository and CI only use synthetic archives, and extraction output stays in
// the user's local cache.

struct zip_entry {
    std::string name{};
    std::uint64_t index{0}; ///< central-directory ordinal (stable for a file)
    std::uint64_t size{0};  ///< uncompressed bytes
    std::uint64_t compressed_size{0};
    std::uint32_t crc32{0};
    std::uint16_t method{0}; ///< 0 = stored, 8 = deflate
    bool directory{false};
};

/// Hard safety ceiling for one entry's declared uncompressed size. The size
/// comes from the (untrusted) central directory, so a forged/hostile archive
/// could otherwise make `read()` allocate an arbitrary amount of memory
/// (zip-bomb/OOM). 512 MiB is far above any official data or resource-pack
/// file while keeping hostile sizes bounded.
inline constexpr std::uint64_t max_entry_bytes = 512ULL * 1024ULL * 1024ULL;

/// Hard safety ceiling for the number of central-directory entries. The count
/// is read from the (untrusted) archive before anything is allocated, so
/// `open()` rejects archives above this bound instead of reserving an
/// attacker-chosen number of entries (memory-exhaustion/OOM). One million is
/// orders of magnitude above any official data/resource pack while keeping the
/// worst-case reservation bounded (~a few tens of MiB for empty entry records).
inline constexpr std::uint64_t max_entries{1'000'000};

/// Read-only archive handle. Move-only; all errors are values.
class zip_reader {
  public:
    zip_reader(zip_reader &&other) noexcept;
    auto operator=(zip_reader &&other) noexcept -> zip_reader &;
    ~zip_reader();

    zip_reader(const zip_reader &) = delete;
    auto operator=(const zip_reader &) -> zip_reader & = delete;

    [[nodiscard]] static auto open(std::string_view path) -> result<zip_reader>;

    [[nodiscard]] auto archive_path() const noexcept -> std::string_view;

    /// Entries in central-directory order (directories included).
    [[nodiscard]] auto entries() const noexcept
        -> const std::vector<zip_entry> &;

    /// O(1) lookup by exact name (hash index built once at `open()`); returns
    /// nullptr when the archive has no such entry. Two entries with the same
    /// name keep the first central-directory occurrence. The index keeps
    /// extraction linear in the number of entries.
    [[nodiscard]] auto find(std::string_view name) const noexcept
        -> const zip_entry *;

    /// Read the full uncompressed content of one entry.
    [[nodiscard]] auto read(const zip_entry &entry)
        -> result<std::vector<std::byte>>;
    [[nodiscard]] auto read(std::string_view name)
        -> result<std::vector<std::byte>>;

    /// Extract one entry below `output_root`, creating missing directories.
    /// Rejects unsafe names (zip-slip, absolute paths, drive letters, `..`).
    [[nodiscard]] auto extract(const zip_entry &entry,
                               std::string_view output_root) -> status;

  private:
    struct impl;

    explicit zip_reader(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

/// True when `name` can be safely joined below an output root: relative,
/// '/'-separated, no empty/`.`/`..` segments, no drive letters, no backslashes
/// or control characters.
///
/// Portable-name rules are enforced unconditionally, on POSIX hosts too
/// (not only when running on Windows): Windows device names (`CON`, `NUL`,
/// `AUX`, `COM1`-`COM9`, `LPT1`-`LPT9`, with or without a file extension, any
/// case) and segments with a trailing dot or space are rejected, because
/// Windows would alias the former to a device and silently rewrite the latter.
/// Manifest building additionally rejects names that collide only by ASCII case
/// (Windows/macOS default file systems are case-insensitive), which
/// `build_manifest()` documents.
[[nodiscard]] auto safe_relative_path(std::string_view name) noexcept -> bool;

/// Join a validated relative path below `root` (adds exactly one separator).
[[nodiscard]] auto join_under(std::string_view root, std::string_view relative)
    -> std::string;

} // namespace refrain::archive
