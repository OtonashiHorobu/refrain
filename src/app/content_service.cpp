// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/services.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <refrain/archive/manifest.hpp>
#include <refrain/archive/zip.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/cook.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/script/wasm.hpp>
#include <schema/schema.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {

namespace archive = refrain::archive;
namespace config = refrain::core::config;
namespace core = refrain::core;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace log = refrain::log;
namespace mod = refrain::mod;
namespace schema = refrain::schema;
namespace script = refrain::script;

// --- data (datapack content) ------------------------------------------------

[[nodiscard]] auto
mount_data_layers(const data_options &options,
                  std::vector<std::unique_ptr<data::pack>> &owned)
    -> refrain::result<std::vector<const data::pack *>> {
    std::vector<const data::pack *> layers{};
    layers.reserve(options.packs.size());
    for (const auto &path : options.packs) {
        auto pack{path.ends_with(".rfs") ? data::open_rfs_pack(path, path)
                                         : data::open_dir_pack(path, path)};
        if (!pack) {
            return std::unexpected{pack.error()};
        }
        owned.push_back(std::move(*pack));
        layers.push_back(owned.back().get());
    }
    return layers;
}

[[nodiscard]] auto format_kind_counts(const data::kind_counts &counts)
    -> std::string {
    return fmt::format("  recipe: {}\n"
                       "  loot_table: {}\n"
                       "  advancement: {}\n"
                       "  worldgen: {}\n"
                       "  structure: {}\n"
                       "  tags: {}\n"
                       "  other: {}\n",
                       counts.recipe, counts.loot_table, counts.advancement,
                       counts.worldgen, counts.structure, counts.tags,
                       counts.other);
}

auto run_data(const data_options &options, bool validate) -> int {
    if (options.packs.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "data: at least one --pack <datapack dir> is required"));
    }
    std::vector<std::unique_ptr<data::pack>> owned{};
    auto layers{mount_data_layers(options, owned)};
    if (!layers) {
        return report_error(layers.error());
    }
    auto set{data::datapack_set::load(
        std::span<const data::pack *const>{*layers},
        data::datapack_load_options{.expected_pack_format = options.pack_format,
                                    .parse_structures = options.structures})};
    if (!set) {
        return report_error(set.error());
    }
    const auto &stats{set->stats()};
    std::cout << fmt::format(
                     "data: {} layer(s), {} data file(s), {} total file(s), "
                     "{} content entries\n",
                     stats.layers.size(), stats.data_files, stats.total_files,
                     stats.counts.total())
              << format_kind_counts(stats.counts);
    std::cout << fmt::format("  tags: {} tag(s), {} value(s)\n",
                             stats.merged_tag_count, stats.tag_value_count)
              << fmt::format("  other files: {} in {} kind(s)\n",
                             stats.other_files, stats.other_kinds.size())
              << fmt::format("  ignored files: {}\n", stats.ignored_files)
              << fmt::format("  nested datapacks: {} ({} file(s))\n",
                             stats.nested_datapacks,
                             stats.nested_datapack_files)
              << fmt::format("  overrides: {}\n", stats.override_count);
    if (!validate) {
        std::cout << "validate: skipped (dump)\n";
        return 0;
    }
    auto report{set->validate()};
    if (!report.ok()) {
        std::cout << fmt::format("validate: {} error(s), {} warning(s)\n",
                                 report.errors.size(), report.warnings.size());
        for (const auto &error : report.errors) {
            std::cout << "  error: " << error << '\n';
        }
        for (const auto &warning : report.warnings) {
            std::cout << "  warning: " << warning << '\n';
        }
        return 1;
    }
    std::cout << fmt::format("validate: OK ({} warning(s))\n",
                             report.warnings.size());
    for (const auto &warning : report.warnings) {
        std::cout << "  warning: " << warning << '\n';
    }
    return 0;
}

/// One `data registry` kind: the content kind plus the stable registry type
/// name that participates in `registry::fingerprint()` (never a compiler
/// spelling, so fingerprints are reproducible across builds).
struct content_registry_spec {
    data::content_kind kind{};
    std::string_view type_name{};
};

/// Every content kind in enum order. `tags` is a merged `tag_set`, not a set
/// of `content_entry` rows, so its entry only marks where the skipped line is
/// printed.
constexpr std::array content_registry_specs{
    content_registry_spec{data::content_kind::recipe, "refrain:content.recipe"},
    content_registry_spec{data::content_kind::loot_table,
                          "refrain:content.loot_table"},
    content_registry_spec{data::content_kind::advancement,
                          "refrain:content.advancement"},
    content_registry_spec{data::content_kind::worldgen,
                          "refrain:content.worldgen"},
    content_registry_spec{data::content_kind::structure,
                          "refrain:content.structure"},
    content_registry_spec{data::content_kind::tags, ""},
    content_registry_spec{data::content_kind::other, "refrain:content.other"},
};

/// Build, freeze and dump one registry per content kind. This exercises the
/// runtime `registry<T>` path: `data check` validates the parsed set but
/// never assembles registries, so this command exercises add()/freeze() on the
/// same load path and prints the frozen fingerprint (deterministic, including
/// empty kinds).
auto run_data_cook(const data_cook_options &options) -> int {
    if (options.pack.empty() || options.out.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "data cook: --pack <dir> and --out <file> are required"));
    }
    auto cooked{data::cook_directory_rfs(options.pack)};
    if (!cooked) {
        return report_error(cooked.error());
    }
    auto written{core::io::atomic_write_file(options.out, cooked->bytes)};
    if (!written) {
        return report_error(written.error());
    }
    fmt::println("data cook: {} file(s), {} byte(s) -> {}", cooked->files,
                 cooked->bytes.size(), options.out);
    return 0;
}

