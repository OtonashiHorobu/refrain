// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// mods.lock parsing/verification/formatting (split from discovery.cpp).

#include <refrain/mod/discovery.hpp>

#include "discovery_internal.hpp"

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/data/checksum.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::mod {

namespace {

namespace config = core::config;
namespace data = refrain::data;

using detail::annotate;
using detail::comment_text;
using detail::fail;
using detail::lower_id;
using detail::toml_string;

[[nodiscard]] auto parse_error_context(std::string_view context,
                                       std::string_view message) -> error {
    return make_error(error_code::parse_error,
                      fmt::format("read_mod_lock: {}: {}", context, message));
}

/// Read a required string field at a literal key path, mapping every failure
/// to a structural `parse_error`.
[[nodiscard]] auto require_string(const config::document &doc,
                                  std::span<const std::string_view> path,
                                  std::string_view context)
    -> result<std::string> {
    auto value{doc.get_string_at(path)};
    if (!value) {
        return std::unexpected{
            parse_error_context(context, value.error().message)};
    }
    return value;
}

[[nodiscard]] auto parse_abi(std::string_view text, std::string_view context)
    -> result<std::uint32_t> {
    std::uint32_t abi{0};
    const auto *begin{text.data()};
    const auto *end{text.data() + text.size()};
    const auto parsed{std::from_chars(begin, end, abi)};
    if (parsed.ec != std::errc{} || parsed.ptr != end || abi == 0U) {
        return std::unexpected{
            parse_error_context(context, "abi must be a positive integer")};
    }
    return abi;
}

[[nodiscard]] auto ends_with(std::string_view text, std::string_view suffix)
    -> bool {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

/// The stored manifest path must have the scanned layout for that mod id.
[[nodiscard]] auto validate_lock_path(const locked_mod &mod) -> status {
    const auto expected{fmt::format("mods/{}/manifest.toml", mod.id)};
    if (mod.path != expected && !ends_with(mod.path, "/" + expected)) {
        return std::unexpected{parse_error_context(
            fmt::format("mod '{}'", mod.id),
            fmt::format("stored path '{}' does not match '<root>/mods/<id>/"
                        "manifest.toml'",
                        mod.path))};
    }
    return status_ok();
}

/// Reject keys the lock writer never emits.
///
/// A `mods.lock` is untrusted input (it may be hand-edited or shipped by a
/// third party). Silently ignoring an unknown key would let a tampered lock
/// carry data that no verifier ever looked at, so every nested table has an
/// explicit allowlist of the keys `mod_lock::to_toml()` writes.
[[nodiscard]] auto reject_unknown_keys(
    const config::document &doc, std::span<const std::string_view> table_path,
    std::span<const std::string_view> allowed, std::string_view path,
    std::string_view table_label) -> status {
    const auto keys{doc.keys_at(table_path)};
    if (!keys) {
        return std::unexpected{parse_error_context(
            path, fmt::format("{}: {}", table_label, keys.error().message))};
    }
    for (const auto &key : *keys) {
        const auto known{std::any_of(
            allowed.begin(), allowed.end(),
            [&key](std::string_view candidate) { return candidate == key; })};
        if (!known) {
            return std::unexpected{parse_error_context(
                path, fmt::format("unknown key '{}' in {}", key, table_label))};
        }
    }
    return status_ok();
}

[[nodiscard]] auto read_mod_lock_impl(std::string_view path)
    -> result<mod_lock> {
    auto doc{config::document::parse_file(path)};
    if (!doc) {
        return std::unexpected{annotate(std::move(doc.error()), path)};
    }
    constexpr std::array<std::string_view, 2> allowed_root{"lock", "mods"};
    for (const auto &key : doc->keys("")) {
        const auto known{std::any_of(
            allowed_root.begin(), allowed_root.end(),
            [&key](std::string_view candidate) { return candidate == key; })};
        if (!known) {
            return std::unexpected{parse_error_context(
                path, fmt::format("unknown top-level table '{}'", key))};
        }
    }
    constexpr std::array<std::string_view, 1> lock_path{"lock"};
    if (!doc->contains_at(lock_path) ||
        doc->kind_at(lock_path) != config::value_kind::table) {
        return std::unexpected{
            parse_error_context(path, "missing or malformed [lock] table")};
    }
    constexpr std::array<std::string_view, 4> allowed_lock_keys{
        "format_version", "gamepack_id", "gamepack_version", "fingerprint"};
    if (auto keys{reject_unknown_keys(*doc, lock_path, allowed_lock_keys, path,
                                      "[lock]")};
        !keys) {
        return std::unexpected{std::move(keys.error())};
    }
    constexpr std::array<std::string_view, 2> version_path{"lock",
                                                           "format_version"};
    if (doc->kind_at(version_path) != config::value_kind::integer) {
        return std::unexpected{parse_error_context(
            path, "lock.format_version must be an integer")};
    }
    const auto format_version{doc->get_int64("lock.format_version", -1)};
    if (format_version != static_cast<std::int64_t>(mod_lock_format_version)) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("read_mod_lock: {}: unsupported format_version {} "
                        "(expected {})",
                        path, format_version, mod_lock_format_version))};
    }
    mod_lock lock{};
    lock.format_version = mod_lock_format_version;
    auto gamepack_id{require_string(
        *doc, std::array<std::string_view, 2>{"lock", "gamepack_id"}, path)};
    if (!gamepack_id) {
        return std::unexpected{std::move(gamepack_id.error())};
    }
    if (!mod::is_valid_mod_id(*gamepack_id)) {
        return std::unexpected{parse_error_context(
            path, "lock.gamepack_id is not a valid mod id")};
    }
    lock.gamepack_id = std::move(*gamepack_id);
    auto gamepack_version{require_string(
        *doc, std::array<std::string_view, 2>{"lock", "gamepack_version"},
        path)};
    if (!gamepack_version) {
        return std::unexpected{std::move(gamepack_version.error())};
    }
    if (!core::semver::parse(*gamepack_version).has_value()) {
        return std::unexpected{parse_error_context(
            path, "lock.gamepack_version is not valid SemVer")};
    }
    lock.gamepack_version = std::move(*gamepack_version);
    auto fingerprint{require_string(
        *doc, std::array<std::string_view, 2>{"lock", "fingerprint"}, path)};
    if (!fingerprint) {
        return std::unexpected{std::move(fingerprint.error())};
    }
    const auto stored_digest{data::parse_digest(*fingerprint)};
    if (!stored_digest) {
        return std::unexpected{parse_error_context(
            path, fmt::format("lock.fingerprint: {}",
                              stored_digest.error().message))};
    }

    constexpr std::array<std::string_view, 1> mods_path{"mods"};
    if (doc->contains_at(mods_path)) {
        if (doc->kind_at(mods_path) != config::value_kind::table) {
            return std::unexpected{
                parse_error_context(path, "[mods] must be a table")};
        }
        for (const auto &id : doc->keys("mods")) {
            if (!mod::is_valid_mod_id(id)) {
                return std::unexpected{parse_error_context(
                    path,
                    fmt::format("[mods] contains an invalid mod id '{}'", id))};
            }
            const std::array<std::string_view, 2> table_path{"mods", id};
            if (doc->kind_at(table_path) != config::value_kind::table) {
                return std::unexpected{parse_error_context(
                    path, fmt::format("[mods.\"{}\"] must be a table", id))};
            }
            constexpr std::array<std::string_view, 5> allowed_mod_keys{
                "version", "abi", "origin", "path", "manifest_hash"};
            if (auto keys{reject_unknown_keys(
                    *doc, table_path, allowed_mod_keys, path,
                    fmt::format("[mods.\"{}\"]", id))};
                !keys) {
                return std::unexpected{std::move(keys.error())};
            }
            locked_mod mod{};
            mod.id = id;
            auto version{require_string(
                *doc, std::array<std::string_view, 3>{"mods", id, "version"},
                path)};
            if (!version) {
                return std::unexpected{std::move(version.error())};
            }
            if (!core::semver::parse(*version).has_value()) {
                return std::unexpected{parse_error_context(
                    path, fmt::format("mod '{}' version '{}' is not valid "
                                      "SemVer",
                                      id, *version))};
            }
            mod.version = std::move(*version);
            auto abi{require_string(
                *doc, std::array<std::string_view, 3>{"mods", id, "abi"},
                path)};
            if (!abi) {
                return std::unexpected{std::move(abi.error())};
            }
            auto parsed_abi{parse_abi(*abi, fmt::format("mod '{}'", id))};
            if (!parsed_abi) {
                return std::unexpected{std::move(parsed_abi.error())};
            }
            mod.abi = *parsed_abi;
            auto origin_text{require_string(
                *doc, std::array<std::string_view, 3>{"mods", id, "origin"},
                path)};
            if (!origin_text) {
                return std::unexpected{std::move(origin_text.error())};
            }
            const auto origin{parse_mod_origin(*origin_text)};
            if (!origin) {
                return std::unexpected{parse_error_context(
                    path, fmt::format("mod '{}' has unknown origin '{}'", id,
                                      *origin_text))};
            }
            mod.origin = *origin;
            auto manifest_path{require_string(
                *doc, std::array<std::string_view, 3>{"mods", id, "path"},
                path)};
            if (!manifest_path) {
                return std::unexpected{std::move(manifest_path.error())};
            }
            if (manifest_path->empty()) {
                return std::unexpected{parse_error_context(
                    path, fmt::format("mod '{}' has an empty path", id))};
            }
            mod.path = std::move(*manifest_path);
            auto manifest_hash{require_string(
                *doc,
                std::array<std::string_view, 3>{"mods", id, "manifest_hash"},
                path)};
            if (!manifest_hash) {
                return std::unexpected{std::move(manifest_hash.error())};
            }
            const auto parsed_hash{data::parse_digest(*manifest_hash)};
            if (!parsed_hash) {
                return std::unexpected{parse_error_context(
                    path, fmt::format("mod '{}' manifest_hash: {}", id,
                                      parsed_hash.error().message))};
            }
            mod.manifest_hash = *parsed_hash;
            if (auto checked{validate_lock_path(mod)}; !checked) {
                return std::unexpected{std::move(checked.error())};
            }
            lock.mods.push_back(std::move(mod));
        }
    }
    std::sort(lock.mods.begin(), lock.mods.end(),
              [](const locked_mod &lhs, const locked_mod &rhs) {
                  return lhs.id < rhs.id;
              });
    for (std::size_t index{1}; index < lock.mods.size(); ++index) {
        const auto previous{lower_id(lock.mods[index - 1U].id)};
        const auto current{lower_id(lock.mods[index].id)};
        if (previous == current) {
            return std::unexpected{parse_error_context(
                path, fmt::format("duplicate mod id '{}' in [mods]",
                                  lock.mods[index].id))};
        }
    }

    const auto computed{lock.fingerprint()};
    if (computed != data::to_hex(*stored_digest)) {
        return std::unexpected{parse_error_context(
            path, "fingerprint mismatch (the lock was edited or belongs to a "
                  "different mod composition)")};
    }
    return lock;
}

