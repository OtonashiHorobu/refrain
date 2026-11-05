// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/mod/discovery.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace fs = std::filesystem;

using refrain::data::blake3;
using refrain::data::to_hex;
using refrain::mod::discover_options;
using refrain::mod::discovered_mod;
using refrain::mod::discovery_root;
using refrain::mod::format_mod_lock;
using refrain::mod::locked_mod;
using refrain::mod::mod_lock;
using refrain::mod::mod_lock_format_version;
using refrain::mod::mod_origin;
using refrain::mod::origin_rank;
using refrain::mod::parse_mod_origin;
using refrain::mod::read_mod_lock;
using refrain::mod::resolve;
using refrain::mod::scan;
using refrain::mod::shadowed_mod;
using refrain::mod::verify_mod_lock;
using refrain::mod::write_mod_lock;

namespace core_io = refrain::core::io;

[[nodiscard]] auto make_temp_directory() -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_discovery_test_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++))};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

/// Removes the scratch directory when the test returns (including early exits
/// on failed preconditions).
struct temp_directory {
    fs::path path{};

    explicit temp_directory(fs::path location) : path{std::move(location)} {}
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

auto path_join(std::string_view base, std::string_view child) -> std::string {
    std::string joined{base};
    if (!joined.empty() && joined.back() != '/') {
        joined.push_back('/');
    }
    joined.append(child);
    return joined;
}

auto create_root(const temp_directory &scratch, std::string_view name)
    -> std::string {
    const auto root{path_join(scratch.path.string(), name)};
    (void)core_io::create_directories(root);
    return root;
}

/// Minimal valid mod manifest with an optional extra root-table fragment.
[[nodiscard]] auto manifest_text(std::string_view id, std::string_view version,
                                 std::string_view extra = {},
                                 std::uint32_t abi = 1) -> std::string {
    std::string text{"[mod]\nid = \""};
    text += id;
    text += "\"\nversion = \"";
    text += version;
    text += "\"\nabi = \"";
    text += std::to_string(abi);
    text += "\"\n";
    if (!extra.empty()) {
        text += extra;
        if (text.back() != '\n') {
            text.push_back('\n');
        }
    }
    return text;
}

auto write_manifest(const std::string &root, std::string_view id,
                    std::string_view version, std::string_view extra = {},
                    std::uint32_t abi = 1) -> std::string {
    const auto mod_dir{path_join(path_join(root, "mods"), id)};
    (void)core_io::create_directories(mod_dir);
    const auto manifest_path{path_join(mod_dir, "manifest.toml")};
    (void)core_io::write_text(manifest_path,
                              manifest_text(id, version, extra, abi));
    return manifest_path;
}

/// Write a manifest into a directory whose name intentionally differs from the
/// manifest id (or arbitrary malformed TOML when `text` is raw).
auto write_raw_manifest(const std::string &root, std::string_view dir_name,
                        std::string_view text) -> std::string {
    const auto mod_dir{path_join(path_join(root, "mods"), dir_name)};
    (void)core_io::create_directories(mod_dir);
    const auto manifest_path{path_join(mod_dir, "manifest.toml")};
    (void)core_io::write_text(manifest_path, text);
    return manifest_path;
}

[[nodiscard]] auto make_spec(bool auto_scan = true, std::uint32_t abi = 1)
    -> refrain::kernel::gamepack_spec {
    refrain::kernel::gamepack_spec spec{};
    spec.id = "test.gamepack";
    spec.version = "1.0.0";
    spec.side = "server";
    spec.abi = abi;
    spec.auto_scan = auto_scan;
    return spec;
}

void pin_mod(refrain::kernel::gamepack_spec &spec, std::string id,
             std::string version) {
    spec.mods.push_back(refrain::kernel::mod_entry{
        .ref = refrain::kernel::mod_ref{.id = std::move(id),
                                        .version = std::move(version)}});
}

[[nodiscard]] auto find_discovered(std::span<const discovered_mod> mods,
                                   std::string_view id)
    -> const discovered_mod * {
    const auto match{
        std::find_if(mods.begin(), mods.end(),
                     [id](const discovered_mod &mod) { return mod.id == id; })};
    return match == mods.end() ? nullptr : &*match;
}

[[nodiscard]] auto find_locked(const mod_lock &lock, std::string_view id)
    -> const locked_mod * {
    const auto match{
        std::find_if(lock.mods.begin(), lock.mods.end(),
                     [id](const locked_mod &mod) { return mod.id == id; })};
    return match == lock.mods.end() ? nullptr : &*match;
}

[[nodiscard]] auto scan_single(const std::string &root, mod_origin origin,
                               const discover_options &options = {})
    -> refrain::result<std::vector<discovered_mod>> {
    const std::array<discovery_root, 1> roots{
        discovery_root{.path = root, .origin = origin}};
    return scan(roots, options);
}

[[nodiscard]] auto manifest_hash_of(std::string_view manifest_path)
    -> refrain::data::digest32 {
    auto bytes{core_io::read_file(manifest_path)};
    if (!bytes) {
        return {};
    }
    return blake3(*bytes);
}

/// Replace the first occurrence; returns the text unchanged when not found.
[[nodiscard]] auto replace_first(std::string text, std::string_view from,
                                 std::string_view to) -> std::string {
    const auto position{text.find(from)};
    if (position != std::string::npos) {
        text.replace(position, from.size(), to);
    }
    return text;
}

[[nodiscard]] auto rewrite_lock_file(const std::string &lock_path,
                                     std::string_view from, std::string_view to)
    -> bool {
    auto text{core_io::read_text(lock_path)};
    if (!text) {
        return false;
    }
    if (text->find(from) == std::string::npos) {
        return false;
    }
    const auto updated{replace_first(*text, from, to)};
    return core_io::write_text(lock_path, updated).has_value();
}

[[nodiscard]] auto write_lock_file(const temp_directory &scratch,
                                   const mod_lock &lock, std::string_view name)
    -> std::string {
    const auto lock_path{path_join(scratch.path.string(), name)};
    expect(write_mod_lock(lock_path, lock).has_value());
    return lock_path;
}

const suite discovery_tests = [] {
    "discovery/origin spelling and priority"_test = [] {
        expect(to_string(mod_origin::builtin) == "builtin");
        expect(to_string(mod_origin::first_party) == "first_party");
        expect(to_string(mod_origin::user) == "user");
        expect(parse_mod_origin("builtin") == mod_origin::builtin);
        expect(parse_mod_origin("first_party") == mod_origin::first_party);
        expect(parse_mod_origin("user") == mod_origin::user);
        expect(!parse_mod_origin("User").has_value());
        expect(!parse_mod_origin("").has_value());
        expect(!parse_mod_origin("third_party").has_value());
        // Higher origin tier = higher priority.
        expect(origin_rank(mod_origin::user) >
               origin_rank(mod_origin::first_party));
        expect(origin_rank(mod_origin::first_party) >
               origin_rank(mod_origin::builtin));
    };

    "discovery/scan finds one mod and hashes its manifest bytes"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto manifest_path{write_manifest(root, "alpha", "1.2.3")};
        const auto scanned{scan_single(root, mod_origin::first_party)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        expect(scanned->size() == 1U);
        const auto *mod{find_discovered(*scanned, "alpha")};
        expect(mod != nullptr);
        if (mod == nullptr) {
            return;
        }
        expect(mod->id == "alpha");
        expect(mod->version == "1.2.3");
        expect(mod->abi == 1U);
        expect(mod->origin == mod_origin::first_party);
        expect(mod->path == manifest_path);
        expect(to_hex(mod->manifest_hash) ==
               to_hex(manifest_hash_of(manifest_path)));
        expect(mod->shadowed.empty());
        expect(mod->required.empty());
        expect(mod->optional.empty());
    };

    "discovery/scan output is sorted by id"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "zeta", "1.0.0");
        write_manifest(root, "alpha", "1.0.0");
        write_manifest(root, "mid", "1.0.0");
        const auto first{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(first));
        if (!first) {
            return;
        }
        expect(first->size() == 3U);
        if (first->size() == 3U) {
            expect((*first)[0].id == "alpha");
            expect((*first)[1].id == "mid");
            expect((*first)[2].id == "zeta");
        }
        // A second scan is byte-for-byte equivalent (deterministic).
        const auto again{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(again));
        if (again && again->size() == first->size()) {
            for (std::size_t index{0}; index < again->size(); ++index) {
                expect((*again)[index].id == (*first)[index].id);
                expect((*again)[index].path == (*first)[index].path);
                expect(to_hex((*again)[index].manifest_hash) ==
                       to_hex((*first)[index].manifest_hash));
            }
        }
    };

    "discovery/scan keeps the highest origin and reports shadows"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto builtin_root{create_root(scratch, "builtin")};
        const auto first_party_root{create_root(scratch, "first_party")};
        const auto user_root{create_root(scratch, "user")};
        write_manifest(builtin_root, "shared", "0.9.0");
        write_manifest(first_party_root, "shared", "1.0.0");
        const auto user_manifest{write_manifest(user_root, "shared", "1.1.0")};

        std::vector<discovery_root> roots{
            discovery_root{.path = builtin_root, .origin = mod_origin::builtin},
            discovery_root{.path = first_party_root,
                           .origin = mod_origin::first_party},
            discovery_root{.path = user_root, .origin = mod_origin::user},
        };
        const auto scanned{scan(roots)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        expect(scanned->size() == 1U);
        const auto *winner{find_discovered(*scanned, "shared")};
        expect(winner != nullptr);
        if (winner == nullptr) {
            return;
        }
        expect(winner->origin == mod_origin::user);
        expect(winner->version == "1.1.0");
        expect(winner->path == user_manifest);
        expect(winner->shadowed.size() == 2U);
        if (winner->shadowed.size() == 2U) {
            expect(winner->shadowed[0].origin == mod_origin::builtin);
            expect(winner->shadowed[0].version == "0.9.0");
            expect(winner->shadowed[1].origin == mod_origin::first_party);
            expect(winner->shadowed[1].version == "1.0.0");
        }
        // Root listing order must not change the winner/shadow set.
        std::reverse(roots.begin(), roots.end());
        const auto reversed{scan(roots)};
        expect(static_cast<bool>(reversed));
        if (reversed && reversed->size() == 1U) {
            const auto *same{find_discovered(*reversed, "shared")};
            expect(same != nullptr);
            if (same != nullptr) {
                expect(same->origin == mod_origin::user);
                expect(same->path == user_manifest);
                expect(same->shadowed.size() == 2U);
            }
        }
    };

    "discovery/scan rejects duplicate ids within one origin"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root_a{create_root(scratch, "a")};
        const auto root_b{create_root(scratch, "b")};
        write_manifest(root_a, "dup", "1.0.0");
        write_manifest(root_b, "dup", "1.0.0");
        const std::array<discovery_root, 2> roots{
            discovery_root{.path = root_a, .origin = mod_origin::user},
            discovery_root{.path = root_b, .origin = mod_origin::user}};
        const auto scanned{scan(roots)};
        expect(!scanned);
        if (!scanned) {
            expect(scanned.error().code == refrain::error_code::already_exists);
            expect(scanned.error().message.find("dup") != std::string::npos);
        }
    };

    // Same id and origin with different versions: the highest SemVer wins and
    // the lower version stays auditable in `shadowed`.
    "discovery/scan picks the highest SemVer within one origin"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root_a{create_root(scratch, "v1")};
        const auto root_b{create_root(scratch, "v2")};
        write_manifest(root_a, "multi-version", "1.2.0");
        write_manifest(root_b, "multi-version", "1.10.0");
        const std::array<discovery_root, 2> roots{
            discovery_root{.path = root_a, .origin = mod_origin::user},
            discovery_root{.path = root_b, .origin = mod_origin::user}};
        const auto scanned{scan(roots)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        expect(scanned->size() == 1U);
        if (scanned->size() == 1U) {
            expect((*scanned)[0].version == "1.10.0");
            expect((*scanned)[0].shadowed.size() == 1U);
            if ((*scanned)[0].shadowed.size() == 1U) {
                expect((*scanned)[0].shadowed[0].version == "1.2.0");
            }
        }
    };

    "discovery/scan tolerates or requires the mods directory"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "empty-root")};
        (void)root;
        const auto tolerated{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(tolerated));
        if (tolerated) {
            expect(tolerated->empty());
        }
        discover_options strict;
        strict.require_mods_dir = true;
        const auto required{scan_single(root, mod_origin::user, strict)};
        expect(!required);
        if (!required) {
            expect(required.error().code == refrain::error_code::io_error);
            expect(required.error().message.find("mods") != std::string::npos);
        }
    };

    "discovery/scan root must exist and be a directory"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto missing{path_join(scratch.path.string(), "does-not-exist")};
        const auto absent{scan_single(missing, mod_origin::user)};
        expect(!absent);
        if (!absent) {
            expect(absent.error().code == refrain::error_code::io_error);
        }
        const auto file_root{path_join(scratch.path.string(), "root-file")};
        expect(core_io::write_text(file_root, "not a directory").has_value());
        const auto file_result{scan_single(file_root, mod_origin::user)};
        expect(!file_result);
        if (!file_result) {
            expect(file_result.error().code == refrain::error_code::io_error);
        }
    };

    "discovery/scan skips unknown entries unless strict"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto mods_dir{path_join(root, "mods")};
        expect(core_io::create_directories(mods_dir).has_value());
        expect(core_io::write_text(path_join(mods_dir, "stray.rfmod"), "x")
                   .has_value());
        expect(core_io::create_directories(path_join(mods_dir, "empty-dir"))
                   .has_value());
        const auto skipped{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(skipped));
        if (skipped) {
            expect(skipped->empty());
        }
        discover_options strict;
        strict.strict_entries = true;
        const auto rejected{scan_single(root, mod_origin::user, strict)};
        expect(!rejected);
        if (!rejected) {
            expect(rejected.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "discovery/scan directory name must match the manifest id"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto manifest_path{write_raw_manifest(
            root, "wrong-dir", manifest_text("right.id", "1.0.0"))};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(!scanned);
        if (!scanned) {
            expect(scanned.error().code == refrain::error_code::parse_error);
            expect(scanned.error().message.find("right.id") !=
                   std::string::npos);
            expect(scanned.error().message.find("wrong-dir") !=
                   std::string::npos);
            expect(scanned.error().message.find(manifest_path) !=
                   std::string::npos);
        }
    };

    "discovery/scan malformed manifests are parse errors naming the path"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto broken_root{create_root(scratch, "broken-root")};
            const auto broken_text{
                write_raw_manifest(broken_root, "broken", "[mod\nid = oops")};
            const auto broken{scan_single(broken_root, mod_origin::user)};
            expect(!broken);
            if (!broken) {
                expect(broken.error().code == refrain::error_code::parse_error);
                expect(broken.error().message.find(broken_text) !=
                       std::string::npos);
            }
            // Valid TOML but not a mod manifest (unknown root table).
            const auto wrong_root{create_root(scratch, "wrong-root")};
            const auto wrong_text{write_raw_manifest(
                wrong_root, "unknown-keys",
                "[bogus]\nvalue = 1\n[mod]\nid = \"unknown-keys\"\n"
                "version = \"1.0.0\"\nabi = \"1\"\n")};
            const auto wrong{scan_single(wrong_root, mod_origin::user)};
            expect(!wrong);
            if (!wrong) {
                expect(wrong.error().code == refrain::error_code::parse_error);
                expect(wrong.error().message.find(wrong_text) !=
                       std::string::npos);
            }
        };

    "discovery/scan unreadable manifest is an io error"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto mod_dir{path_join(path_join(root, "mods"), "dir-manifest")};
        // A directory where the manifest file is expected: reading it fails.
        expect(core_io::create_directories(path_join(mod_dir, "manifest.toml"))
                   .has_value());
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(!scanned);
        if (!scanned) {
            expect(scanned.error().code == refrain::error_code::io_error);
        }
    };

    // The manifest reader caps untrusted input: >1 MiB is refused before the
    // parser (and before any full-size allocation).
    "discovery/scan rejects oversized manifests"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto manifest_path{write_manifest(root, "big-mod", "1.0.0")};
        std::error_code resize_error;
        fs::resize_file(manifest_path, 1024U * 1024U + 1U, resize_error);
        if (resize_error) {
            return; // filesystem without resize support
        }
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(!scanned);
        if (!scanned) {
            expect(scanned.error().code ==
                   refrain::error_code::invalid_argument);
            expect(scanned.error().message.find(manifest_path) !=
                   std::string::npos);
        }
    };

    // A manifest symlink could redirect the scan outside the instance root.
    "discovery/scan rejects symlinked manifests"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto manifest_path{write_manifest(root, "linked-mod", "1.0.0")};
        const auto outside{path_join(scratch.path.string(), "outside.toml")};
        expect(
            core_io::write_text(outside, manifest_text("linked-mod", "1.0.0"))
                .has_value());
        std::error_code link_error;
        fs::remove(manifest_path, link_error);
        link_error.clear();
        fs::create_symlink(outside, manifest_path, link_error);
        if (link_error) {
            return; // no symlink support/privilege on this host
        }
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(!scanned);
        if (!scanned) {
            expect(scanned.error().code ==
                   refrain::error_code::permission_denied);
            expect(scanned.error().message.find(manifest_path) !=
                   std::string::npos);
        }
    };

    "discovery/scan case-only duplicate id within one origin is rejected"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{create_root(scratch, "root")};
            write_manifest(root, "CaseMod", "1.0.0");
            write_manifest(root, "casemod", "1.0.0");
            const auto scanned{scan_single(root, mod_origin::user)};
            expect(!scanned);
            if (!scanned) {
                expect(scanned.error().code ==
                       refrain::error_code::already_exists);
            }
        };

    // Case-folding is per id, never per sorted neighbour: a colliding pair
    // must still be caught when an unrelated id sorts between them.
    "discovery/scan case-only duplicate across non-adjacent ids"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "CaseA", "1.0.0");
        write_manifest(root, "Between", "1.0.0");
        write_manifest(root, "casea", "1.0.0");
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(!scanned);
        if (!scanned) {
            expect(scanned.error().code == refrain::error_code::already_exists);
            expect(scanned.error().message.find("CaseA") != std::string::npos);
            expect(scanned.error().message.find("installed twice") !=
                   std::string::npos);
        }
    };

    "discovery/scan normalises separators and keeps unicode paths"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto unicode_root{create_root(scratch, "уникод-root")};
        const auto manifest_path{
            write_manifest(unicode_root, "alpha", "1.0.0")};
        // Pass the root with redundant trailing/duplicate separators.
        const auto scanned{
            scan_single(unicode_root + "///", mod_origin::first_party)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        expect(scanned->size() == 1U);
        const auto *mod{find_discovered(*scanned, "alpha")};
        expect(mod != nullptr);
        if (mod != nullptr) {
            expect(mod->path == manifest_path);
            expect(mod->path.find("//") == std::string::npos);
        }
    };

    "discovery/resolve auto_scan selects every discovered mod"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "b-mod", "1.0.0");
        write_manifest(root, "a-mod", "2.0.0");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::first_party)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        expect(resolved->format_version == mod_lock_format_version);
        expect(resolved->gamepack_id == "test.gamepack");
        expect(resolved->gamepack_version == "1.0.0");
        expect(resolved->mods.size() == 2U);
        if (resolved->mods.size() == 2U) {
            expect(resolved->mods[0].id == "a-mod");
            expect(resolved->mods[1].id == "b-mod");
        }
        const auto *a{find_locked(*resolved, "a-mod")};
        expect(a != nullptr);
        if (a != nullptr) {
            expect(a->version == "2.0.0");
            expect(a->abi == 1U);
            expect(a->origin == mod_origin::first_party);
            expect(a->path.find("a-mod") != std::string::npos);
            expect(to_hex(a->manifest_hash) ==
                   to_hex(manifest_hash_of(
                       path_join(path_join(path_join(root, "mods"), "a-mod"),
                                 "manifest.toml"))));
        }
        expect(resolved->warnings.empty());
    };

    "discovery/resolve auto_scan off keeps only explicit pins"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        write_manifest(root, "b-mod", "1.0.0");
        auto spec{make_spec(false)};
        pin_mod(spec, "a-mod", "1.0.0");
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (resolved) {
            expect(resolved->mods.size() == 1U);
            if (resolved->mods.size() == 1U) {
                expect(resolved->mods[0].id == "a-mod");
            }
        }
        // No pins at all: a valid but empty lock (still has identity fields).
        auto empty_spec{make_spec(false)};
        const auto empty_lock{refrain::mod::resolve(empty_spec, *scanned)};
        expect(static_cast<bool>(empty_lock));
        if (empty_lock) {
            expect(empty_lock->mods.empty());
            expect(empty_lock->gamepack_id == "test.gamepack");
        }
    };

    // The ABI gate is not only for explicit pins: auto_scan candidates built
    // for another gamepack ABI must fail resolution instead of being pushed
    // into the lock unchecked.
    "discovery/resolve auto_scan enforces the abi gate"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "good-mod", "1.0.0", {}, 1);
        write_manifest(root, "abi-99-mod", "1.0.0", {}, 99);
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        auto spec{make_spec(true, 1)};
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(!resolved);
        if (!resolved) {
            expect(resolved.error().code ==
                   refrain::error_code::version_mismatch);
            expect(resolved.error().message.find("abi-99-mod") !=
                   std::string::npos);
            expect(resolved.error().message.find("declares abi 99") !=
                   std::string::npos);
            expect(resolved.error().message.find("requires abi 1") !=
                   std::string::npos);
        }
        // With a consistent ABI the same auto_scan set resolves.
        write_manifest(root, "good-mod", "1.0.0", {}, 99);
        const auto scanned_consistent{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned_consistent));
        if (!scanned_consistent) {
            return;
        }
        auto consistent_spec{make_spec(true, 99)};
        const auto consistent{
            refrain::mod::resolve(consistent_spec, *scanned_consistent)};
        expect(static_cast<bool>(consistent));
        if (consistent) {
            expect(consistent->mods.size() == 2U);
        }
    };

    "discovery/resolve missing and mismatched pins are rejected"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        auto missing_spec{make_spec(false)};
        pin_mod(missing_spec, "ghost", "1.0.0");
        const auto missing{refrain::mod::resolve(missing_spec, *scanned)};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
            expect(missing.error().message.find("ghost") != std::string::npos);
        }
        auto version_spec{make_spec(false)};
        pin_mod(version_spec, "a-mod", "2.0.0");
        const auto mismatch{refrain::mod::resolve(version_spec, *scanned)};
        expect(!mismatch);
        if (!mismatch) {
            expect(mismatch.error().code ==
                   refrain::error_code::version_mismatch);
            expect(mismatch.error().message.find("1.0.0") != std::string::npos);
            expect(mismatch.error().message.find("2.0.0") != std::string::npos);
        }
        // ABI gate: the manifest declares abi 2 while the gamepack needs abi 1.
        write_manifest(root, "abi-mod", "1.0.0", {}, 2);
        const auto scanned_abi{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned_abi));
        if (!scanned_abi) {
            return;
        }
        auto abi_spec{make_spec(false, 1)};
        pin_mod(abi_spec, "abi-mod", "1.0.0");
        const auto abi_mismatch{refrain::mod::resolve(abi_spec, *scanned_abi)};
        expect(!abi_mismatch);
        if (!abi_mismatch) {
            expect(abi_mismatch.error().code ==
                   refrain::error_code::version_mismatch);
            expect(abi_mismatch.error().message.find("abi") !=
                   std::string::npos);
        }
        // Duplicate pin entries are rejected before resolution.
        auto duplicate_spec{make_spec(false)};
        pin_mod(duplicate_spec, "a-mod", "1.0.0");
        pin_mod(duplicate_spec, "a-mod", "1.0.0");
        const auto duplicate{refrain::mod::resolve(duplicate_spec, *scanned)};
        expect(!duplicate);
        if (!duplicate) {
            expect(duplicate.error().code ==
                   refrain::error_code::already_exists);
        }
    };

    "discovery/resolve invalid gamepack specs are rejected"_test = [] {
        const std::array<discovered_mod, 0> none{};
        auto empty_id{make_spec()};
        empty_id.id.clear();
        expect(!refrain::mod::resolve(empty_id, none));
        if (!refrain::mod::resolve(empty_id, none)) {
            expect(refrain::mod::resolve(empty_id, none).error().code ==
                   refrain::error_code::invalid_argument);
        }
        auto bad_version{make_spec()};
        bad_version.version = "1.x";
        expect(!refrain::mod::resolve(bad_version, none));
        auto zero_abi{make_spec(true, 0)};
        expect(!refrain::mod::resolve(zero_abi, none));
        auto bad_conflict{make_spec()};
        bad_conflict.policy.conflict = "yolo";
        expect(!refrain::mod::resolve(bad_conflict, none));
        auto bad_origin{make_spec()};
        bad_origin.policy.origin_priority = {"builtin", "moon"};
        expect(!refrain::mod::resolve(bad_origin, none));
        auto custom_order{make_spec()};
        custom_order.policy.origin_priority = {"user", "first_party",
                                               "builtin"};
        const auto custom_result{refrain::mod::resolve(custom_order, none)};
        expect(!custom_result);
        if (!custom_result) {
            expect(custom_result.error().code ==
                   refrain::error_code::unsupported);
        }
    };

    "discovery/resolve conflict fail rejects cross-origin shadowing"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto builtin_root{create_root(scratch, "builtin")};
        const auto user_root{create_root(scratch, "user")};
        write_manifest(builtin_root, "shared", "1.0.0");
        write_manifest(user_root, "shared", "1.1.0");
        const std::array<discovery_root, 2> roots{
            discovery_root{.path = builtin_root, .origin = mod_origin::builtin},
            discovery_root{.path = user_root, .origin = mod_origin::user}};
        const auto scanned{scan(roots)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        auto spec{make_spec(true)};
        spec.policy.conflict = "fail";
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(!resolved);
        if (!resolved) {
            expect(resolved.error().code ==
                   refrain::error_code::already_exists);
            expect(resolved.error().message.find("builtin") !=
                   std::string::npos);
            expect(resolved.error().message.find("user") != std::string::npos);
        }
    };

    "discovery/resolve explicit policy overrides with a warning"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto builtin_root{create_root(scratch, "builtin")};
        const auto user_root{create_root(scratch, "user")};
        write_manifest(builtin_root, "shared", "1.0.0");
        write_manifest(user_root, "shared", "1.1.0");
        const std::array<discovery_root, 2> roots{
            discovery_root{.path = builtin_root, .origin = mod_origin::builtin},
            discovery_root{.path = user_root, .origin = mod_origin::user}};
        const auto scanned{scan(roots)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        auto spec{make_spec(true)};
        spec.policy.conflict = "explicit";
        spec.policy.allow_custom_override = true;
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        expect(resolved->mods.size() == 1U);
        if (resolved->mods.size() == 1U) {
            expect(resolved->mods[0].id == "shared");
            expect(resolved->mods[0].origin == mod_origin::user);
            expect(resolved->mods[0].version == "1.1.0");
        }
        expect(resolved->warnings.size() == 1U);
        if (resolved->warnings.size() == 1U) {
            expect(resolved->warnings[0].find("builtin") != std::string::npos);
            expect(resolved->warnings[0].find("user") != std::string::npos);
        }
        // Overriding is only legal when policy allows it.
        auto strict_spec{make_spec(true)};
        strict_spec.policy.conflict = "explicit";
        strict_spec.policy.allow_custom_override = false;
        const auto forbidden{refrain::mod::resolve(strict_spec, *scanned)};
        expect(!forbidden);
        if (!forbidden) {
            expect(forbidden.error().code ==
                   refrain::error_code::already_exists);
        }
    };

    "discovery/resolve shadowed candidates are ignored when not selected"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto builtin_root{create_root(scratch, "builtin")};
            const auto user_root{create_root(scratch, "user")};
            write_manifest(builtin_root, "shared", "1.0.0");
            write_manifest(user_root, "shared", "1.1.0");
            const std::array<discovery_root, 2> roots{
                discovery_root{.path = builtin_root,
                               .origin = mod_origin::builtin},
                discovery_root{.path = user_root, .origin = mod_origin::user}};
            const auto scanned{scan(roots)};
            expect(static_cast<bool>(scanned));
            if (!scanned) {
                return;
            }
            // auto_scan = false and no pins: the shadowed id is not selected,
            // so policy.conflict = fail must not fire.
            auto spec{make_spec(false)};
            spec.policy.conflict = "fail";
            const auto resolved{refrain::mod::resolve(spec, *scanned)};
            expect(static_cast<bool>(resolved));
            if (resolved) {
                expect(resolved->mods.empty());
            }
        };

    "discovery/resolve dependency satisfied across selected mods"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "dep-mod", "1.4.0");
        write_manifest(root, "user-mod", "1.0.0",
                       "\n[requires]\n\"dep-mod\" = \"^1.0.0\"\n");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (resolved) {
            expect(resolved->mods.size() == 2U);
        }
    };

    "discovery/resolve dependency version mismatch"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "dep-mod", "2.0.0");
        write_manifest(root, "user-mod", "1.0.0",
                       "\n[requires]\n\"dep-mod\" = \"^1.0.0\"\n");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(!resolved);
        if (!resolved) {
            expect(resolved.error().code ==
                   refrain::error_code::version_mismatch);
            expect(resolved.error().message.find("dep-mod") !=
                   std::string::npos);
            expect(resolved.error().message.find("^1.0.0") !=
                   std::string::npos);
            expect(resolved.error().message.find("2.0.0") != std::string::npos);
        }
    };

    "discovery/resolve missing dependency is not_found with the edge"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{create_root(scratch, "root")};
            write_manifest(root, "user-mod", "1.0.0",
                           "\n[requires]\n\"missing-mod\" = \"^1.0.0\"\n");
            auto spec{make_spec(true)};
            const auto scanned{scan_single(root, mod_origin::user)};
            expect(static_cast<bool>(scanned));
            if (!scanned) {
                return;
            }
            const auto resolved{refrain::mod::resolve(spec, *scanned)};
            expect(!resolved);
            if (!resolved) {
                expect(resolved.error().code == refrain::error_code::not_found);
                expect(resolved.error().message.find("missing-mod") !=
                       std::string::npos);
                expect(resolved.error().message.find("user-mod") !=
                       std::string::npos);
            }
        };

    "discovery/resolve dependency cycle is deterministic"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "cycle-a", "1.0.0",
                       "\n[requires]\n\"cycle-b\" = \"^1.0.0\"\n");
        write_manifest(root, "cycle-b", "1.0.0",
                       "\n[requires]\n\"cycle-a\" = \"^1.0.0\"\n");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto first{refrain::mod::resolve(spec, *scanned)};
        const auto second{refrain::mod::resolve(spec, *scanned)};
        expect(!first);
        expect(!second);
        if (!first && !second) {
            expect(first.error().code == refrain::error_code::mod_error);
            expect(first.error().message.find("cycle") != std::string::npos);
            expect(first.error().message.find("cycle-a") != std::string::npos);
            expect(first.error().message.find("cycle-b") != std::string::npos);
            expect(first.error().message == second.error().message);
        }
    };

    "discovery/resolve self-cycle from crafted candidates"_test = [] {
        discovered_mod self{};
        self.id = "self-mod";
        self.version = "1.0.0";
        self.abi = 1;
        self.origin = mod_origin::user;
        self.path = "mods/self-mod/manifest.toml";
        self.required.push_back(refrain::kernel::mod_requirement{
            .id = "self-mod", .range = "^1.0.0"});
        const std::array<discovered_mod, 1> candidates{self};
        auto spec{make_spec(true)};
        const auto resolved{refrain::mod::resolve(spec, candidates)};
        expect(!resolved);
        if (!resolved) {
            expect(resolved.error().code == refrain::error_code::mod_error);
            expect(resolved.error().message.find("self-mod") !=
                   std::string::npos);
        }
    };

    "discovery/resolve optional dependencies tolerate absence, check versions"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{create_root(scratch, "root")};
            write_manifest(root, "opt-user", "1.0.0",
                           "\n[optional_requires]\n\"opt-dep\" = \"^1.0.0\"\n");
            auto spec{make_spec(true)};
            const auto absent_scan{scan_single(root, mod_origin::user)};
            expect(static_cast<bool>(absent_scan));
            if (!absent_scan) {
                return;
            }
            const auto absent{refrain::mod::resolve(spec, *absent_scan)};
            expect(static_cast<bool>(absent));
            if (absent) {
                expect(absent->warnings.size() == 1U);
                if (absent->warnings.size() == 1U) {
                    expect(absent->warnings[0].find("opt-dep") !=
                           std::string::npos);
                }
            }
            // Present but outside the optional range: rejected by default.
            write_manifest(root, "opt-dep", "2.0.0");
            const auto present_scan{scan_single(root, mod_origin::user)};
            expect(static_cast<bool>(present_scan));
            if (!present_scan) {
                return;
            }
            const auto mismatch{refrain::mod::resolve(spec, *present_scan)};
            expect(!mismatch);
            if (!mismatch) {
                expect(mismatch.error().code ==
                       refrain::error_code::version_mismatch);
            }
            // ... and tolerated when optional checks are disabled.
            discover_options lax;
            lax.check_optional_requirements = false;
            const auto lax_result{
                refrain::mod::resolve(spec, *present_scan, lax)};
            expect(static_cast<bool>(lax_result));
            if (lax_result) {
                expect(lax_result->mods.size() == 2U);
            }
        };

    "discovery/lock to_toml is deterministic and sorted"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "zeta-mod", "1.0.0");
        write_manifest(root, "alpha-mod", "1.0.0");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        const auto first{resolved->to_toml()};
        const auto second{resolved->to_toml()};
        expect(first == second);
        expect(first.find("[lock]") != std::string::npos);
        expect(first.find("format_version = 1") != std::string::npos);
        expect(first.find("gamepack_id = \"test.gamepack\"") !=
               std::string::npos);
        expect(first.find("[mods.\"alpha-mod\"]") != std::string::npos);
        expect(first.find("[mods.\"zeta-mod\"]") != std::string::npos);
        // Ascending id order: alpha-mod appears before zeta-mod.
        expect(first.find("[mods.\"alpha-mod\"]") <
               first.find("[mods.\"zeta-mod\"]"));
        const auto fingerprint{resolved->fingerprint()};
        expect(fingerprint.size() == 64U);
        expect(std::all_of(fingerprint.begin(), fingerprint.end(),
                           [](char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'a' && character <= 'f');
                           }));
        expect(first.find("fingerprint = \"" + fingerprint + "\"") !=
               std::string::npos);
        const auto canonical{resolved->canonical_text()};
        expect(canonical.find("refrain-mods-lock") != std::string::npos);
        expect(canonical.find("gamepack=test.gamepack@1.0.0") !=
               std::string::npos);
        expect(canonical.find("mod=alpha-mod\t1.0.0\t1\tuser\t") !=
               std::string::npos);
        expect(format_mod_lock(*resolved).find("test.gamepack@1.0.0") !=
               std::string::npos);
    };

    "discovery/lock fingerprint ignores host-local paths"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root_a{create_root(scratch, "instance-a")};
        const auto root_b{create_root(scratch, "instance-b")};
        write_manifest(root_a, "same-mod", "1.0.0");
        write_manifest(root_b, "same-mod", "1.0.0");
        auto spec{make_spec(true)};
        const auto scanned_a{scan_single(root_a, mod_origin::user)};
        const auto scanned_b{scan_single(root_b, mod_origin::user)};
        expect(static_cast<bool>(scanned_a));
        expect(static_cast<bool>(scanned_b));
        if (!scanned_a || !scanned_b) {
            return;
        }
        const auto lock_a{refrain::mod::resolve(spec, *scanned_a)};
        const auto lock_b{refrain::mod::resolve(spec, *scanned_b)};
        expect(static_cast<bool>(lock_a));
        expect(static_cast<bool>(lock_b));
        if (!lock_a || !lock_b) {
            return;
        }
        // Different install roots => different audit paths ...
        expect(lock_a->mods.at(0).path != lock_b->mods.at(0).path);
        // ... but the composition identity is identical.
        expect(lock_a->fingerprint() == lock_b->fingerprint());
        expect(lock_a->canonical_text() == lock_b->canonical_text());
    };

    "discovery/lock write/read roundtrip"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        write_manifest(root, "b-mod", "2.3.4");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::first_party)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        const auto lock_path{write_lock_file(scratch, *resolved, "mods.lock")};
        const auto read{read_mod_lock(lock_path)};
        expect(static_cast<bool>(read));
        if (!read) {
            return;
        }
        expect(read->format_version == mod_lock_format_version);
        expect(read->gamepack_id == resolved->gamepack_id);
        expect(read->gamepack_version == resolved->gamepack_version);
        expect(read->fingerprint() == resolved->fingerprint());
        expect(read->mods.size() == resolved->mods.size());
        for (const auto &mod : resolved->mods) {
            const auto *roundtrip{find_locked(*read, mod.id)};
            expect(roundtrip != nullptr);
            if (roundtrip == nullptr) {
                continue;
            }
            expect(roundtrip->version == mod.version);
            expect(roundtrip->abi == mod.abi);
            expect(roundtrip->origin == mod.origin);
            expect(roundtrip->path == mod.path);
            expect(to_hex(roundtrip->manifest_hash) ==
                   to_hex(mod.manifest_hash));
        }
        // The TOML round-trip is byte-stable as well.
        expect(read->to_toml() == resolved->to_toml());
        expect(read->warnings.empty());
    };

    "discovery/lock read rejects tampered version and hash"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        const auto lock_path{
            write_lock_file(scratch, *resolved, "version.lock")};
        expect(rewrite_lock_file(lock_path, "version = \"1.0.0\"",
                                 "version = \"9.9.9\""));
        const auto version_read{read_mod_lock(lock_path)};
        expect(!version_read);
        if (!version_read) {
            expect(version_read.error().code ==
                   refrain::error_code::parse_error);
            expect(version_read.error().message.find("fingerprint") !=
                   std::string::npos);
        }
        // Restore and tamper the manifest hash instead.
        expect(write_mod_lock(lock_path, *resolved).has_value());
        const auto hash_hex{to_hex(resolved->mods.at(0).manifest_hash)};
        std::string forged{hash_hex};
        forged[0] = forged[0] == '0' ? '1' : '0';
        expect(rewrite_lock_file(lock_path,
                                 "manifest_hash = \"" + hash_hex + "\"",
                                 "manifest_hash = \"" + forged + "\""));
        const auto hash_read{read_mod_lock(lock_path)};
        expect(!hash_read);
        if (!hash_read) {
            expect(hash_read.error().code == refrain::error_code::parse_error);
        }
    };

    "discovery/lock read rejects tampered path and fingerprint"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        const auto lock_path{write_lock_file(scratch, *resolved, "path.lock")};
        // Path must stay in the scanned layout.
        expect(rewrite_lock_file(lock_path, "manifest.toml", "evil.toml"));
        const auto path_read{read_mod_lock(lock_path)};
        expect(!path_read);
        if (!path_read) {
            expect(path_read.error().code == refrain::error_code::parse_error);
        }
        // Restore and tamper the fingerprint itself.
        expect(write_mod_lock(lock_path, *resolved).has_value());
        const auto fingerprint{resolved->fingerprint()};
        std::string forged(64U, 'a');
        expect(rewrite_lock_file(lock_path,
                                 "fingerprint = \"" + fingerprint + "\"",
                                 "fingerprint = \"" + forged + "\""));
        const auto fingerprint_read{read_mod_lock(lock_path)};
        expect(!fingerprint_read);
        if (!fingerprint_read) {
            expect(fingerprint_read.error().code ==
                   refrain::error_code::parse_error);
            expect(fingerprint_read.error().message.find("fingerprint") !=
                   std::string::npos);
        }
    };

    "discovery/lock read rejects structural corruption"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        const auto lock_path{
            write_lock_file(scratch, *resolved, "struct.lock")};
        expect(rewrite_lock_file(lock_path, "format_version = 1",
                                 "format_version = 2"));
        const auto version{read_mod_lock(lock_path)};
        expect(!version);
        if (!version) {
            expect(version.error().code ==
                   refrain::error_code::version_mismatch);
        }
        expect(write_mod_lock(lock_path, *resolved).has_value());
        expect(rewrite_lock_file(lock_path, "gamepack_id = \"test.gamepack\"",
                                 "gamepack_id = \"\""));
        const auto identity{read_mod_lock(lock_path)};
        expect(!identity);
        if (!identity) {
            expect(identity.error().code == refrain::error_code::parse_error);
        }
        expect(write_mod_lock(lock_path, *resolved).has_value());
        expect(rewrite_lock_file(lock_path, "origin = \"user\"",
                                 "origin = \"hacker\""));
        const auto origin{read_mod_lock(lock_path)};
        expect(!origin);
        if (!origin) {
            expect(origin.error().code == refrain::error_code::parse_error);
        }
        expect(write_mod_lock(lock_path, *resolved).has_value());
        const auto manifest_hash{to_hex(resolved->mods.at(0).manifest_hash)};
        expect(rewrite_lock_file(lock_path,
                                 "manifest_hash = \"" + manifest_hash + "\"",
                                 "manifest_hash = \"not-hex\""));
        const auto hash{read_mod_lock(lock_path)};
        expect(!hash);
        if (!hash) {
            expect(hash.error().code == refrain::error_code::parse_error);
        }
        // Unknown top-level table is rejected.
        expect(write_mod_lock(lock_path, *resolved).has_value());
        auto text{core_io::read_text(lock_path)};
        expect(static_cast<bool>(text));
        if (text) {
            expect(core_io::write_text(lock_path, *text + "\n[bogus]\nx = 1\n")
                       .has_value());
            const auto unknown{read_mod_lock(lock_path)};
            expect(!unknown);
            if (!unknown) {
                expect(unknown.error().code ==
                       refrain::error_code::parse_error);
            }
        }
        // Missing lock file surfaces an io_error.
        const auto absent{
            read_mod_lock(path_join(scratch.path.string(), "missing.lock"))};
        expect(!absent);
        if (!absent) {
            expect(absent.error().code == refrain::error_code::io_error);
        }
    };

    // Nested unknown keys are untrusted data too: rejecting them keeps a
    // hand-edited lock from carrying values no verifier looked at.
    "discovery/lock read rejects unknown nested keys"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        const auto lock_path{
            write_lock_file(scratch, *resolved, "unknown.lock")};

        // Unknown key in [lock].
        expect(rewrite_lock_file(lock_path, "format_version = 1",
                                 "format_version = 1\nextra = 1"));
        const auto lock_extra{read_mod_lock(lock_path)};
        expect(!lock_extra);
        if (!lock_extra) {
            expect(lock_extra.error().code == refrain::error_code::parse_error);
            expect(lock_extra.error().message.find("extra") !=
                   std::string::npos);
        }

        // Unknown key in [mods."<id>"] (`abi = "1"` only exists there).
        expect(write_mod_lock(lock_path, *resolved).has_value());
        expect(rewrite_lock_file(lock_path, "abi = \"1\"",
                                 "abi = \"1\"\nextra = 1"));
        const auto mod_extra{read_mod_lock(lock_path)};
        expect(!mod_extra);
        if (!mod_extra) {
            expect(mod_extra.error().code == refrain::error_code::parse_error);
            expect(mod_extra.error().message.find("extra") !=
                   std::string::npos);
        }

        // The untouched lock still reads (the whitelist accepts every key the
        // writer emits).
        expect(write_mod_lock(lock_path, *resolved).has_value());
        expect(static_cast<bool>(read_mod_lock(lock_path)));
    };

    "discovery/lock write fails when the parent directory is missing"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            mod_lock lock{};
            lock.gamepack_id = "test.gamepack";
            lock.gamepack_version = "1.0.0";
            const auto missing_path{path_join(
                path_join(scratch.path.string(), "missing-dir"), "mods.lock")};
            const auto written{write_mod_lock(missing_path, lock)};
            expect(!written);
            if (!written) {
                expect(written.error().code == refrain::error_code::io_error);
            }
        };

    "discovery/verify accepts a matching re-scan and detects drift"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto manifest_a{write_manifest(root, "a-mod", "1.0.0")};
        write_manifest(root, "b-mod", "1.0.0");
        auto spec{make_spec(true)};
        const auto scanned{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        const auto resolved{refrain::mod::resolve(spec, *scanned)};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        expect(verify_mod_lock(*resolved, *scanned).has_value());
        // Version drift after the fact.
        write_manifest(root, "a-mod", "1.0.1");
        const auto rescan_version{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(rescan_version));
        if (rescan_version) {
            const auto drift{verify_mod_lock(*resolved, *rescan_version)};
            expect(!drift);
            if (!drift) {
                expect(drift.error().code ==
                       refrain::error_code::version_mismatch);
                expect(drift.error().message.find("version drift") !=
                       std::string::npos);
            }
        }
        // Same version, changed bytes: manifest hash drift.
        write_manifest(root, "a-mod", "1.0.0",
                       "\n[meta]\ndisplay_name = \"changed\"\n");
        const auto rescan_hash{scan_single(root, mod_origin::user)};
        expect(static_cast<bool>(rescan_hash));
        if (rescan_hash) {
            const auto hash_drift{verify_mod_lock(*resolved, *rescan_hash)};
            expect(!hash_drift);
            if (!hash_drift) {
                expect(hash_drift.error().code ==
                       refrain::error_code::version_mismatch);
                expect(hash_drift.error().message.find("manifest hash") !=
                       std::string::npos);
            }
        }
        // Origin drift: same manifest scanned from a different tier.
        write_manifest(root, "a-mod", "1.0.0");
        const auto rescan_origin{scan_single(root, mod_origin::first_party)};
        expect(static_cast<bool>(rescan_origin));
        if (rescan_origin) {
            const auto origin_drift{verify_mod_lock(*resolved, *rescan_origin)};
            expect(!origin_drift);
            if (!origin_drift) {
                expect(origin_drift.error().code ==
                       refrain::error_code::version_mismatch);
                expect(origin_drift.error().message.find("origin drift") !=
                       std::string::npos);
            }
        }
        // Removed mod: missing from a fresh empty tree.
        const auto empty_scan{
            scan_single(create_root(scratch, "empty"), mod_origin::user)};
        expect(static_cast<bool>(empty_scan));
        if (empty_scan) {
            const auto missing{verify_mod_lock(*resolved, *empty_scan)};
            expect(!missing);
            if (!missing) {
                expect(missing.error().code ==
                       refrain::error_code::version_mismatch);
                expect(missing.error().message.find("not discovered") !=
                       std::string::npos);
            }
        }
        expect(std::string{manifest_a}.find("manifest.toml") !=
               std::string::npos);
    };

    // --- Single scanner and file collector -----------------------------------

    "discovery/scan_mods_tree scans a mods directory directly"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "a-mod", "1.0.0");
        write_manifest(root, "b-mod", "2.0.0");
        const auto mods_dir{path_join(root, "mods")};
        const auto scanned{refrain::mod::scan_mods_tree(mods_dir)};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        expect(scanned->size() == 2U);
        if (scanned->size() == 2U) {
            expect((*scanned)[0].id == "a-mod");
            expect((*scanned)[1].id == "b-mod");
            expect((*scanned)[0].kind == refrain::mod::package_kind::directory);
        }
    };

    "discovery/candidate mirrors the manifest fields"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        write_manifest(root, "rich", "1.2.3",
                       "[provides]\nservices = [\"refrain:data.packs\"]\n"
                       "[requires]\n\"dep\" = \"^1.0.0\"\n"
                       "[meta]\ndisplay_name = \"Rich mod\"\n");
        const auto scanned{refrain::mod::scan_mods_tree(path_join(root, "mods"),
                                                        mod_origin::user)};
        expect(static_cast<bool>(scanned));
        if (!scanned || scanned->size() != 1U) {
            return;
        }
        const auto &mod{(*scanned)[0]};
        expect(mod.id == "rich");
        expect(mod.version == "1.2.3");
        expect(mod.side == refrain::kernel::manifest_side::both);
        expect(mod.meta.display_name == "Rich mod");
        expect(mod.provides.size() == 1U);
        expect(mod.required.size() == 1U);
        expect(mod.kind == refrain::mod::package_kind::directory);

        // The audit text stays byte-identical to the manifest formatter, so
        // `mods check` output is unchanged by the unified scanner.
        auto manifest{refrain::kernel::load_mod_manifest(mod.path)};
        expect(static_cast<bool>(manifest));
        if (manifest) {
            expect(refrain::mod::format_candidate(mod) ==
                   refrain::kernel::format_mod_manifest(*manifest));
        }
    };

    "discovery/scan_mods_tree rejects a three-way case collision"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "root")};
        const auto mods_dir{path_join(root, "mods")};
        (void)core_io::create_directories(path_join(mods_dir, "alpha"));
        (void)core_io::write_text(
            path_join(path_join(mods_dir, "alpha"), "manifest.toml"),
            manifest_text("alpha", "1.0.0"));
        (void)core_io::create_directories(path_join(mods_dir, "Alpha"));
        (void)core_io::write_text(
            path_join(path_join(mods_dir, "Alpha"), "manifest.toml"),
            manifest_text("Alpha", "1.0.0"));
        (void)core_io::create_directories(path_join(mods_dir, "ALPHA"));
        (void)core_io::write_text(
            path_join(path_join(mods_dir, "ALPHA"), "manifest.toml"),
            manifest_text("ALPHA", "1.0.0"));
        const auto scanned{refrain::mod::scan_mods_tree(mods_dir)};
        expect(!scanned);
        if (!scanned) {
            expect(scanned.error().code == refrain::error_code::already_exists);
        }
    };

    "discovery/collect_package_files ships content and skips src/dotfiles"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{create_root(scratch, "mod")};
            (void)core_io::write_text(path_join(root, "manifest.toml"),
                                      manifest_text("pack.me", "1.0.0"));
            (void)core_io::write_text(path_join(root, "README.md"), "readme");
            (void)core_io::create_directories(path_join(root, "data"));
            (void)core_io::write_text(
                path_join(path_join(root, "data"), "a.txt"), "a");
            (void)core_io::create_directories(path_join(root, "assets"));
            (void)core_io::write_text(
                path_join(path_join(root, "assets"), ".hidden"), "hidden");
            (void)core_io::create_directories(path_join(root, "src"));
            (void)core_io::write_text(
                path_join(path_join(root, "src"), "main.cpp"), "int main;");
            constexpr std::array<std::string_view, 1> excluded{"src"};
            const auto collected{refrain::mod::collect_package_files(
                root, refrain::mod::package_collect_options{
                          .error_prefix = "test", .exclude_roots = excluded})};
            expect(static_cast<bool>(collected));
            if (!collected) {
                return;
            }
            expect(collected->size() == 3U);
            if (collected->size() == 3U) {
                expect((*collected)[0].path == "README.md");
                expect((*collected)[1].path == "data/a.txt");
                expect((*collected)[2].path == "manifest.toml");
            }
        };

    "discovery/collect_package_files rejects a symlink"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "mod")};
        (void)core_io::write_text(path_join(root, "manifest.toml"),
                                  manifest_text("pack.me", "1.0.0"));
        const auto outside{path_join(root, "outside.txt")};
        (void)core_io::write_text(outside, "secret");
        (void)core_io::create_directories(path_join(root, "data"));
        std::error_code link_error;
        fs::create_symlink(outside, path_join(path_join(root, "data"), "link"),
                           link_error);
        if (link_error) {
            return; // symlinks unsupported on this filesystem
        }
        const auto collected{refrain::mod::collect_package_files(
            root,
            refrain::mod::package_collect_options{.error_prefix = "test"})};
        expect(!collected);
        if (!collected) {
            expect(collected.error().code ==
                   refrain::error_code::permission_denied);
        }
    };

    "discovery/collect_package_files honours the rfmod layout"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{create_root(scratch, "mod")};
        (void)core_io::write_text(path_join(root, "manifest.toml"),
                                  manifest_text("pack.me", "1.0.0"));
        (void)core_io::write_text(path_join(root, "LICENSE"), "MIT");
        (void)core_io::write_text(path_join(root, "README.md"), "readme");
        (void)core_io::create_directories(path_join(root, "data"));
        (void)core_io::write_text(path_join(path_join(root, "data"), "a.txt"),
                                  "a");
        (void)core_io::create_directories(path_join(root, "docs"));
        (void)core_io::write_text(path_join(path_join(root, "docs"), "b.txt"),
                                  "b");
        constexpr std::array<std::string_view, 1> roots{"data"};
        constexpr std::array<std::string_view, 1> extra{"LICENSE"};
        const auto collected{refrain::mod::collect_package_files(
            root,
            refrain::mod::package_collect_options{.error_prefix = "test",
                                                  .roots = roots,
                                                  .extra_root_files = extra,
                                                  .include_manifest = false})};
        expect(static_cast<bool>(collected));
        if (!collected) {
            return;
        }
        expect(collected->size() == 2U);
        if (collected->size() == 2U) {
            expect((*collected)[0].path == "LICENSE");
            expect((*collected)[1].path == "data/a.txt");
        }
    };
};

} // namespace
