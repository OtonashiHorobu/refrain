// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/discovery.hpp>

#include "discovery_internal.hpp"

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/path.hpp>
#include <refrain/kernel/manifest.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::mod {
namespace {

namespace config = core::config;
namespace kernel = refrain::kernel;
namespace data = refrain::data;

using data::digest32;

using detail::annotate;
using detail::comment_text;
using detail::fail;
using detail::lower_id;
using detail::toml_escape;
using detail::toml_string;

[[nodiscard]] auto is_separator(char character) noexcept -> bool {
    return character == '/' || character == '\\';
}

/// Normalize a host path for storage/diagnostics: '\\' becomes '/', repeated
/// separators collapse and trailing separators are dropped (a lone "/" stays).
[[nodiscard]] auto normalize_separators(std::string_view path) -> std::string {
    std::string normalized;
    normalized.reserve(path.size());
    bool last_was_separator{false};
    for (const char character : path) {
        if (is_separator(character)) {
            if (normalized.empty() || !last_was_separator) {
                normalized.push_back('/');
            }
            last_was_separator = true;
        } else {
            normalized.push_back(character);
            last_was_separator = false;
        }
    }
    while (normalized.size() > 1U && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

[[nodiscard]] auto join_host_path(std::string_view base, std::string_view child)
    -> std::string {
    std::string joined{normalize_separators(base)};
    if (joined.empty()) {
        return std::string{child};
    }
    if (joined.back() != '/') {
        joined.push_back('/');
    }
    joined.append(child);
    return joined;
}

/// Deterministic ASCII lowercasing (mod ids are ASCII; this is not locale
/// dependent). Used for the portable case-insensitive duplicate rules.
[[nodiscard]] auto text_of(std::span<const std::byte> bytes)
    -> std::string_view {
    return std::string_view{
        reinterpret_cast<const char *>(bytes.data()), // NOLINT
        bytes.size()};
}

// ---------------------------------------------------------------------------
// scan()
// ---------------------------------------------------------------------------

[[nodiscard]] auto read_manifest(std::string_view manifest_path)
    -> result<discovered_mod> {
    // Manifests are untrusted input: cap the read at 1 MiB and never follow a
    // symlink (a link could point outside the scanned instance root).
    constexpr auto manifest_limit{1ULL * 1024ULL * 1024ULL};
    auto bytes{core::io::read_file(
        manifest_path, {.max_bytes = manifest_limit, .allow_symlink = false})};
    if (!bytes) {
        return std::unexpected{
            annotate(std::move(bytes.error()), manifest_path)};
    }
    auto doc{config::document::parse_text(text_of(*bytes), manifest_path)};
    if (!doc) {
        return std::unexpected{annotate(std::move(doc.error()), manifest_path)};
    }
    auto manifest{kernel::parse_mod_manifest(*doc)};
    if (!manifest) {
        // The kernel parser distinguishes manifest-schema violations
        // (invalid_argument/already_exists); a broken on-disk manifest is a
        // parse failure from the scanner's point of view, so normalize the
        // code and keep the detailed message.
        auto failure{std::move(manifest.error())};
        failure.code = error_code::parse_error;
        return std::unexpected{annotate(std::move(failure), manifest_path)};
    }
    discovered_mod mod{};
    mod.id = manifest->common.id;
    mod.version = manifest->common.version;
    mod.abi = manifest->common.abi;
    mod.side = manifest->common.side;
    mod.meta = manifest->common.meta;
    mod.path = std::string{manifest_path};
    mod.manifest_hash = data::blake3(*bytes);
    mod.required = std::move(manifest->required);
    mod.optional = std::move(manifest->optional);
    mod.provides = std::move(manifest->provides);
    mod.entry = std::move(manifest->entry);
    return mod;
}

[[nodiscard]] auto scan_mods_directory(const discovery_root &root,
                                       std::string_view mods_dir,
                                       const discover_options &options,
                                       std::vector<discovered_mod> &candidates)
    -> status {
    auto names{core::io::list_directory(mods_dir)};
    if (!names) {
        return std::unexpected{std::move(names.error())};
    }
    for (const auto &dir_name : *names) {
        const auto mod_dir{join_host_path(mods_dir, dir_name)};
        if (!core::io::is_directory(mod_dir)) {
            if (options.strict_entries) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("auto_scan: '{}' under '{}' is not a mod "
                                     "directory (loose files are not scanned)",
                                     mod_dir, mods_dir))};
            }
            continue;
        }
        const auto manifest_path{
            join_host_path(mod_dir, kernel::mod_manifest_filename)};
        if (!core::io::exists(manifest_path)) {
            if (options.strict_entries ||
                options.require_manifest_in_directories) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("auto_scan: mod directory '{}' has no {}",
                                     mod_dir, kernel::mod_manifest_filename))};
            }
            continue;
        }
        auto mod{read_manifest(manifest_path)};
        if (!mod) {
            return std::unexpected{std::move(mod.error())};
        }
        // The directory name is the on-disk identity: a mismatch is a broken
        // install, not a silently renamed mod.
        if (mod->id != dir_name) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("auto_scan: '{}' declares id '{}' but its "
                                 "directory is named '{}'",
                                 manifest_path, mod->id, dir_name))};
        }
        mod->origin = root.origin;
        candidates.push_back(std::move(*mod));
    }
    return status_ok();
}

