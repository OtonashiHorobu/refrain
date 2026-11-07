// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/startup_content.hpp>

#include <refrain/mod/discovery.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/mod/manifest.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::mod {
namespace {

namespace config = core::config;
namespace data = refrain::data;
namespace io = core::io;

using log_sink = std::function<void(std::string_view)>;

constexpr std::string_view installed_directory_name{"installed"};
constexpr std::string_view rejected_directory_name{"rejected"};

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

/// Prefix an error with the context it came from (never duplicates it).
[[nodiscard]] auto annotate(error original, std::string_view context) -> error {
    if (original.message.find(context) != std::string::npos) {
        return original;
    }
    original.message = std::string{context} + ": " + original.message;
    return original;
}

void emit(const log_sink &log, std::string line) {
    if (log) {
        log(line);
    }
}

[[nodiscard]] auto inbox_directory(const startup_content_options &options)
    -> std::string {
    return options.inbox_dir.empty()
               ? data::join_path(options.store_root, "mods/inbox")
               : options.inbox_dir;
}

[[nodiscard]] auto receipt_path(std::string_view store_root,
                                std::string_view id, std::string_view version)
    -> std::string {
    return data::join_path(
        data::join_path(data::join_path(store_root, "mods"), id),
        std::string{version} + ".json");
}

/// Validate the options that become paths or binary fields before any write.
[[nodiscard]] auto validate_options(const startup_content_options &options)
    -> status {
    if (options.store_root.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "startup-content: content store root must not be empty")};
    }
    if (options.store_root.find('\0') != std::string::npos) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "startup-content: content store root contains a NUL byte")};
    }
    if (!data::is_valid_content_composition(options.composition)) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("startup-content: composition must be a safe file name "
                        "of at most {} bytes ([A-Za-z0-9._-], not starting "
                        "with '.')",
                        data::max_content_composition_bytes))};
    }
    if (options.base_priority >
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "startup-content: base_priority does not fit the RFRX int32 "
                 "field")};
    }
    {
        std::vector<std::string> ordered{options.layer_order};
        std::sort(ordered.begin(), ordered.end());
        const auto duplicate{
            std::adjacent_find(ordered.begin(), ordered.end())};
        if (duplicate != ordered.end()) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("startup-content: layer_order contains duplicate "
                            "id '{}'",
                            *duplicate))};
        }
    }
    if (options.gc_keep_days < 0) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "startup-content: gc_keep_days must not be negative")};
    }
    if (options.lock_timeout_ms < 0) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "startup-content: lock_timeout_ms must not be negative")};
    }
    return status_ok();
}

// ---------------------------------------------------------------------------
// Stage 0: store lock
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Stages 1-2: inbox scan + install
// ---------------------------------------------------------------------------

struct package_identity {
    std::string id{};
    std::string version{};
    std::uint32_t abi{0};
    mod::manifest_side side{mod::manifest_side::both};
};

/// Parse the package manifest without verifying the whole archive: the cheap
/// scan only needs id/version (the receipt check), and `install_rfmod` runs
/// the full `verify_rfmod` before its first CAS write.
[[nodiscard]] auto read_package_identity(const rfmod_reader &reader,
                                         std::string_view path)
    -> result<package_identity> {
    auto bytes{reader.read(rfmod_manifest_path)};
    if (!bytes) {
        return std::unexpected{
            annotate(std::move(bytes.error()),
                     fmt::format("startup-content: '{}'", path))};
    }
    const std::string text{
        reinterpret_cast<const char *>(bytes->data()), // NOLINT
        bytes->size()};
    const auto origin{fmt::format("{}/{}", path, rfmod_manifest_path)};
    auto document{config::document::parse_text(text, origin)};
    if (!document) {
        return std::unexpected{
            annotate(std::move(document.error()),
                     fmt::format("startup-content: '{}'", path))};
    }
    auto manifest{mod::parse_mod_manifest(
        *document, mod::manifest_parse_options{.allow_package_tables = true})};
    if (!manifest) {
        return std::unexpected{
            annotate(std::move(manifest.error()),
                     fmt::format("startup-content: '{}'", path))};
    }
    return package_identity{.id = manifest->common.id,
                            .version = manifest->common.version,
                            .abi = manifest->common.abi,
                            .side = manifest->common.side};
}

