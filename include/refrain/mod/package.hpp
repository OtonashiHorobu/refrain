// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/discovery.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::mod {

// Package sources: the host never assumes where a mod's files
// live. It asks a `package_resolver` for a `package_source` per candidate and
// reads `manifest.toml` (and, later, content files) through that source.
//
// Implementations:
//  * `builtin_package_source` — the embedded RFSI image (manifest/data of every
//    internal mod);
//  * `directory_package_source` — `<root>/mods/<id>/` on disk (data-only mods
//    discovered by `mod::scan`; code loading is not implemented).
// `.rfmod` archives are installed as directory trees, so the native
// archive source (`package_kind::rfmod`) is reserved for a future loader.

/// One opened mod package. Reads are relative to the package root
/// ('/'-separated, no `..`, never a symlink) and bounded by `max_read_bytes`.
class package_source {
  public:
    package_source() = default;
    package_source(const package_source &) = delete;
    auto operator=(const package_source &) -> package_source & = delete;
    package_source(package_source &&) = delete;
    auto operator=(package_source &&) -> package_source & = delete;
    virtual ~package_source() = default;

    [[nodiscard]] virtual auto kind() const noexcept -> package_kind = 0;

    /// Read one regular file of the package. Errors: `not_found` for an absent
    /// member, `invalid_argument` for an unsafe path or a file over the size
    /// limit, `permission_denied` for a symlink, `io_error`/`parse_error` for
    /// unreadable/damaged storage.
    [[nodiscard]] virtual auto read(std::string_view relative_path) const
        -> result<std::vector<std::byte>> = 0;

    /// Whole embedded image bytes when this source is backed by one (the
    /// builtin RFSI image); empty for other sources. Providers such as
    /// `refrain.data.vanilla` mount these as their lowest-priority pack.
    [[nodiscard]] virtual auto image_bytes() const noexcept
        -> std::span<const std::byte> {
        return {};
    }
};

/// Resolve the package source for one candidate. Returning an error aborts the
/// load with that error (annotated with the mod id).
using package_resolver =
    std::function<result<std::unique_ptr<package_source>>(const candidate &)>;

/// Upper bound for a single package member read (manifests and content files).
inline constexpr std::size_t package_max_member_bytes{64ULL * 1024ULL *
                                                      1024ULL};

/// Builtin RFSI image source. `image` must outlive the source. When `mod_id`
/// is non-empty, reads are package-relative and resolve to
/// `mods/<mod_id>/<relative>` inside the image; an empty id addresses the image
/// root (used by tests/tools that read the image directly).
[[nodiscard]] auto make_builtin_package_source(std::span<const std::byte> image,
                                               std::string_view mod_id = {})
    -> result<std::unique_ptr<package_source>>;

/// Directory source rooted at the directory that contains `manifest.toml`
/// (derived from `candidate.path` by `make_package_resolver`).
[[nodiscard]] auto make_directory_package_source(std::string root)
    -> result<std::unique_ptr<package_source>>;

/// Standard resolver used by the server/tests: `package_kind::builtin`
/// candidates read from `builtin_image`; `package_kind::directory` candidates
/// read from the directory containing their scanned `manifest.toml`.
[[nodiscard]] auto
make_package_resolver(std::span<const std::byte> builtin_image)
    -> result<package_resolver>;

} // namespace refrain::mod