[[nodiscard]] auto merge_candidates(std::vector<discovered_mod> candidates)
    -> result<std::vector<discovered_mod>>;

/// Shared scan core. With `roots_are_mods_dirs` each `root.path` is itself the
/// `mods/` directory (the `--mods <dir>` form); otherwise `root.path` is an
/// instance root containing `mods/`.
[[nodiscard]] auto scan_impl(std::span<const discovery_root> roots,
                             const discover_options &options,
                             bool roots_are_mods_dirs)
    -> result<std::vector<discovered_mod>> {
    if (roots.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "auto_scan: at least one discovery root is required")};
    }

    std::vector<discovered_mod> candidates;
    for (const auto &root : roots) {
        if (root.path.empty()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "auto_scan: discovery root has an empty path")};
        }
        const auto root_path{normalize_separators(root.path)};
        if (!core::io::is_directory(root_path)) {
            return std::unexpected{
                fail(error_code::io_error,
                     fmt::format("auto_scan: discovery root '{}' is not a "
                                 "directory",
                                 root_path))};
        }
        const auto mods_dir{roots_are_mods_dirs
                                ? root_path
                                : join_host_path(root_path, "mods")};
        if (!roots_are_mods_dirs && !core::io::exists(mods_dir)) {
            if (options.require_mods_dir) {
                return std::unexpected{
                    fail(error_code::io_error,
                         fmt::format("auto_scan: discovery root '{}' has no "
                                     "'mods' directory",
                                     root_path))};
            }
            continue;
        }
        if (!core::io::is_directory(mods_dir)) {
            return std::unexpected{fail(
                error_code::io_error,
                fmt::format("auto_scan: '{}' is not a directory", mods_dir))};
        }
        if (auto scanned{
                scan_mods_directory(root, mods_dir, options, candidates)};
            !scanned) {
            return std::unexpected{std::move(scanned.error())};
        }
    }

    return merge_candidates(std::move(candidates));
}