auto run_data_registry(const data_options &options) -> int {
    if (options.packs.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "data: at least one --pack <datapack dir> is required"));
    }
    std::vector<std::unique_ptr<data::pack>> owned{};
    auto layers{mount_data_layers(options, owned)};
    if (!layers) {
        return report_error(layers.error());
    }
    auto set{data::datapack_set::load(
        std::span<const data::pack *const>{*layers},
        data::datapack_load_options{.expected_pack_format = options.pack_format,
                                    .parse_structures = options.structures})};
    if (!set) {
        return report_error(set.error());
    }

    std::size_t kinds{0};
    std::size_t total{0};
    for (const auto &spec : content_registry_specs) {
        const auto kind_name{data::content_kind_name(spec.kind)};
        if (spec.kind == data::content_kind::tags) {
            std::cout << "registry tags: skipped (merged tag registry; see "
                         "data check)\n";
            continue;
        }
        data::registry<const data::content_entry *> registry{
            std::string{spec.type_name}};
        // `entries()` is already sorted by id, so registration order (and thus
        // the id assignment) is deterministic.
        const auto entries{set->entries(spec.kind)};
        for (const auto *entry : entries) {
            const auto added{registry.add(entry->id, entry)};
            if (!added) {
                return report_error(refrain::make_error(
                    added.error().code,
                    fmt::format("data registry: {} '{}': {}", kind_name,
                                entry->id.to_string(), added.error().message)));
            }
        }
        const auto frozen{registry.freeze()};
        if (!frozen) {
            return report_error(frozen.error());
        }
        std::cout << fmt::format("registry {}: {} entry(s) fingerprint={}\n",
                                 kind_name, registry.size(),
                                 data::to_hex(registry.fingerprint()));
        if (options.list) {
            for (const auto *entry : entries) {
                std::cout << fmt::format("  {} {}\n", kind_name,
                                         entry->id.to_string());
            }
        }
        ++kinds;
        total += registry.size();
    }
    std::cout << fmt::format("data registry: {} kind(s), {} entry(s)\n", kinds,
                             total);
    return 0;
}

// --- content store (CAS TOC indexes) ---------------------------------------

/// `<id>` rendered as the 16 lowercase hex digits used by segment file names.
[[nodiscard]] auto segment_id_text(std::uint64_t id) -> std::string {
    return fmt::format("{:016x}", id);
}

[[nodiscard]] auto wrap_error(refrain::error error, std::string_view context)
    -> refrain::error {
    error.message = std::string{context} + ": " + error.message;
    return error;
}

/// Validate a composition id (also used as a file name) with the same
/// predicate as the runtime open paths (`open_content_pack` /
/// `content_stream::open`), so a name this CLI publishes can always be
/// opened again.
[[nodiscard]] auto validate_content_name(std::string_view value,
                                         std::string_view command,
                                         std::string_view what)
    -> refrain::status {
    if (data::is_valid_content_composition(value)) {
        return refrain::status_ok();
    }
    return std::unexpected{refrain::make_error(
        refrain::error_code::invalid_argument,
        fmt::format("{}: --{} must be a safe file name of at most {} bytes "
                    "([A-Za-z0-9._-], not starting with '.')",
                    command, what, data::max_content_composition_bytes))};
}

[[nodiscard]] auto validate_content_store(std::string_view store,
                                          std::string_view command)
    -> refrain::status {
    if (store.empty()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            std::string{command} + ": --store must not be empty")};
    }
    if (store.find('\0') != std::string_view::npos) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            std::string{command} + ": --store contains a NUL byte")};
    }
    if (!data::is_valid_utf8(store)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            std::string{command} + ": --store is not valid UTF-8")};
    }
    return refrain::status_ok();
}

/// Serialize a content-store writer with every other writer of the same store
/// (`data::store_lock`). Read-only commands never
/// call this. The default timeout matches the server's lock timeout.
[[nodiscard]] auto acquire_store_lock(std::string_view store,
                                      std::string_view command)
    -> refrain::result<data::store_lock> {
    return data::store_lock::acquire(
        store, 5000, [command](std::string_view line) {
            fmt::println(stderr, "{}: {}", command, line);
        });
}

[[nodiscard]] auto content_kind_code(std::string_view name)
    -> refrain::result<std::uint16_t> {
    if (name == "content") {
        return data::blob_kind_content;
    }
    if (name == "cook") {
        return data::blob_kind_cook;
    }
    if (name == "world") {
        return data::blob_kind_world;
    }
    if (name == "builtin") {
        return data::blob_kind_builtin;
    }
    return std::unexpected{refrain::make_error(
        refrain::error_code::invalid_argument,
        "content build: --kind must be content, cook, world or builtin")};
}

[[nodiscard]] auto content_kind_name(std::uint16_t kind) -> std::string_view {
    switch (kind) {
    case data::blob_kind_content:
        return "content";
    case data::blob_kind_cook:
        return "cook";
    case data::blob_kind_world:
        return "world";
    case data::blob_kind_builtin:
        return "builtin";
    default:
        return "unknown";
    }
}

struct content_input_file {
    std::string location{};
    std::string path{};
};