/// First free `<directory>/<stem>[-<n>]<extension>`.
[[nodiscard]] auto unique_target(std::string_view directory,
                                 std::string_view name)
    -> std::optional<std::string> {
    auto target{data::join_path(directory, name)};
    if (!io::exists(target)) {
        return target;
    }
    const auto dot{name.rfind('.')};
    const auto stem{dot == std::string_view::npos || dot == 0U
                        ? name
                        : name.substr(0, dot)};
    const auto extension{dot == std::string_view::npos || dot == 0U
                             ? std::string_view{}
                             : name.substr(dot)};
    for (std::size_t suffix{1}; suffix < 100000U; ++suffix) {
        target = data::join_path(
            directory, fmt::format("{}-{}{}", stem, suffix, extension));
        if (!io::exists(target)) {
            return target;
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto move_package(std::string_view source,
                                std::string_view directory)
    -> result<std::string> {
    if (auto created{io::create_directories(directory)}; !created) {
        return std::unexpected{annotate(
            std::move(created.error()),
            fmt::format("startup-content: inbox directory '{}'", directory))};
    }
    auto target{unique_target(directory, data::filename(source))};
    if (!target) {
        return std::unexpected{
            fail(error_code::io_error,
                 fmt::format("startup-content: cannot find a free name for "
                             "'{}' in '{}'",
                             source, directory))};
    }
    std::error_code error;
    std::filesystem::rename(std::filesystem::path{std::string{source}},
                            std::filesystem::path{*target}, error);
    if (error) {
        return std::unexpected{
            fail(error_code::io_error,
                 fmt::format("startup-content: cannot move '{}' to '{}': {}",
                             source, *target, error.message()))};
    }
    return *target;
}

[[nodiscard]] auto remove_package(std::string_view path) -> status {
    std::error_code error;
    std::filesystem::remove(std::filesystem::path{std::string{path}}, error);
    if (error) {
        return std::unexpected{
            fail(error_code::io_error,
                 fmt::format("startup-content: cannot remove '{}': {}", path,
                             error.message()))};
    }
    return status_ok();
}

/// Count and quarantine one rejected package. Moving is itself a write, so
/// scan-only runs leave the file where it is (and say so).
void reject_package(const startup_content_options &options,
                    std::string_view inbox, std::string_view path,
                    std::string reason, startup_content_report &report,
                    const log_sink &log) {
    ++report.failed;
    emit(log, fmt::format("startup-content: install: rejected {}: {}", path,
                          reason));
    if (!options.auto_install) {
        return;
    }
    auto moved{
        move_package(path, data::join_path(inbox, rejected_directory_name))};
    if (!moved) {
        emit(log, moved.error().message);
    }
}

[[nodiscard]] auto process_package(const startup_content_options &options,
                                   std::string_view inbox,
                                   std::string_view path,
                                   std::optional<data::content_store> &store,
                                   startup_content_report &report,
                                   const log_sink &log) -> status {
    auto reader{rfmod_reader::open_file(path)};
    if (!reader) {
        reject_package(options, inbox, path, reader.error().message, report,
                       log);
        return status_ok();
    }
    auto identity{read_package_identity(*reader, path)};
    if (!identity) {
        reject_package(options, inbox, path, identity.error().message, report,
                       log);
        return status_ok();
    }

    const auto receipt{
        receipt_path(options.store_root, identity->id, identity->version)};
    if (io::exists(receipt)) {
        auto parsed{read_receipt(receipt)};
        if (!parsed) {
            reject_package(options, inbox, path,
                           fmt::format("install receipt '{}' is unreadable: {}",
                                       receipt, parsed.error().message),
                           report, log);
            return status_ok();
        }
        if (parsed->archive_blake3 == reader->archive_digest()) {
            ++report.skipped;
            emit(log, fmt::format(
                          "startup-content: install: {}@{} already installed",
                          identity->id, identity->version));
            return status_ok();
        }
    }

    if (!options.auto_install) {
        ++report.skipped;
        emit(log,
             fmt::format(
                 "startup-content: install: {}@{} skipped auto_install=off",
                 identity->id, identity->version));
        return status_ok();
    }

    if (!store) {
        auto opened{data::content_store::open(options.store_root)};
        if (!opened) {
            return std::unexpected{
                annotate(std::move(opened.error()),
                         fmt::format("startup-content: content store '{}'",
                                     options.store_root))};
        }
        store.emplace(std::move(*opened));
    }

    rfmod_install_options install{};
    install.require_signature = options.require_signature;
    install.trusted_keys = options.trusted_keys;
    install.write_receipt = true;
    install.origin = options.install_origin;
    // The startup bootstrap already holds `<store>/content/.lock`; taking it
    // again inside install_rfmod would wait on ourselves.
    install.lock_store = false;
    auto installed{install_rfmod(*reader, *store, install)};
    if (!installed) {
        reject_package(options, inbox, path, installed.error().message, report,
                       log);
        return status_ok();
    }

    ++report.installed;
    emit(log,
         fmt::format("startup-content: install: installed {}@{} files={}",
                     identity->id, identity->version, installed->files.size()));
    if (options.keep_installed) {
        auto moved{move_package(
            path, data::join_path(inbox, installed_directory_name))};
        if (!moved) {
            emit(log, moved.error().message);
        }
    } else {
        auto removed{remove_package(path)};
        if (!removed) {
            emit(log, removed.error().message);
        }
    }
    return status_ok();
}

/// Sorted regular `*.rfmod` files in the inbox (a missing inbox is empty).
[[nodiscard]] auto scan_packages(const startup_content_options &options)
    -> result<std::vector<std::string>> {
    const auto inbox{inbox_directory(options)};
    if (!io::is_directory(inbox)) {
        return std::vector<std::string>{};
    }
    auto names{io::list_directory(inbox)};
    if (!names) {
        return std::unexpected{
            annotate(std::move(names.error()),
                     fmt::format("startup-content: scan '{}'", inbox))};
    }
    std::vector<std::string> packages;
    for (const auto &name : *names) {
        if (!std::string_view{name}.ends_with(rfmod_extension)) {
            continue;
        }
        const auto path{data::join_path(inbox, name)};
        if (!io::is_regular_file(path)) {
            continue;
        }
        packages.push_back(path);
    }
    return packages;
}

// ---------------------------------------------------------------------------
// Stage 3: resolution index from receipts
// ---------------------------------------------------------------------------

struct receipt_set {
    std::vector<mod_receipt> receipts{};
    std::size_t files{0};
};

/// Read every receipt strictly (a corrupt one fails the whole stage; it is a
/// GC root and the index source, never a best-effort input).
[[nodiscard]] auto load_receipts(std::string_view store_root)
    -> result<receipt_set> {
    auto paths{list_receipt_paths(store_root)};
    if (!paths) {
        return std::unexpected{annotate(
            std::move(paths.error()),
            fmt::format("startup-content: receipts in '{}'", store_root))};
    }
    receipt_set set;
    set.receipts.reserve(paths->size());
    for (const auto &path : *paths) {
        auto receipt{read_receipt(path)};
        if (!receipt) {
            return std::unexpected{annotate(
                std::move(receipt.error()),
                fmt::format("startup-content: receipts in '{}'", store_root))};
        }
        set.files += receipt->files.size();
        set.receipts.push_back(std::move(*receipt));
    }
    return set;
}

/// One version per mod id (highest SemVer; ties broken by the raw version
/// text). Two versions of the same mod cannot both be indexed: their shared
/// locations would be duplicate entries, and the design selects one version
/// (`mods.lock` pin first, highest SemVer otherwise).
[[nodiscard]] auto
select_receipts(const std::vector<mod_receipt> &receipts,
                const std::map<std::string, std::string> &pinned,
                const log_sink &log)
    -> result<std::vector<const mod_receipt *>> {
    std::vector<core::semver> versions;
    versions.reserve(receipts.size());
    for (const auto &receipt : receipts) {
        auto version{core::semver::parse(receipt.version)};
        if (!version) {
            return std::unexpected{
                annotate(std::move(version.error()),
                         fmt::format("startup-content: receipt {}@{}",
                                     receipt.id, receipt.version))};
        }
        versions.push_back(std::move(*version));
    }

    std::map<std::string, std::vector<std::size_t>> by_id;
    for (std::size_t index{0}; index < receipts.size(); ++index) {
        by_id[receipts[index].id].push_back(index);
    }

    std::map<std::string, std::size_t> selected;
    for (const auto &[id, indices] : by_id) {
        const auto pin{pinned.find(id)};
        if (pin != pinned.end()) {
            std::optional<std::size_t> exact{};
            for (const auto index : indices) {
                if (receipts[index].version != pin->second) {
                    continue;
                }
                if (exact.has_value()) {
                    return std::unexpected{
                        fail(error_code::already_exists,
                             fmt::format("startup-content: pinned '{}@{}' has "
                                         "multiple install receipts",
                                         id, pin->second))};
                }
                exact = index;
            }
            if (!exact.has_value()) {
                return std::unexpected{
                    fail(error_code::not_found,
                         fmt::format("startup-content: pinned '{}@{}' is not "
                                     "installed (receipts present: {})",
                                     id, pin->second, indices.size()))};
            }
            selected.emplace(id, *exact);
            continue;
        }
        // Origin is the host-observed tier, so it outranks SemVer: a user
        // update of a first-party mod wins even at the same version. SemVer
        // (then the raw version text) only breaks ties inside one origin.
        const auto better = [&receipts, &versions](std::size_t lhs,
                                                   std::size_t rhs) {
            const auto rank{origin_rank(receipts[lhs].origin)};
            const auto other_rank{origin_rank(receipts[rhs].origin)};
            if (rank != other_rank) {
                return rank > other_rank;
            }
            const auto order{core::compare(versions[lhs], versions[rhs])};
            if (order != std::strong_ordering::equal) {
                return order == std::strong_ordering::greater;
            }
            return receipts[lhs].version > receipts[rhs].version;
        };
        std::size_t best{indices.front()};
        for (const auto index : indices) {
            if (better(index, best)) {
                best = index;
            }
        }
        if (indices.size() > 1U) {
            emit(log,
                 fmt::format(
                     "startup-content: index: {} multiple versions, using {} "
                     "origin={}",
                     id, receipts[best].version,
                     to_string(receipts[best].origin)));
        }
        selected.emplace(id, best);
    }

    // Unlisted layers are appended after the explicit `layer_order`; within
    // that tail a user receipt must still outrank a first-party one. Sorting
    // by (origin rank desc, id) keeps the append order deterministic and
    // independent of receipt file order.
    std::vector<const mod_receipt *> ordered;
    ordered.reserve(selected.size());
    for (const auto &[id, index] : selected) {
        (void)id;
        ordered.push_back(&receipts[index]);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const mod_receipt *lhs, const mod_receipt *rhs) {
                  const auto rank{origin_rank(lhs->origin)};
                  const auto other_rank{origin_rank(rhs->origin)};
                  if (rank != other_rank) {
                      return rank > other_rank;
                  }
                  return lhs->id < rhs->id;
              });
    return ordered;
}

/// One selected receipt plus the priority derived from its layer rank.
struct layer_receipt {
    const mod_receipt *receipt{nullptr};
    std::int32_t priority{0};
};

/// Comma-separated id preview: at most five ids, then "and N more".
[[nodiscard]] auto summarize_ids(const std::vector<std::string> &ids)
    -> std::string {
    constexpr std::size_t preview_limit{5};
    const auto shown{std::min(ids.size(), preview_limit)};
    std::string text;
    for (std::size_t index{0}; index < shown; ++index) {
        if (index != 0U) {
            text += ", ";
        }
        text += ids[index];
    }
    if (ids.size() > shown) {
        text += fmt::format(" and {} more", ids.size() - shown);
    }
    return text;
}

/// Assign one distinct priority per selected receipt from the explicit layer
/// order: listed ids first (in list order), then unlisted selected receipts
/// sorted by id (the `select_receipts()` order). The per-layer line is stable
/// for CI/operators. Listed ids without a receipt and unlisted receipts are
/// reported once, aggregated, because the gamepack also declares builtin mods
/// that are not receipt layers.
[[nodiscard]] auto
assign_layer_priorities(const startup_content_options &options,
                        const std::vector<const mod_receipt *> &selected,
                        const log_sink &log)
    -> result<std::vector<layer_receipt>> {
    if (selected.size() > options.base_priority) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("startup-content: {} selected layer(s) do not fit "
                             "base_priority {}",
                             selected.size(), options.base_priority))};
    }

    std::map<std::string, const mod_receipt *> by_id;
    for (const auto *receipt : selected) {
        by_id.emplace(receipt->id, receipt);
    }

    std::vector<layer_receipt> layers;
    layers.reserve(selected.size());
    std::vector<std::string> listed;
    std::vector<std::string> missing;
    for (const auto &id : options.layer_order) {
        const auto found{by_id.find(id)};
        if (found == by_id.end()) {
            missing.push_back(id);
            continue;
        }
        listed.push_back(id);
        layers.push_back(layer_receipt{
            .receipt = found->second,
            .priority = static_cast<std::int32_t>(
                options.base_priority -
                static_cast<std::uint32_t>(layers.size())),
        });
    }

    std::vector<std::string> appended;
    for (const auto *receipt : selected) {
        if (std::find(listed.begin(), listed.end(), receipt->id) !=
            listed.end()) {
            continue;
        }
        appended.push_back(receipt->id);
        layers.push_back(layer_receipt{
            .receipt = receipt,
            .priority = static_cast<std::int32_t>(
                options.base_priority -
                static_cast<std::uint32_t>(layers.size())),
        });
    }

    if (!missing.empty()) {
        emit(log, fmt::format("startup-content: layer-order: {} declared "
                              "id(s) missing: {}",
                              missing.size(), summarize_ids(missing)));
    }
    if (!appended.empty()) {
        emit(log, fmt::format("startup-content: layer-order: {} appended: {}",
                              appended.size(), summarize_ids(appended)));
    }
    for (const auto &layer : layers) {
        emit(log, fmt::format("startup-content: layer: {}@{} priority={}",
                              layer.receipt->id, layer.receipt->version,
                              layer.priority));
    }
    return layers;
}