/// Merge raw candidates from every origin tier into one catalog: deterministic
/// order, same-origin duplicate detection (case-insensitive) and origin/shadow
/// resolution. Shared by `scan()` and `load_composition()` so the builtin image
/// and disk roots are merged by exactly the same policy.
[[nodiscard]] auto merge_candidates(std::vector<discovered_mod> candidates)
    -> result<std::vector<discovered_mod>> {
    // Deterministic candidate order before the duplicate/shadow analysis:
    // case-insensitive id, then origin tier (low -> high), then path.
    std::sort(candidates.begin(), candidates.end(),
              [](const discovered_mod &lhs, const discovered_mod &rhs) {
                  const auto lhs_key{lower_id(lhs.id)};
                  const auto rhs_key{lower_id(rhs.id)};
                  if (lhs_key != rhs_key) {
                      return lhs_key < rhs_key;
                  }
                  if (lhs.origin != rhs.origin) {
                      return origin_rank(lhs.origin) < origin_rank(rhs.origin);
                  }
                  return lhs.path < rhs.path;
              });

    std::vector<discovered_mod> winners;
    for (std::size_t begin{0}; begin < candidates.size();) {
        const auto key{lower_id(candidates[begin].id)};
        std::size_t end{begin + 1U};
        while (end < candidates.size() && lower_id(candidates[end].id) == key) {
            ++end;
        }

        // Rule 3: within one origin, multiple versions are allowed
        // and the highest SemVer wins with a warning; the same version twice is
        // an ambiguous install tree and fails closed.
        std::vector<discovered_mod> origin_winners;
        std::size_t sub_begin{begin};
        while (sub_begin < end) {
            const auto origin{candidates[sub_begin].origin};
            std::size_t sub_end{sub_begin + 1U};
            while (sub_end < end && candidates[sub_end].origin == origin) {
                ++sub_end;
            }
            for (std::size_t first{sub_begin}; first < sub_end; ++first) {
                for (std::size_t second{first + 1U}; second < sub_end;
                     ++second) {
                    if (candidates[first].version ==
                        candidates[second].version) {
                        return std::unexpected{fail(
                            error_code::already_exists,
                            fmt::format(
                                "auto_scan: mod id '{}' version '{}' is "
                                "installed twice within origin '{}': '{}' and "
                                "'{}'",
                                candidates[first].id, candidates[first].version,
                                to_string(origin), candidates[first].path,
                                candidates[second].path))};
                    }
                }
            }
            std::size_t best{sub_begin};
            for (std::size_t index{sub_begin + 1U}; index < sub_end; ++index) {
                const auto lhs{core::semver::parse(candidates[index].version)};
                const auto rhs{core::semver::parse(candidates[best].version)};
                if (lhs && rhs &&
                    core::compare(*lhs, *rhs) ==
                        std::strong_ordering::greater) {
                    best = index;
                }
            }
            discovered_mod chosen{candidates[best]};
            chosen.shadowed.clear();
            for (std::size_t index{sub_begin}; index < sub_end; ++index) {
                if (index == best) {
                    continue;
                }
                const auto &loser{candidates[index]};
                chosen.shadowed.push_back(shadowed_mod{
                    .origin = loser.origin,
                    .version = loser.version,
                    .path = loser.path,
                    .manifest_hash = loser.manifest_hash,
                });
            }
            origin_winners.push_back(std::move(chosen));
            sub_begin = sub_end;
        }

        // Highest origin wins; same-origin versions were reduced above.
        std::size_t winner{0};
        for (std::size_t index{1}; index < origin_winners.size(); ++index) {
            if (origin_rank(origin_winners[index].origin) >
                origin_rank(origin_winners[winner].origin)) {
                winner = index;
            }
        }
        discovered_mod selected{origin_winners[winner]};
        for (std::size_t index{0}; index < origin_winners.size(); ++index) {
            if (index == winner) {
                continue;
            }
            const auto &loser{origin_winners[index]};
            selected.shadowed.push_back(shadowed_mod{
                .origin = loser.origin,
                .version = loser.version,
                .path = loser.path,
                .manifest_hash = loser.manifest_hash,
            });
            selected.shadowed.insert(selected.shadowed.end(),
                                     loser.shadowed.begin(),
                                     loser.shadowed.end());
        }
        winners.push_back(std::move(selected));
        begin = end;
    }

    return winners;
}

// ---------------------------------------------------------------------------
// collect_package_files()
// ---------------------------------------------------------------------------

[[nodiscard]] auto contains_name(std::span<const std::string_view> names,
                                 std::string_view value) -> bool {
    return std::any_of(
        names.begin(), names.end(),
        [value](std::string_view entry) { return entry == value; });
}