/// Recursively collect regular files under `root` as safe relative locations
/// (`'/'`-separated, UTF-8). Dot entries are skipped; path-unsafe names and
/// symbolic links are an error instead of being silently followed/dropped.
[[nodiscard]] auto collect_content_tree(std::string_view root)
    -> refrain::result<std::vector<content_input_file>> {
    std::vector<content_input_file> files;
    std::vector<std::pair<std::string, std::string>> pending;
    pending.emplace_back(std::string{root}, std::string{});
    while (!pending.empty()) {
        auto [directory, prefix] = std::move(pending.back());
        pending.pop_back();
        auto names{io::list_directory(directory)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            if (name.empty() || name.front() == '.') {
                continue;
            }
            const auto location{data::join_path(prefix, name)};
            const auto path{data::join_path(directory, name)};
            if (!data::is_valid_utf8(name) ||
                !data::is_safe_relative_path(location)) {
                return std::unexpected{refrain::make_error(
                    refrain::error_code::invalid_argument,
                    "content build: unsafe path '" + path + "'")};
            }
            std::error_code link_error;
            const auto link_status{std::filesystem::symlink_status(
                std::filesystem::path{path}, link_error)};
            if (link_error) {
                return std::unexpected{refrain::make_error(
                    refrain::error_code::io_error,
                    "content build: cannot inspect '" + path +
                        "': " + link_error.message())};
            }
            if (std::filesystem::is_symlink(link_status)) {
                return std::unexpected{refrain::make_error(
                    refrain::error_code::invalid_argument,
                    "content build: refusing symbolic link '" + path + "'")};
            }
            if (io::is_directory(path)) {
                pending.emplace_back(path, location);
            } else if (io::is_regular_file(path)) {
                files.push_back(
                    content_input_file{.location = location, .path = path});
            }
        }
    }
    std::sort(files.begin(), files.end(),
              [](const content_input_file &lhs, const content_input_file &rhs) {
                  return lhs.location < rhs.location;
              });
    return files;
}

/// Physical size of a blob, clamped into the RFBT `stored_size` field.
[[nodiscard]] auto blob_stored_size(const data::content_store &store,
                                    const data::digest32 &digest)
    -> refrain::result<std::uint32_t> {
    auto size{io::file_size(store.path_for(digest))};
    if (!size) {
        return std::unexpected{size.error()};
    }
    if (*size > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            "content build: blob " + data::to_hex(digest) +
                " is larger than the 32-bit stored-size field")};
    }
    return static_cast<std::uint32_t>(*size);
}