[[nodiscard]] auto build_entries(const std::vector<layer_receipt> &layers)
    -> std::vector<data::resolution_input> {
    std::size_t total{0};
    for (const auto &layer : layers) {
        total += layer.receipt->files.size();
    }
    std::vector<data::resolution_input> entries;
    entries.reserve(total);
    for (const auto &layer : layers) {
        for (const auto &file : layer.receipt->files) {
            // The package manifest is install metadata, not a VFS location:
            // publishing it would expose a meaningless root-level
            // `manifest.toml` resource that every mod would shadow. It stays a
            // GC root through the receipt (`extra_roots`).
            if (file.path == rfmod_manifest_path) {
                continue;
            }
            entries.push_back(
                data::resolution_input{.location = file.path,
                                       .source = layer.receipt->id,
                                       .priority = layer.priority,
                                       .blob = file.blake3,
                                       .flags = 0});
        }
    }
    return entries;
}

[[nodiscard]] auto publish_index(const startup_content_options &options,
                                 const receipt_set &receipts,
                                 startup_content_report &report,
                                 const log_sink &log) -> status {
    if (receipts.receipts.empty()) {
        return status_ok();
    }
    auto selected{
        select_receipts(receipts.receipts, options.pinned_versions, log)};
    if (!selected) {
        return std::unexpected{std::move(selected.error())};
    }
    auto layers{assign_layer_priorities(options, *selected, log)};
    if (!layers) {
        return std::unexpected{std::move(layers.error())};
    }
    auto entries{build_entries(*layers)};
    const auto entry_count{entries.size()};
    data::content_publish_options publish{};
    publish.composition = options.composition;
    publish.entries = std::move(entries);
    auto written{data::publish_resolution(options.store_root, publish)};
    if (!written) {
        return std::unexpected{annotate(
            std::move(written.error()),
            fmt::format("startup-content: index '{}'", options.composition))};
    }
    report.index_published = true;
    report.published_entries = entry_count;
    emit(log, fmt::format("startup-content: index: published {} entries={}",
                          data::content_resolution_path(options.store_root,
                                                        options.composition),
                          entry_count));
    return status_ok();
}