[[nodiscard]] auto
collect_directory_files(const std::string &root, std::string_view relative,
                        const package_collect_options &options,
                        std::vector<package_file> &files) -> status {
    const auto directory{relative.empty() ? root
                                          : data::join_path(root, relative)};
    auto names{core::io::list_directory(directory)};
    if (!names) {
        return std::unexpected{annotate(
            std::move(names.error()),
            fmt::format("{}: {}", options.error_prefix,
                        relative.empty() ? root : std::string{relative}))};
    }
    const bool top_level{relative.empty()};
    for (const auto &name : *names) {
        if (options.skip_dotfiles && !name.empty() && name.front() == '.') {
            continue;
        }
        // `manifest.toml` is handled by the caller (it is either included
        // explicitly or rebuilt by the `.rfmod` writer), never by the walker.
        if (top_level && name == kernel::mod_manifest_filename) {
            continue;
        }
        const auto child_relative{
            top_level ? name : fmt::format("{}/{}", relative, name)};
        const auto full{data::join_path(root, child_relative)};
        if (core::io::is_symlink(full)) {
            if (options.reject_symlinks) {
                return std::unexpected{
                    fail(error_code::permission_denied,
                         fmt::format("{}: symlinks are not allowed: {}",
                                     options.error_prefix, child_relative))};
            }
            continue;
        }
        if (core::io::is_directory(full)) {
            if (top_level) {
                if (contains_name(options.exclude_roots, name)) {
                    continue;
                }
                if (!options.roots.empty() &&
                    !contains_name(options.roots, name)) {
                    continue;
                }
            }
            if (auto walked{collect_directory_files(root, child_relative,
                                                    options, files)};
                !walked) {
                return walked;
            }
            continue;
        }
        if (!core::io::is_regular_file(full)) {
            if (options.reject_special_files) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("{}: non-regular file in mod tree: {}",
                                     options.error_prefix, full))};
            }
            continue;
        }
        // With an explicit root list only the requested content directories
        // and the declared metadata files are shipped.
        if (top_level && !options.roots.empty() &&
            !contains_name(options.extra_root_files, name)) {
            continue;
        }
        if (!data::is_safe_relative_path(child_relative)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("{}: unsafe path '{}'", options.error_prefix,
                                 child_relative))};
        }
        if (files.size() >= options.max_files) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("{}: too many files (more than {})",
                                 options.error_prefix, options.max_files))};
        }
        auto bytes{
            core::io::read_file(full, {.max_bytes = options.max_file_bytes,
                                       .allow_symlink = false})};
        if (!bytes) {
            auto failure{std::move(bytes.error())};
            if (failure.code == error_code::invalid_argument) {
                // read_file() reports a byte-limit violation in its own
                // wording; keep the packer's stable message instead.
                return std::unexpected{fail(
                    error_code::invalid_argument,
                    fmt::format("{}: file '{}' exceeds the configured size "
                                "limit",
                                options.error_prefix, child_relative))};
            }
            return std::unexpected{annotate(
                std::move(failure),
                fmt::format("{}: {}", options.error_prefix, child_relative))};
        }
        files.push_back(
            package_file{.path = child_relative, .data = std::move(*bytes)});
    }
    return status_ok();
}

} // namespace

auto to_string(package_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case package_kind::builtin:
        return "builtin";
    case package_kind::directory:
        return "directory";
    case package_kind::rfmod:
        return "rfmod";
    }
    return "directory";
}

auto scan(std::span<const discovery_root> roots,
          const discover_options &options)
    -> result<std::vector<discovered_mod>> {
    return scan_impl(roots, options, false);
}

auto scan_mods_tree(std::string_view mods_dir, mod_origin origin,
                    const discover_options &options)
    -> result<std::vector<discovered_mod>> {
    const std::array<discovery_root, 1> roots{
        {discovery_root{.path = std::string{mods_dir}, .origin = origin}}};
    return scan_impl(roots, options, true);
}