// ---------------------------------------------------------------------------
// verify_mod_lock()
// ---------------------------------------------------------------------------

[[nodiscard]] auto verify_impl(const mod_lock &lock,
                               std::span<const discovered_mod> discovered)
    -> status {
    if (lock.format_version != mod_lock_format_version) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("verify_mod_lock: unsupported lock format_version {} "
                        "(expected {})",
                        lock.format_version, mod_lock_format_version))};
    }
    std::map<std::string, const discovered_mod *> by_id;
    std::map<std::string, std::string> by_lower_id;
    for (const auto &mod : discovered) {
        if (by_id.find(mod.id) != by_id.end()) {
            return std::unexpected{make_error(
                error_code::already_exists,
                fmt::format("verify_mod_lock: duplicate discovered id '{}'",
                            mod.id))};
        }
        const auto lower{lower_id(mod.id)};
        const auto clash{by_lower_id.find(lower)};
        if (clash != by_lower_id.end()) {
            return std::unexpected{make_error(
                error_code::already_exists,
                fmt::format("verify_mod_lock: ids '{}' and '{}' only differ "
                            "by case",
                            clash->second, mod.id))};
        }
        by_id.emplace(mod.id, &mod);
        by_lower_id.emplace(lower, mod.id);
    }
    for (const auto &locked : lock.mods) {
        const auto found{by_id.find(locked.id)};
        if (found == by_id.end()) {
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("verify_mod_lock: mod '{}@{}' from the lock was "
                            "not discovered",
                            locked.id, locked.version))};
        }
        const auto *mod{found->second};
        const auto locked_version{core::semver::parse(locked.version)};
        const auto discovered_version{core::semver::parse(mod->version)};
        if (!locked_version || !discovered_version) {
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("verify_mod_lock: mod '{}' has an unparsable "
                            "version (lock '{}', discovered '{}')",
                            locked.id, locked.version, mod->version))};
        }
        if (!(*locked_version == *discovered_version)) {
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("verify_mod_lock: mod '{}' version drift: lock "
                            "says '{}', discovered '{}'",
                            locked.id, locked.version, mod->version))};
        }
        if (mod->abi != locked.abi) {
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("verify_mod_lock: mod '{}' abi drift: lock says "
                            "{}, discovered {}",
                            locked.id, locked.abi, mod->abi))};
        }
        if (mod->origin != locked.origin) {
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("verify_mod_lock: mod '{}' origin drift: lock "
                            "says '{}', discovered '{}'",
                            locked.id, to_string(locked.origin),
                            to_string(mod->origin)))};
        }
        if (mod->manifest_hash != locked.manifest_hash) {
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("verify_mod_lock: mod '{}' manifest hash drift "
                            "(content changed without a version bump)",
                            locked.id))};
        }
    }
    return status_ok();
}

} // namespace