// ---------------------------------------------------------------------------
// Stage 4: GC
// ---------------------------------------------------------------------------

[[nodiscard]] auto collect_garbage(const startup_content_options &options,
                                   const receipt_set &receipts,
                                   startup_content_report &report,
                                   const log_sink &log) -> status {
    if (options.gc == startup_content_options::gc_mode::off) {
        emit(log, "startup-content: gc: off");
        return status_ok();
    }
    const auto table_path{data::content_blob_table_path(options.store_root)};
    if (!io::exists(table_path)) {
        emit(log, "startup-content: gc: skipped");
        return status_ok();
    }

    data::gc_options gc{};
    gc.apply = options.gc == startup_content_options::gc_mode::apply;
    // Compaction stays a `content gc --compact` operation: startup must not
    // rewrite segments behind the server's back.
    gc.compact = false;
    gc.keep_days = options.gc_keep_days;
    gc.budget_ms = options.gc_budget_ms;
    for (const auto &receipt : receipts.receipts) {
        for (const auto &file : receipt.files) {
            gc.extra_roots.push_back(file.blake3);
        }
    }
    auto collected{data::collect_garbage(
        options.store_root, gc, [&log](std::string_view line) {
            if (line.find("warning") == std::string_view::npos) {
                return;
            }
            emit(log, fmt::format("startup-content: gc: {}", line));
        })};
    if (!collected) {
        return std::unexpected{
            annotate(std::move(collected.error()), "startup-content: gc")};
    }
    report.gc_planned_blobs = collected->unreferenced_blobs;
    report.gc_planned_segments = collected->orphan_segments;
    report.gc_deleted_blobs = collected->deleted_blobs;
    report.gc_deleted_segments = collected->deleted_segments;
    report.gc_deferred_blobs = collected->deferred_blobs;
    report.gc_deferred_segments = collected->deferred_segments;
    emit(log,
         gc.apply
             ? fmt::format("startup-content: gc: applied blobs={} segments={}",
                           report.gc_deleted_blobs, report.gc_deleted_segments)
             : fmt::format("startup-content: gc: dry run blobs={} segments={}",
                           report.gc_planned_blobs,
                           report.gc_planned_segments));
    return status_ok();
}