auto collect_package_files(std::string_view mod_directory,
                           const package_collect_options &options)
    -> result<std::vector<package_file>> {
    if (mod_directory.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("{}: empty mod directory", options.error_prefix))};
    }
    const std::string root{normalize_separators(mod_directory)};
    if (!core::io::is_directory(root)) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    fmt::format("{}: '{}' is not a directory",
                                                options.error_prefix, root))};
    }
    std::vector<package_file> files;
    if (options.include_manifest) {
        const auto manifest_path{
            data::join_path(root, std::string{kernel::mod_manifest_filename})};
        if (!core::io::is_regular_file(manifest_path)) {
            return std::unexpected{
                fail(error_code::not_found,
                     fmt::format("{}: '{}' has no {}", options.error_prefix,
                                 root, kernel::mod_manifest_filename))};
        }
        auto bytes{core::io::read_file(
            manifest_path,
            {.max_bytes = options.max_file_bytes, .allow_symlink = false})};
        if (!bytes) {
            return std::unexpected{
                annotate(std::move(bytes.error()),
                         fmt::format("{}: {}", options.error_prefix,
                                     kernel::mod_manifest_filename))};
        }
        files.push_back(
            package_file{.path = std::string{kernel::mod_manifest_filename},
                         .data = std::move(*bytes)});
    }
    if (auto walked{collect_directory_files(root, {}, options, files)};
        !walked) {
        return std::unexpected{std::move(walked.error())};
    }
    if (files.size() > options.max_files) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("{}: too many files ({} > {})", options.error_prefix,
                        files.size(), options.max_files))};
    }
    std::sort(files.begin(), files.end(),
              [](const package_file &lhs, const package_file &rhs) {
                  return lhs.path < rhs.path;
              });
    return files;
}

auto format_candidate(const candidate &mod) -> std::string {
    auto out{fmt::format("mod {}@{} (side={}, abi={}{})\n", mod.id, mod.version,
                         kernel::to_string(mod.side), mod.abi,
                         mod.is_external() ? ", external" : ", internal")};
    if (!mod.meta.display_name.empty()) {
        out += fmt::format("display name: {}\n", mod.meta.display_name);
    }
    if (!mod.provides.empty()) {
        out += "provides:";
        for (const auto &service : mod.provides) {
            out += fmt::format(" {}", service);
        }
        out += '\n';
    }
    if (!mod.required.empty()) {
        out += "requires:";
        for (const auto &requirement : mod.required) {
            out += fmt::format(" {}", requirement.text());
        }
        out += '\n';
    }
    if (!mod.optional.empty()) {
        out += "optional:";
        for (const auto &requirement : mod.optional) {
            out += fmt::format(" {}", requirement.text());
        }
        out += '\n';
    }
    if (mod.entry) {
        out += fmt::format("entry: {}\n",
                           mod.entry->native.empty()
                               ? fmt::format("wasm {}", mod.entry->wasm)
                               : fmt::format("native {}", mod.entry->native));
    }
    return out;
}

auto resolve(const kernel::gamepack_spec &spec,
             std::span<const discovered_mod> discovered,
             const discover_options &options,
             std::span<const std::string_view> required_services)
    -> result<composition> {
    return detail::resolve_catalog(spec, discovered, options, required_services,
                                   false);
}

auto preview_composition(const kernel::gamepack_spec &spec,
                         std::span<const std::string_view> required_services)
    -> result<composition> {
    if (spec.mods.empty()) {
        if (spec.auto_scan) {
            return std::unexpected{fail(
                error_code::unsupported,
                "gamepack: auto-discovery requires a resolved mod catalog; "
                "list the mods explicitly in [mods].list or resolve the "
                "composition against a scanned catalog")};
        }
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "gamepack: cannot resolve an empty mod list")};
    }
    // Manifest-level preview: the declared `[mods].list` is the catalog. The
    // candidate keeps the gamepack-declared dependency edges (`[mods."<id>"]`
    // requires) so the plan text is identical to the historical
    // manifest-level plan output.
    std::vector<discovered_mod> catalog;
    catalog.reserve(spec.mods.size());
    for (const auto &entry : spec.mods) {
        catalog.push_back(discovered_mod{.id = entry.ref.id,
                                         .version = entry.ref.version,
                                         .abi = spec.abi,
                                         .origin = mod_origin::builtin,
                                         .side = kernel::manifest_side::both,
                                         .path = "<gamepack>",
                                         .required = entry.dependencies,
                                         .kind = package_kind::builtin});
    }
    auto preview{
        detail::resolve_catalog(spec, catalog, {}, required_services, true)};
    if (!preview) {
        // Keep the historical `gamepack:` prefix for manifest-level errors.
        auto failure{std::move(preview.error())};
        if (failure.message.starts_with("resolve: ")) {
            failure.message = "gamepack: " + failure.message.substr(10);
        }
        return std::unexpected{std::move(failure)};
    }
    return preview;
}