auto mod_lock::canonical_text() const -> std::string {
    auto ordered{mods};
    std::sort(ordered.begin(), ordered.end(),
              [](const locked_mod &lhs, const locked_mod &rhs) {
                  return lhs.id < rhs.id;
              });
    std::string text;
    text += "refrain-mods-lock\n";
    text += "format_version=" + std::to_string(format_version) + "\n";
    text += "gamepack=" + gamepack_id + "@" + gamepack_version + "\n";
    for (const auto &mod : ordered) {
        text += "mod=" + mod.id + "\t" + mod.version + "\t" +
                std::to_string(mod.abi) + "\t" +
                std::string{to_string(mod.origin)} + "\t" +
                data::to_hex(mod.manifest_hash) + "\n";
    }
    return text;
}

auto mod_lock::fingerprint() const -> std::string {
    const auto text{canonical_text()};
    return data::to_hex(data::blake3(
        std::as_bytes(std::span<const char>{text.data(), text.size()})));
}

auto mod_lock::to_toml() const -> std::string {
    auto ordered{mods};
    std::sort(ordered.begin(), ordered.end(),
              [](const locked_mod &lhs, const locked_mod &rhs) {
                  return lhs.id < rhs.id;
              });
    std::string out;
    out +=
        "# mods.lock: deterministic composition lock; do not edit by hand.\n";
    auto warnings_sorted{warnings};
    std::sort(warnings_sorted.begin(), warnings_sorted.end());
    warnings_sorted.erase(
        std::unique(warnings_sorted.begin(), warnings_sorted.end()),
        warnings_sorted.end());
    for (const auto &warning : warnings_sorted) {
        out += "# warning: " + comment_text(warning) + "\n";
    }
    out += "[lock]\n";
    out += "format_version = " + std::to_string(format_version) + "\n";
    out += "gamepack_id = " + toml_string(gamepack_id) + "\n";
    out += "gamepack_version = " + toml_string(gamepack_version) + "\n";
    out += "fingerprint = " + toml_string(fingerprint()) + "\n";
    for (const auto &mod : ordered) {
        out += "\n[mods." + toml_string(mod.id) + "]\n";
        out += "version = " + toml_string(mod.version) + "\n";
        out += "abi = " + toml_string(std::to_string(mod.abi)) + "\n";
        out += "origin = " + toml_string(to_string(mod.origin)) + "\n";
        out += "path = " + toml_string(mod.path) + "\n";
        out +=
            "manifest_hash = " + toml_string(data::to_hex(mod.manifest_hash)) +
            "\n";
    }
    return out;
}