// ---------------------------------------------------------------------------
// pipeline
// ---------------------------------------------------------------------------

/// Stages 1-4. `writable` is false only for a read-only store with no requested
/// writes: install moves are skipped (auto_install is already false) and the
/// index is not published; a dry-run GC is still allowed because it writes
/// nothing.
[[nodiscard]] auto run_pipeline(const startup_content_options &options,
                                bool writable, startup_content_report &report,
                                const log_sink &log) -> status {
    const auto inbox{inbox_directory(options)};
    auto packages{scan_packages(options)};
    if (!packages) {
        return std::unexpected{std::move(packages.error())};
    }
    report.scanned = packages->size();

    std::optional<data::content_store> store;
    for (const auto &path : *packages) {
        auto processed{
            process_package(options, inbox, path, store, report, log)};
        if (!processed) {
            return std::unexpected{std::move(processed.error())};
        }
    }
    if (!packages->empty()) {
        emit(log,
             fmt::format(
                 "startup-content: install: installed={} skipped={} failed={}",
                 report.installed, report.skipped, report.failed));
    }

    auto receipts{load_receipts(options.store_root)};
    if (!receipts) {
        return std::unexpected{std::move(receipts.error())};
    }
    report.receipt_files = receipts->files;

    if (writable) {
        if (auto published{publish_index(options, *receipts, report, log)};
            !published) {
            return std::unexpected{std::move(published.error())};
        }
    }

    return collect_garbage(options, *receipts, report, log);
}

} // namespace

