// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Path-addressed object store for RWF auxiliary data. One RFOS index plus
// immutable RFSG segments:
//
//   objects/index.rfw              # RFMF frame, payload = RFOS path index
//   objects/segments/<id16>.rfseg  # RFSG, compressed objects
//
// `write(..., durable = false)` batches; `commit()` seals one segment, then
// publishes the index atomically. Data is published before the index, so a
// crash leaves at most orphan segments. Paths are relative, '/'-separated and
// validated; `media` is a hint for export/decoding, not part of the content.

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::world {

/// Payload kind hint stored in the object index.
enum class object_media : std::uint8_t {
    binary = 0,
    nbt = 1,
    json = 2,
    png = 3,
    zip = 4,
    text = 5,
};

struct object_store_stats {
    std::size_t objects{0};
    std::size_t segments{0};
    std::uint64_t live_bytes{0};
    std::uint64_t garbage_bytes{0};
};

/// Upper bound for one object payload (both decoded and stored). Large
/// datapacks/aux files above this fail closed instead of driving an
/// unbounded allocation from the object index.
inline constexpr std::size_t max_object_bytes{64ULL << 20U};

class object_store {
  public:
    explicit object_store(std::string root);
    ~object_store();

    object_store(const object_store &) = delete;
    auto operator=(const object_store &) -> object_store & = delete;
    object_store(object_store &&other) noexcept;
    auto operator=(object_store &&other) noexcept -> object_store &;

    [[nodiscard]] auto root() const -> const std::string &;

    [[nodiscard]] auto write(std::string_view path,
                             std::span<const std::byte> payload,
                             object_media media, bool durable = true) -> status;

    [[nodiscard]] auto read(std::string_view path,
                            std::size_t max_bytes = max_object_bytes)
        -> result<std::vector<std::byte>>;

    [[nodiscard]] auto erase(std::string_view path) -> status;

    [[nodiscard]] auto exists(std::string_view path) const -> bool;

    /// Sorted paths beginning with `prefix` (empty = all).
    [[nodiscard]] auto list(std::string_view prefix = {}) const
        -> result<std::vector<std::string>>;

    [[nodiscard]] auto commit() -> status;

    /// Rewrite only live objects into a fresh segment and publish a new index;
    /// old segments are removed. Returns the post-compaction statistics.
    [[nodiscard]] auto compact() -> result<object_store_stats>;

    /// Relative, '/'-separated, ≤512 bytes, ≤32 components; no empty, '.',
    /// '..' or backslash components; characters [A-Za-z0-9._-].
    [[nodiscard]] static auto valid_path(std::string_view path) -> bool;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::world