auto run_content_build(const content_build_options &options) -> int {
    if (auto status{validate_content_store(options.store, "content build")};
        !status) {
        return report_error(status.error());
    }
    if (auto status{validate_content_name(options.composition, "content build",
                                          "composition")};
        !status) {
        return report_error(status.error());
    }
    if (options.source.empty()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "content build: --source must not be empty"));
    }
    auto kind{content_kind_code(options.kind)};
    if (!kind) {
        return report_error(kind.error());
    }
    // Writers of one content store are serialized:
    // without this, a `content build` racing a running server's startup
    // install/publish can interleave `blobs.idx`/RFRX publications.
    auto lock{acquire_store_lock(options.store, "content build")};
    if (!lock) {
        return report_error(lock.error());
    }
    auto store{data::content_store::open(options.store)};
    if (!store) {
        return report_error(store.error());
    }
    auto files{collect_content_tree(options.root)};
    if (!files) {
        return report_error(files.error());
    }

    // The previous table is both the dedupe source and the metadata carried
    // forward for blobs that already exist. It is read
    // before anything is written so a corrupt table fails before publishing.
    const auto table_path{data::content_blob_table_path(options.store)};
    std::optional<data::blob_table_view> previous_table;
    if (io::exists(table_path)) {
        auto loaded{data::load_blob_table(options.store)};
        if (!loaded) {
            return report_error(loaded.error());
        }
        previous_table = std::move(*loaded);
    }
    std::map<data::digest32, data::blob_table_entry> previous_entries;
    if (previous_table) {
        for (const auto &entry : previous_table->entries) {
            previous_entries.emplace(entry.digest, entry);
        }
    }

    // One batch per build. Small raw blobs (<= 64 KiB, including zero-byte
    // files) are appended to content segments; packed blobs keep one file each.
    auto staged{data::batch::begin(options.store,
                                   previous_table ? &*previous_table : nullptr,
                                   data::content_segment_target_bytes)};
    if (!staged) {
        return report_error(staged.error());
    }
    std::vector<data::resolution_input> new_entries;
    new_entries.reserve(files->size());
    std::set<data::digest32> build_blobs;
    for (const auto &file : *files) {
        auto bytes{io::read_file(file.path)};
        if (!bytes) {
            return report_error(bytes.error());
        }
        auto digest{staged->put(*bytes)};
        if (!digest) {
            return report_error(digest.error());
        }
        new_entries.push_back(data::resolution_input{
            .location = file.location,
            .source = options.source,
            .priority = options.priority,
            .blob = *digest,
            .flags = 0,
        });
        build_blobs.insert(*digest);
    }
    if (auto committed{staged->commit()}; !committed) {
        return report_error(committed.error());
    }
    std::map<data::digest32, data::blob_metadata> new_blobs;
    std::set<std::uint64_t> build_segment_ids;
    std::size_t in_segment_blobs{0};
    for (const auto &meta : staged->new_blobs()) {
        new_blobs.emplace(meta.digest, meta);
        if ((meta.flags & data::blob_flags_in_segment) != 0U) {
            build_segment_ids.insert(meta.segment_id);
            ++in_segment_blobs;
        }
    }

    // Load the other compositions with the *old* table (they are bound to it by
    // the blake3 prefix). The composition being rebuilt is replaced, so its old
    // file is skipped. Every index is re-encoded against the new table below,
    // because any refcount change rewrites blobs.idx and therefore every
    // prefix.
    const auto index_path{
        data::content_resolution_path(options.store, options.composition)};
    std::vector<data::resolution_index_file> previous_indexes;
    if (previous_table) {
        auto loaded{data::load_resolution_indexes(options.store,
                                                  *previous_table, index_path)};
        if (!loaded) {
            return report_error(loaded.error());
        }
        previous_indexes = std::move(*loaded);
    } else {
        auto resolution_paths{data::list_resolution_paths(options.store)};
        if (!resolution_paths) {
            return report_error(resolution_paths.error());
        }
        for (const auto &path : *resolution_paths) {
            if (path == index_path) {
                continue;
            }
            return report_error(refrain::make_error(
                refrain::error_code::invalid_argument,
                "content build: existing resolution index '" + path +
                    "' cannot be read without blobs.idx"));
        }
    }

    std::map<data::digest32, std::uint64_t> reference_counts;
    for (const auto &index : previous_indexes) {
        for (const auto &entry : index.view.entries) {
            ++reference_counts[previous_table->entries[entry.blob_index]
                                   .digest];
        }
    }
    for (const auto &entry : new_entries) {
        ++reference_counts[entry.blob];
    }

    // Every previous record is retained, even when its recomputed
    // refcount is 0 (installed content may be protected only by its receipt);
    // GC — not build — decides which unreferenced records to drop. refcount is
    // recomputed from the existing RFRX entries plus this build's new entries,
    // and 0 is a valid value.
    std::map<data::digest32, data::blob_table_entry> table_by_digest;
    for (const auto &[digest, previous] : previous_entries) {
        auto entry{previous};
        entry.refcount = 0;
        table_by_digest.emplace(digest, entry);
    }
    // Batch metadata is authoritative for the physical location of blobs this
    // invocation wrote: raw blobs live in a segment, packed blobs keep a file.
    for (const auto &[digest, meta] : new_blobs) {
        if (table_by_digest.contains(digest)) {
            continue;
        }
        data::blob_table_entry entry{};
        entry.digest = digest;
        entry.logical_size = meta.logical_size;
        entry.flags = meta.flags;
        entry.block_count = (meta.flags & data::blob_flags_packed) != 0U
                                ? static_cast<std::uint32_t>(
                                      (meta.logical_size + 65535U) / 65536U)
                                : 0U;
        entry.stored_size = meta.stored_size;
        entry.segment_id = meta.segment_id;
        entry.segment_offset = meta.segment_offset;
        entry.kind = *kind;
        table_by_digest.emplace(digest, entry);
    }
    for (const auto &[digest, count] : reference_counts) {
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            return report_error(refrain::make_error(
                refrain::error_code::invalid_argument,
                "content build: blob refcount exceeds the 32-bit field"));
        }
        auto found{table_by_digest.find(digest)};
        if (found != table_by_digest.end()) {
            found->second.refcount = static_cast<std::uint32_t>(count);
            continue;
        }
        // Fallback (slow path): metadata for a blob
        // referenced by an older index but absent from the old table.
        // `layout()` loads and re-hashes the standalone physical blob. A blob
        // that only exists inside a segment cannot be reached here (RFBT is
        // the segment's only directory).
        data::blob_table_entry entry{};
        entry.digest = digest;
        entry.refcount = static_cast<std::uint32_t>(count);
        auto layout{store->layout(digest)};
        if (!layout) {
            return report_error(
                wrap_error(layout.error(), "content build: reindexing blob " +
                                               data::to_hex(digest)));
        }
        entry.logical_size = layout->logical_size;
        entry.block_count = layout->block_count;
        if (layout->kind == data::blob_kind::packed) {
            entry.flags = data::blob_flags_packed;
        }
        auto stored{blob_stored_size(*store, digest)};
        if (!stored) {
            return report_error(stored.error());
        }
        entry.stored_size = *stored;
        // No prior classification record exists; use this invocation's kind
        // as the first class.
        entry.kind = *kind;
        table_by_digest.emplace(digest, entry);
    }
    std::vector<data::blob_table_entry> table_entries;
    table_entries.reserve(table_by_digest.size());
    for (const auto &[digest, entry] : table_by_digest) {
        table_entries.push_back(entry);
    }

    auto table_bytes{data::build_blob_table(table_entries)};
    if (!table_bytes) {
        return report_error(table_bytes.error());
    }
    auto table_view{data::parse_blob_table(*table_bytes)};
    if (!table_view) {
        return report_error(table_view.error());
    }
    // Build every replacement RFRX in memory first. If any build fails, no
    // table or index file has been touched yet; the old table/index pair stays
    // readable (the set is still not transactional across files, but no
    // foreseeable half-published state is left behind).
    std::uint64_t total_references{0};
    std::vector<std::pair<std::string, std::vector<std::byte>>> rebuilt_indexes;
    rebuilt_indexes.reserve(previous_indexes.size() + 1U);
    for (const auto &index : previous_indexes) {
        std::vector<data::resolution_input> inputs;
        inputs.reserve(index.view.entries.size());
        for (const auto &entry : index.view.entries) {
            inputs.push_back(data::resolution_input{
                .location = entry.location,
                .source = entry.source,
                .priority = entry.priority,
                .blob = previous_table->entries[entry.blob_index].digest,
                .flags = 0,
            });
            ++total_references;
        }
        auto rebuilt{data::build_resolution_index(inputs, *table_view)};
        if (!rebuilt) {
            return report_error(rebuilt.error());
        }
        rebuilt_indexes.emplace_back(index.path, std::move(*rebuilt));
    }
    auto index_bytes{data::build_resolution_index(new_entries, *table_view)};
    if (!index_bytes) {
        return report_error(index_bytes.error());
    }
    rebuilt_indexes.emplace_back(index_path, std::move(*index_bytes));
    total_references += new_entries.size();

    // Create the resolutions directory before the table write so a directory
    // creation failure leaves the old table/index pair untouched.
    const auto resolutions{data::parent_path(index_path)};
    if (!resolutions.empty()) {
        if (auto directories{io::create_directories(std::string{resolutions})};
            !directories) {
            return report_error(directories.error());
        }
    }
    // Publish the table first, then every RFRX: an index is only readable when
    // its blake3 prefix matches the published table.
    auto table_written{io::atomic_write_file(table_path, *table_bytes)};
    if (!table_written) {
        return report_error(table_written.error());
    }
    for (const auto &[path, bytes] : rebuilt_indexes) {
        auto rewritten{data::publish_resolution_bytes(path, bytes)};
        if (!rewritten) {
            return report_error(rewritten.error());
        }
    }

    std::cout << fmt::format("content build: {} file(s), {} blob(s)\n",
                             files->size(), build_blobs.size());
    std::cout << "  resolution: " << index_path << " (" << new_entries.size()
              << " entry(s))\n";
    std::cout << fmt::format("  blobs: {} blob(s), {} reference(s)\n",
                             table_entries.size(), total_references);
    std::cout << fmt::format("  segments: {} segment(s), {} blob(s)\n",
                             build_segment_ids.size(), in_segment_blobs);
    return 0;
}