auto scan_builtin_image(std::span<const std::byte> image)
    -> result<std::vector<candidate>> {
    auto opened{data::builtin_image::open(image)};
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    std::vector<candidate> mods;
    for (const auto &entry : opened->entries()) {
        // Only `mods/<id>/manifest.toml` is a mod identity: other files are
        // payload (data/assets/scripts) and carry no manifest.
        const auto &path{entry.path};
        constexpr std::string_view prefix{"mods/"};
        constexpr std::string_view suffix{"/manifest.toml"};
        if (!path.starts_with(prefix) || !path.ends_with(suffix) ||
            path.size() <= prefix.size() + suffix.size()) {
            continue;
        }
        const auto id{path.substr(prefix.size(),
                                  path.size() - prefix.size() - suffix.size())};
        if (id.find('/') != std::string_view::npos) {
            continue; // nested manifest: not a mod root
        }
        auto bytes{opened->read(path)};
        if (!bytes) {
            return std::unexpected{std::move(bytes.error())};
        }
        auto doc{
            config::document::parse_text(text_of(*bytes), std::string{path})};
        if (!doc) {
            return std::unexpected{std::move(doc.error())};
        }
        auto manifest{kernel::parse_mod_manifest(*doc)};
        if (!manifest) {
            auto failure{std::move(manifest.error())};
            failure.code = error_code::parse_error;
            return std::unexpected{std::move(failure)};
        }
        if (manifest->common.id != id) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("builtin image: '{}' declares id '{}'", path,
                                 manifest->common.id))};
        }
        mods.push_back(candidate{.id = manifest->common.id,
                                 .version = manifest->common.version,
                                 .abi = manifest->common.abi,
                                 .origin = mod_origin::builtin,
                                 .side = manifest->common.side,
                                 .meta = manifest->common.meta,
                                 .provides = std::move(manifest->provides),
                                 .path = fmt::format("<builtin>/{}", path),
                                 .manifest_hash = data::blake3(*bytes),
                                 .required = std::move(manifest->required),
                                 .optional = std::move(manifest->optional),
                                 .kind = package_kind::builtin,
                                 .entry = std::move(manifest->entry)});
    }
    std::sort(mods.begin(), mods.end(),
              [](const candidate &lhs, const candidate &rhs) {
                  return lhs.id < rhs.id;
              });
    return mods;
}

auto resolve_composition(const kernel::gamepack_spec &parsed,
                         std::span<const discovery_root> roots,
                         const composition_load_options &options)
    -> result<composition> {
    auto spec{parsed};
    if (options.auto_scan.has_value()) {
        spec.auto_scan = *options.auto_scan;
    }

    std::vector<discovered_mod> catalog;
    if (!options.builtin_image.empty()) {
        auto builtin{scan_builtin_image(options.builtin_image)};
        if (!builtin) {
            return std::unexpected{std::move(builtin.error())};
        }
        catalog = std::move(*builtin);
    }
    for (const auto &root : roots) {
        if (root.path.empty()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "auto_scan: discovery root has an empty path")};
        }
        const auto root_path{normalize_separators(root.path)};
        if (!core::io::is_directory(root_path)) {
            return std::unexpected{
                fail(error_code::io_error,
                     fmt::format("auto_scan: discovery root '{}' is not a "
                                 "directory",
                                 root_path))};
        }
        const auto mods_dir{join_host_path(root_path, "mods")};
        if (!core::io::exists(mods_dir)) {
            if (options.discovery.require_mods_dir) {
                return std::unexpected{
                    fail(error_code::io_error,
                         fmt::format("auto_scan: discovery root '{}' has no "
                                     "'mods' directory",
                                     root_path))};
            }
            continue;
        }
        if (!core::io::is_directory(mods_dir)) {
            return std::unexpected{fail(
                error_code::io_error,
                fmt::format("auto_scan: '{}' is not a directory", mods_dir))};
        }
        if (auto scanned{scan_mods_directory(root, mods_dir, options.discovery,
                                             catalog)};
            !scanned) {
            return std::unexpected{std::move(scanned.error())};
        }
    }

    auto merged{merge_candidates(std::move(catalog))};
    if (!merged) {
        return std::unexpected{std::move(merged.error())};
    }
    return detail::resolve_catalog(spec, *merged, options.discovery,
                                   options.required_services, false);
}