auto merge_pinned_versions(const mod_lock *lock,
                           std::span<const mod::mod_ref> overrides)
    -> std::map<std::string, std::string> {
    std::map<std::string, std::string> pins;
    if (lock != nullptr) {
        for (const auto &entry : lock->mods) {
            pins.emplace(entry.id, entry.version);
        }
    }
    for (const auto &override : overrides) {
        if (override.id.empty() || override.version.empty()) {
            continue;
        }
        pins[override.id] = override.version;
    }
    return pins;
}

auto run_startup_content(const startup_content_options &options)
    -> result<startup_content_report> {
    if (auto valid{validate_options(options)}; !valid) {
        return std::unexpected{std::move(valid.error())};
    }
    const log_sink log{options.log};
    startup_content_report report;

    auto lock{data::store_lock::acquire(
        options.store_root, options.lock_timeout_ms,
        [&log](std::string_view line) {
            emit(log, fmt::format("startup-content: {}", line));
        })};
    if (!lock) {
        if (lock.error().code != error_code::permission_denied) {
            return std::unexpected{std::move(lock.error())};
        }
        // A read-only store can still be scanned/reported, but only when the
        // caller did not ask for a write (install or an applying GC).
        const bool writes_requested{
            options.auto_install ||
            options.gc == startup_content_options::gc_mode::apply};
        if (writes_requested) {
            return std::unexpected{std::move(lock.error())};
        }
        report.read_only = true;
        emit(log, "startup-content: lock: read-only");
        if (auto ran{run_pipeline(options, false, report, log)}; !ran) {
            return std::unexpected{std::move(ran.error())};
        }
        return report;
    }

    report.lock_acquired = lock->held();
    emit(log, "startup-content: lock: acquired");
    if (auto ran{run_pipeline(options, true, report, log)}; !ran) {
        return std::unexpected{std::move(ran.error())};
    }
    return report;
}

} // namespace refrain::mod