auto run_content_dump(const content_dump_options &options) -> int {
    if (auto status{validate_content_store(options.store, "content dump")};
        !status) {
        return report_error(status.error());
    }
    auto table{data::load_blob_table(options.store)};
    if (!table) {
        return report_error(table.error());
    }
    std::uint64_t references{0};
    std::array<bool, 4> kinds{};
    for (const auto &entry : table->entries) {
        references += entry.refcount;
        if (entry.kind < kinds.size()) {
            kinds[entry.kind] = true;
        }
    }
    std::string kind_list;
    for (std::size_t index{0}; index < kinds.size(); ++index) {
        if (!kinds[index]) {
            continue;
        }
        if (!kind_list.empty()) {
            kind_list.push_back(',');
        }
        kind_list += content_kind_name(static_cast<std::uint16_t>(index));
    }
    if (kind_list.empty()) {
        kind_list = "none";
    }
    auto segments{data::scan_content_segments(options.store)};
    if (!segments) {
        return report_error(segments.error());
    }
    std::cout << fmt::format(
        "content: {} blob(s), {} reference(s), kind {}, {} segment(s)\n",
        table->entries.size(), references, kind_list, segments->size());
    const auto shown{std::min<std::size_t>(table->entries.size(), 20U)};
    for (std::size_t index{0}; index < shown; ++index) {
        const auto &entry{table->entries[index]};
        const auto location{(entry.flags & data::blob_flags_in_segment) != 0U
                                ? fmt::format("segment={}:{}",
                                              segment_id_text(entry.segment_id),
                                              entry.segment_offset)
                                : std::string{"file"}};
        std::cout << fmt::format(
            "  {} size={} flags=0x{:x} refcount={} kind={} blocks={} "
            "stored={} {}\n",
            data::to_hex(entry.digest).substr(0, 16), entry.logical_size,
            entry.flags, entry.refcount, content_kind_name(entry.kind),
            entry.block_count, entry.stored_size, location);
    }
    return 0;
}

auto run_content_where(const content_where_options &options) -> int {
    if (auto status{validate_content_store(options.store, "content where")};
        !status) {
        return report_error(status.error());
    }
    if (auto status{validate_content_name(options.composition, "content where",
                                          "composition")};
        !status) {
        return report_error(status.error());
    }
    auto table{data::load_blob_table(options.store)};
    if (!table) {
        return report_error(table.error());
    }
    const auto path{
        data::content_resolution_path(options.store, options.composition)};
    auto bytes{io::read_file(path)};
    if (!bytes) {
        return report_error(wrap_error(std::move(bytes.error()), path));
    }
    auto parsed{data::parse_resolution_index(*bytes, *table)};
    if (!parsed) {
        return report_error(wrap_error(std::move(parsed.error()), path));
    }
    auto resolved{data::resolve(*parsed, options.location)};
    if (!resolved) {
        return report_error(wrap_error(std::move(resolved.error()),
                                       "content where: composition '" +
                                           options.composition + "'"));
    }
    // Read-only diagnostic: open the existing store without creating
    // `<store>/content/blobs` (which the write-side `open()` would do).
    auto store{data::content_store::open_existing(options.store)};
    if (!store) {
        return report_error(store.error());
    }
    // The diagnostic must say whether the winner (and
    // each fallback) actually resolves to readable bytes, not just to a
    // digest. A missing file/segment is reported as `exists=no` instead of
    // failing the command: `content where` is a diagnostic.
    const auto blob_exists = [&](const data::resolution_entry &entry) -> bool {
        if (entry.blob_index >= table->entries.size()) {
            return false;
        }
        const auto location{data::blob_location_from_table_entry(
            table->entries[entry.blob_index])};
        if (!location.in_segment) {
            return io::exists(store->path_for(location.digest));
        }
        auto reader{data::segment_reader::open(
            data::segment_path(options.store, location.segment_id))};
        if (!reader) {
            return false;
        }
        const auto &header{(*reader).header()};
        return static_cast<std::uint64_t>(location.segment_offset) <=
                   header.data_bytes &&
               static_cast<std::uint64_t>(location.stored_size) <=
                   header.data_bytes -
                       static_cast<std::uint64_t>(location.segment_offset);
    };
    auto winner_blob{data::resolved_blob(*resolved, *table)};
    if (!winner_blob) {
        return report_error(winner_blob.error());
    }
    std::cout << fmt::format(
        "winner: {} blob={} source={} priority={} exists={}\n",
        options.location, data::to_hex(*winner_blob), resolved->winner->source,
        resolved->winner->priority,
        blob_exists(*resolved->winner) ? "yes" : "no");
    for (const auto *fallback : resolved->fallback) {
        auto fallback_blob{data::blob_digest(*fallback, *table)};
        if (!fallback_blob) {
            return report_error(fallback_blob.error());
        }
        std::cout << fmt::format(
            "fallback: blob={} source={} priority={} exists={}\n",
            data::to_hex(*fallback_blob), fallback->source, fallback->priority,
            blob_exists(*fallback) ? "yes" : "no");
    }
    return 0;
}