auto load_composition(std::string_view gamepack_path,
                      std::span<const discovery_root> roots,
                      const composition_load_options &options)
    -> result<composition> {
    auto spec{kernel::load_gamepack_spec(gamepack_path, options.host_side)};
    if (!spec) {
        return std::unexpected{std::move(spec.error())};
    }
    return resolve_composition(*spec, roots, options);
}

auto composition::provider(std::string_view service) const noexcept
    -> const kernel::resolved_provider * {
    const auto match{
        std::find_if(providers.begin(), providers.end(),
                     [service](const kernel::resolved_provider &entry) {
                         return entry.service == service;
                     })};
    return match == providers.end() ? nullptr : &*match;
}

auto composition::find(std::string_view id) const noexcept
    -> const candidate * {
    const auto match{
        std::find_if(load_order.begin(), load_order.end(),
                     [id](const candidate &mod) { return mod.id == id; })};
    return match == load_order.end() ? nullptr : &*match;
}

auto composition::format() const -> std::string {
    auto out{fmt::format("gamepack {}@{} (side={}, abi={}, auto_scan={})\n",
                         spec.id, spec.version, spec.side, spec.abi,
                         spec.auto_scan ? "true" : "false")};
    if (!spec.meta.display_name.empty()) {
        out += fmt::format("display name: {}\n", spec.meta.display_name);
    }

    out += fmt::format("mods: {} (load order)\n", load_order.size());
    std::size_t position{1};
    for (const auto &mod : load_order) {
        out += fmt::format("  {:>2}. {}@{}", position++, mod.id, mod.version);
        if (!mod.required.empty()) {
            auto joined{std::string{}};
            for (const auto &dependency : mod.required) {
                if (!joined.empty()) {
                    joined += ", ";
                }
                joined += dependency.text();
            }
            out += fmt::format(" requires [{}]", joined);
        }
        out += '\n';
    }

    out += fmt::format("providers: {}\n", providers.size());
    std::size_t width{0};
    for (const auto &entry : providers) {
        width = std::max(width, entry.service.size());
    }
    for (const auto &entry : providers) {
        switch (entry.mode) {
        case kernel::provider_mode::exclusive:
            out += fmt::format("  {:<{}} -> {}\n", entry.service, width,
                               entry.provider);
            for (const auto &skipped : entry.skipped) {
                out += fmt::format("  {:<{}}    skipped: {}\n", "", width,
                                   skipped);
            }
            break;
        case kernel::provider_mode::instanced:
            for (const auto &instance : entry.instances) {
                const auto marker{instance.name == entry.default_instance
                                      ? " (default)"
                                      : ""};
                out += fmt::format("  {:<{}} [instanced:{}]{} -> {}\n",
                                   entry.service, width, instance.name, marker,
                                   instance.provider);
            }
            break;
        case kernel::provider_mode::chain:
            out += fmt::format("  {:<{}} [chain] base -> {}\n", entry.service,
                               width, entry.provider);
            for (const auto &wrapper : entry.wrappers) {
                out += fmt::format("  {:<{}} [chain] wrap -> {}\n", "", width,
                                   wrapper);
            }
            break;
        }
    }

    if (!spec.mods_dir.empty()) {
        out += "mods_dir: ";
        for (std::size_t index{0}; index < spec.mods_dir.size(); ++index) {
            if (index != 0) {
                out += ", ";
            }
            out += spec.mods_dir[index];
        }
        out += '\n';
    }
    return out;
}

} // namespace refrain::mod