auto write_mod_lock(std::string_view path, const mod_lock &lock) -> status {
    return core::io::atomic_write_text(path, lock.to_toml());
}

auto read_mod_lock(std::string_view path) -> result<mod_lock> {
    return read_mod_lock_impl(path);
}

auto verify_mod_lock(const mod_lock &lock,
                     std::span<const discovered_mod> discovered) -> status {
    return verify_impl(lock, discovered);
}

auto format_mod_lock(const mod_lock &lock) -> std::string {
    std::string out{
        fmt::format("mods.lock: gamepack {}@{}, {} mod(s), fingerprint {}\n",
                    lock.gamepack_id, lock.gamepack_version, lock.mods.size(),
                    lock.fingerprint())};
    auto warnings_sorted{lock.warnings};
    std::sort(warnings_sorted.begin(), warnings_sorted.end());
    for (const auto &warning : warnings_sorted) {
        out += fmt::format("  warning: {}\n", warning);
    }
    for (const auto &mod : lock.mods) {
        out +=
            fmt::format("  {}@{} (abi {}, origin={}, hash={})\n    path: {}\n",
                        mod.id, mod.version, mod.abi, to_string(mod.origin),
                        data::to_hex(mod.manifest_hash), mod.path);
    }
    return out;
}

} // namespace refrain::mod