auto run_content_verify(const content_verify_options &options) -> int {
    if (auto status{validate_content_store(options.store, "content verify")};
        !status) {
        return report_error(status.error());
    }
    auto table{data::load_blob_table(options.store)};
    if (!table) {
        return report_error(table.error());
    }
    std::uint64_t references{0};
    for (const auto &entry : table->entries) {
        references += entry.refcount;
    }
    std::cout << fmt::format(
        "verify: blobs.idx OK ({} blob(s), {} reference(s))\n",
        table->entries.size(), references);

    auto indexes{data::load_resolution_indexes(options.store, *table)};
    if (!indexes) {
        return report_error(indexes.error());
    }
    std::uint64_t total_entries{0};
    for (const auto &index : *indexes) {
        std::cout << fmt::format("verify: {} OK ({} entry(s))\n",
                                 data::filename(index.path),
                                 index.view.entries.size());
        total_entries += index.view.entries.size();
    }
    if (indexes->empty()) {
        std::cout << "verify: no resolution indexes\n";
    } else {
        std::cout << fmt::format("verify: {} resolution entry(s) checked\n",
                                 total_entries);
    }

    if (options.blobs) {
        auto store{data::content_store::open(options.store)};
        if (!store) {
            return report_error(store.error());
        }
        // Group the in-segment records by segment so each segment file is
        // opened/hashed once; the RFBT is a segment's only directory.
        std::map<std::uint64_t, std::vector<data::segment_blob_span>>
            segment_spans;
        std::set<std::uint64_t> referenced_segments;
        for (const auto &entry : table->entries) {
            const auto location{data::blob_location_from_table_entry(entry)};
            auto bytes{store->read(location)};
            if (!bytes) {
                return report_error(
                    wrap_error(bytes.error(), "content verify: blob " +
                                                  data::to_hex(entry.digest)));
            }
            // `read()` decodes every packed block and checks the logical
            // digest; `layout()` additionally re-hashes each block, which is
            // what `content verify --blobs` promises.
            if (!location.in_segment &&
                (entry.flags & data::blob_flags_packed) != 0U) {
                auto layout{store->layout(entry.digest)};
                if (!layout) {
                    return report_error(wrap_error(
                        layout.error(), "content verify: packed blob " +
                                            data::to_hex(entry.digest)));
                }
            }
            if (location.in_segment) {
                referenced_segments.insert(entry.segment_id);
                segment_spans[entry.segment_id].push_back(
                    data::segment_blob_span{.offset = entry.segment_offset,
                                            .size = entry.stored_size});
            }
        }
        for (auto &[id, spans] : segment_spans) {
            std::sort(spans.begin(), spans.end(),
                      [](const data::segment_blob_span &lhs,
                         const data::segment_blob_span &rhs) {
                          if (lhs.offset != rhs.offset) {
                              return lhs.offset < rhs.offset;
                          }
                          return lhs.size < rhs.size;
                      });
            const auto path{data::segment_path(options.store, id)};
            auto reader{data::segment_reader::open(path)};
            if (!reader) {
                return report_error(
                    wrap_error(reader.error(), "content verify: segment " +
                                                   segment_id_text(id)));
            }
            const auto expected{reader->header().blob_count};
            const auto data_bytes{reader->header().data_bytes};
            // `verify(spans)` checks spans only when the count matches the
            // header, so validate the referenced boundaries unconditionally
            // (also covers segments whose dead blobs were already dropped).
            for (const auto &span : spans) {
                if (span.offset % data::content_segment_alignment != 0U ||
                    static_cast<std::uint64_t>(span.offset) > data_bytes ||
                    static_cast<std::uint64_t>(span.size) >
                        data_bytes - static_cast<std::uint64_t>(span.offset)) {
                    return report_error(refrain::make_error(
                        refrain::error_code::parse_error,
                        fmt::format("content verify: segment {}: blob span "
                                    "{}+{} is not an aligned in-body range "
                                    "({} byte(s))",
                                    segment_id_text(id), span.offset, span.size,
                                    data_bytes)));
                }
            }
            if (spans.size() > expected) {
                return report_error(refrain::make_error(
                    refrain::error_code::parse_error,
                    fmt::format("content verify: segment {} has {} table "
                                "record(s) but its header declares {} blob(s)",
                                segment_id_text(id), spans.size(), expected)));
            }
            if (spans.size() == expected) {
                if (auto verified{reader->verify(spans)}; !verified) {
                    return report_error(wrap_error(verified.error(),
                                                   "content verify: segment " +
                                                       segment_id_text(id)));
                }
                std::cout << fmt::format("verify: segment {} OK ({} blob(s))\n",
                                         segment_id_text(id), spans.size());
            } else {
                // The table may no longer list every blob of an older segment
                // (GC drops zero-reference records even below the compaction
                // threshold). The body hash still has to match; the referenced
                // spans were bounds-checked above.
                if (auto verified{reader->verify()}; !verified) {
                    return report_error(wrap_error(verified.error(),
                                                   "content verify: segment " +
                                                       segment_id_text(id)));
                }
                std::cout << fmt::format(
                    "verify: segment {} OK ({} blob(s) referenced, {} "
                    "unreferenced)\n",
                    segment_id_text(id), spans.size(),
                    expected - static_cast<std::uint32_t>(spans.size()));
            }
        }
        std::cout << fmt::format("verify: {} blob(s) content OK\n",
                                 table->entries.size());
        std::cout << fmt::format("verify: {} segment(s) OK\n",
                                 segment_spans.size());

        auto segments{data::scan_content_segments(options.store)};
        if (!segments) {
            return report_error(segments.error());
        }
        std::size_t orphan_segments{0};
        std::uint64_t orphan_bytes{0};
        for (const auto &segment : *segments) {
            if (referenced_segments.contains(segment.id)) {
                continue;
            }
            ++orphan_segments;
            orphan_bytes += segment.file_size;
        }
        std::cout << fmt::format("verify: {} orphan segment(s), {} byte(s)\n",
                                 orphan_segments, orphan_bytes);
        if (options.strict && orphan_segments != 0U) {
            return report_error(refrain::make_error(
                refrain::error_code::parse_error,
                fmt::format("content verify: {} orphan segment(s) present "
                            "(--strict)",
                            orphan_segments)));
        }
    }
    std::cout << "content verify: OK\n";
    return 0;
}

/// GC roots supplied by the caller: every installed-mod receipt's
/// `files[].blake3` digest (one vector element per file, duplicates included).
/// Receipts are a mod-layer concept, so `data::collect_garbage` only receives
/// the resulting digests.
[[nodiscard]] auto load_receipt_roots(std::string_view store)
    -> refrain::result<std::vector<data::digest32>> {
    std::vector<data::digest32> roots;
    auto paths{mod::list_receipt_paths(store)};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    for (const auto &path : *paths) {
        auto receipt{mod::read_receipt(path)};
        if (!receipt) {
            return std::unexpected{receipt.error()};
        }
        for (const auto &file : receipt->files) {
            roots.push_back(file.blake3);
        }
    }
    return roots;
}

auto run_content_gc(const content_gc_options &options) -> int {
    if (auto status{validate_content_store(options.store, "content gc")};
        !status) {
        return report_error(status.error());
    }
    if (options.dry_run && options.apply) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "content gc: --dry-run and --apply are mutually exclusive"));
    }
    if (options.keep_days < 0) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "content gc: --keep-days must not be negative"));
    }
    if (options.keep_days > data::max_gc_keep_days) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            fmt::format("content gc: --keep-days {} exceeds the maximum of {}",
                        options.keep_days, data::max_gc_keep_days)));
    }
    // Default (no flag) is the safe dry-run.
    const bool apply{options.apply && !options.dry_run};

    // `--apply` is a writer: take the same store lock as startup/build so a
    // running server cannot publish roots between our mark and sweep. Dry runs
    // only read.
    //
    // The lock is acquired *before* the receipt roots are snapshotted: a
    // concurrent mod install/remove that holds the lock would otherwise be
    // able to change the receipts after our snapshot and before the sweep,
    // making live blobs look unreferenced.
    std::optional<data::store_lock> lock;
    if (apply) {
        auto acquired{acquire_store_lock(options.store, "content gc")};
        if (!acquired) {
            return report_error(acquired.error());
        }
        lock.emplace(std::move(*acquired));
    }

    // The root set is completed before the library runs: a corrupt receipt
    // fails closed here, before any table or blob is touched. Reading
    // the receipts under the lock makes the snapshot consistent with the
    // writers that share it.
    auto extra_roots{load_receipt_roots(options.store)};
    if (!extra_roots) {
        return report_error(extra_roots.error());
    }
    data::gc_options gc{};
    gc.apply = apply;
    gc.compact = options.compact;
    gc.keep_days = options.keep_days;
    gc.budget_ms = options.budget_ms;
    gc.extra_roots = std::move(*extra_roots);

    // The library emits the stable report lines without the `gc: ` prefix; the
    // CLI only adds that prefix and a newline.
    auto report{
        data::collect_garbage(options.store, gc, [](std::string_view line) {
            std::cout << "gc: " << line << '\n';
        })};
    if (!report) {
        return report_error(report.error());
    }
    // A budgeted run reports its deferred counters once more as a single
    // stable line (the per-kind `deferred ...` lines come from the library).
    if (report->deferred_blobs != 0U || report->deferred_segments != 0U) {
        std::cout << "gc: deferred " << report->deferred_blobs
                  << " blobs totaling " << report->deferred_blob_bytes
                  << " bytes and " << report->deferred_segments
                  << " segments totaling " << report->deferred_segment_bytes
                  << " bytes\n";
    }
    return 0;
}

// --- builtin content image (RFSI v1) ---------------------------------------

/// Index of every mod in the image (`mods.index.toml`), written from the same
/// `mod::scan_mods_tree()` catalog the host/discovery use (no second parser).
[[nodiscard]] auto build_builtin_index(std::span<const mod::candidate> mods)
    -> std::string {
    auto out{std::string{"# Generated by 'refrain-tools builtin image'. "
                         "Deterministic; do not edit.\n"}};
    out += "[archive]\nformat = \"rfsi\"\nversion = 1\n\n";
    for (const auto &mod : mods) {
        out += fmt::format("[mods.\"{}\"]\n", mod.id);
        out += fmt::format("version = \"{}\"\n", mod.version);
        out += fmt::format("abi = {}\n", mod.abi);
        out += fmt::format("side = \"{}\"\n", mod::to_string(mod.side));
        if (mod.is_external()) {
            out += "entry = \"external\"\n";
        } else {
            out += "entry = \"internal\"\n";
        }
        out += fmt::format("manifest = \"mods/{}/manifest.toml\"\n\n", mod.id);
    }
    return out;
}

// --- builtin image (RFSI v1) ----------------------------------------------

[[nodiscard]] auto emit_builtin_image_cpp(std::span<const std::byte> bytes,
                                          std::string_view digest_hex)
    -> std::string {
    auto out{std::string{
        "// SPDX-License-Identifier: MIT\n"
        "//\n"
        "// GENERATED FILE - DO NOT EDIT.\n"
        "// Generated by: refrain-tools builtin image --emit-cpp\n"
        "// Source of truth: mods/<id>/manifest.toml + data/assets/scripts\n"
        "// One embedded RFSI v1 builtin image: RFBT + RFRX +\n"
        "// content segments + standalone blobs.\n"
        "// Image BLAKE3: "}};
    out += digest_hex;
    out += "\n// Image size: " + std::to_string(bytes.size()) + " byte(s)\n\n";
    out += "#include <refrain/builtin/image.hpp>\n"
           "\n"
           "#include <array>\n"
           "#include <cstddef>\n"
           "#include <string_view>\n"
           "\n"
           "namespace refrain::builtin {\n"
           "\n"
           "alignas(64) constexpr std::array<std::byte, " +
           std::to_string(bytes.size()) + "> k_builtin_image{\n";
    constexpr std::size_t per_line{12};
    for (std::size_t index{0}; index < bytes.size(); ++index) {
        if (index % per_line == 0) {
            out += "    ";
        }
        out += fmt::format(
            "std::byte{{0x{:02x}}},",
            static_cast<unsigned>(std::to_integer<int>(bytes[index])));
        if (index % per_line == per_line - 1 || index + 1 == bytes.size()) {
            out += '\n';
        }
    }
    out += "};\n\n";
    out +=
        "constexpr std::size_t builtin_image_size{k_builtin_image.size()};\n";
    out += "constexpr std::string_view builtin_image_blake3{\"";
    out += digest_hex;
    out += "\"};\n\n";
    out += "auto image_data() noexcept -> const std::byte * {\n"
           "    return k_builtin_image.data();\n"
           "}\n\n"
           "auto image_size() noexcept -> std::size_t {\n"
           "    return k_builtin_image.size();\n"
           "}\n\n"
           "auto image_digest() noexcept -> std::string_view {\n"
           "    return builtin_image_blake3;\n"
           "}\n\n";
    out += "} // namespace refrain::builtin\n";
    return out;
}

auto run_builtin_image(const builtin_image_options &options) -> int {
    if (options.out.empty()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "builtin image: --out is required"));
    }
    auto manifests{mod::scan_mods_tree(options.mods_dir)};
    if (!manifests) {
        return report_error(manifests.error());
    }

    // Mod tree plus mods.index.toml. The file walker is the one collector
    // shared with `mods pack`; only the archive path prefix differs.
    constexpr std::array<std::string_view, 1> excluded_roots{"src"};
    std::vector<data::builtin_image_input> inputs;
    for (const auto &candidate : *manifests) {
        const auto mod_root{
            fmt::format("{}/{}", options.mods_dir, candidate.id)};
        auto files{mod::collect_package_files(
            mod_root,
            mod::package_collect_options{.error_prefix = "builtin image",
                                         .exclude_roots = excluded_roots,
                                         .include_manifest = true})};
        if (!files) {
            return report_error(files.error());
        }
        for (auto &file : *files) {
            inputs.push_back(data::builtin_image_input{
                .path = fmt::format("mods/{}/{}", candidate.id, file.path),
                .data = std::move(file.data)});
        }
    }
    std::sort(inputs.begin(), inputs.end(),
              [](const data::builtin_image_input &lhs,
                 const data::builtin_image_input &rhs) {
                  return lhs.path < rhs.path;
              });
    const auto index_text{build_builtin_index(*manifests)};
    std::vector<std::byte> index_bytes(index_text.size());
    std::memcpy(index_bytes.data(), index_text.data(), index_text.size());
    inputs.push_back(data::builtin_image_input{.path = "mods.index.toml",
                                               .data = std::move(index_bytes)});

    auto bytes{data::build_builtin_image(inputs)};
    if (!bytes) {
        return report_error(bytes.error());
    }
    const std::span<const std::byte> image_bytes{bytes->data(), bytes->size()};
    auto opened{data::builtin_image::open(image_bytes)};
    if (!opened) {
        return report_error(opened.error());
    }
    const auto digest{data::to_hex(opened->digest())};
    const auto file_count{opened->file_count()};

    auto written{io::atomic_write_file(options.out, image_bytes)};
    if (!written) {
        return report_error(written.error());
    }
    if (!options.emit_cpp.empty()) {
        auto cpp{emit_builtin_image_cpp(image_bytes, digest)};
        auto cpp_written{io::atomic_write_text(options.emit_cpp, cpp)};
        if (!cpp_written) {
            return report_error(cpp_written.error());
        }
    }
    std::cout << "builtin image: " << file_count << " file(s), "
              << bytes->size() << " byte(s), BLAKE3 " << digest << " -> "
              << options.out << '\n';
    return 0;
}

} // namespace refrain::app
