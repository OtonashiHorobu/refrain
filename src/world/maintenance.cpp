// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Quiesced in-place palette maintenance.

#include <refrain/world/maintenance.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/platform/random_access_file.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/rwf.hpp>
#include <refrain/world/store.hpp>

#include <fmt/format.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace refrain::world {
namespace {

namespace io = core::io;
namespace fs = std::filesystem;
namespace file = platform::file;

using data::join_path;

/// Marker schema (`refrain.palette-maintenance/1`). A different value means a
/// newer/foreign writer; recovery fails closed instead of guessing.
constexpr std::string_view marker_schema{"refrain.palette-maintenance/1"};

/// Marker size cap; the document is six short fields.
constexpr std::uint64_t max_marker_bytes{64ULL * 1024ULL};

/// Deterministic scratch attempts tried by a rebuild. Recovery accepts the same
/// range; a future protocol that needs more must bump the marker schema.
constexpr unsigned max_scratch_attempts{16U};

/// Depth cap for the fsync walk. A world directory is shallow (root,
/// dimensions/<ns>/<dim>/{regions,blobs,objects}, ...).
constexpr unsigned max_sync_depth{16U};

enum class maintenance_phase : std::uint8_t { planned, rebuilding, swapping };

enum class path_kind : std::uint8_t { absent, directory, other };

[[nodiscard]] auto failing(error_code code, std::string_view message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, message)};
}

[[nodiscard]] auto wrap(error failure, std::string_view context) -> error {
    failure.message = fmt::format("{}: {}", context, failure.message);
    return failure;
}

[[nodiscard]] auto combine(error primary, const error &cleanup) -> error {
    primary.message = fmt::format("{}; cleanup also failed: {}",
                                  primary.message, cleanup.message);
    return primary;
}

[[nodiscard]] auto phase_name(maintenance_phase value) -> std::string_view {
    switch (value) {
    case maintenance_phase::planned:
        return "planned";
    case maintenance_phase::rebuilding:
        return "rebuilding";
    case maintenance_phase::swapping:
        return "swapping";
    }
    return "planned";
}

[[nodiscard]] auto parse_phase(std::string_view text)
    -> std::optional<maintenance_phase> {
    if (text == "planned") {
        return maintenance_phase::planned;
    }
    if (text == "rebuilding") {
        return maintenance_phase::rebuilding;
    }
    if (text == "swapping") {
        return maintenance_phase::swapping;
    }
    return std::nullopt;
}

/// Absolute, lexically normal root plus the derived sibling paths. The root is
/// not symlink-resolved: maintenance refuses a symlinked world root, and
/// comparing different spellings of the same directory is enough here (the
/// world must be closed; cross-process aliasing is unsupported).
struct world_paths {
    std::string root;
    std::string parent;
    std::string name;
    std::string marker;
};

[[nodiscard]] auto describe_root(std::string_view world_root)
    -> result<world_paths> {
    if (world_root.empty()) {
        return failing(error_code::invalid_argument, "world path is empty");
    }
    if (world_root == "." || world_root == "..") {
        return failing(
            error_code::invalid_argument,
            fmt::format("'{}' is not a maintenance world root", world_root));
    }
    const fs::path input{std::string{world_root}};
    std::error_code error;
    fs::path absolute{fs::absolute(input, error)};
    if (error) {
        error.clear();
        absolute = input;
    }
    absolute = absolute.lexically_normal();
    if (!absolute.has_filename()) {
        absolute = absolute.parent_path();
    }
    if (!absolute.has_filename()) {
        return failing(
            error_code::invalid_argument,
            fmt::format("'{}' is not a world directory path", world_root));
    }
    world_paths paths{};
    paths.root = absolute.string();
    const auto parent{absolute.parent_path()};
    paths.parent = parent.empty() ? std::string{"."} : parent.string();
    paths.name = absolute.filename().string();
    paths.marker = fmt::format("{}.palette-maintenance.json", paths.root);
    return paths;
}

[[nodiscard]] auto scratch_path(const world_paths &paths, unsigned attempt,
                                std::string_view suffix) -> std::string {
    const auto name{fmt::format(".{}.palette-maintenance-{}-{}.{}", paths.name,
                                platform::process_id(), attempt, suffix)};
    return (fs::path{paths.parent} / name).string();
}

struct marker_record {
    maintenance_phase phase{maintenance_phase::planned};
    std::uint64_t pid{0};
    std::string world;
    std::string temp;
    std::string backup;
};

[[nodiscard]] auto marker_text(const marker_record &record) -> std::string {
    data::json::value::object_type object;
    object.emplace("backup", data::json::value{record.backup});
    object.emplace("phase",
                   data::json::value{std::string{phase_name(record.phase)}});
    object.emplace("pid",
                   data::json::value{static_cast<std::int64_t>(record.pid)});
    object.emplace("schema", data::json::value{std::string{marker_schema}});
    object.emplace("temp", data::json::value{record.temp});
    object.emplace("world", data::json::value{record.world});
    return fmt::format("{}\n",
                       data::json::value{std::move(object)}.dump_pretty(2));
}

[[nodiscard]] auto write_marker(const world_paths &paths,
                                const marker_record &record) -> status {
    if (auto written{io::atomic_write_text(paths.marker, marker_text(record))};
        !written) {
        return std::unexpected{
            wrap(written.error(), "cannot write palette maintenance marker")};
    }
    // atomic_write fsyncs the file and renames it; flush the directory entry so
    // the marker survives a power loss before the swap starts.
    return file::sync_directory(paths.parent);
}

struct scratch_identity {
    std::uint64_t pid{0};
    unsigned attempt{0};
};

[[nodiscard]] auto parse_unsigned(std::string_view text)
    -> std::optional<std::uint64_t> {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value{0};
    const auto parsed{
        std::from_chars(text.data(), text.data() + text.size(), value)};
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// Validate that `candidate` is one of our sibling scratch paths and return the
/// pid/attempt encoded in its name. Anything else (absolute path, different
/// parent, foreign name) is a corrupt/foreign marker.
[[nodiscard]] auto parse_scratch(const world_paths &paths,
                                 std::string_view candidate,
                                 std::string_view suffix)
    -> result<scratch_identity> {
    const fs::path path{std::string{candidate}};
    if (!path.is_absolute()) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance scratch path '{}' is "
                                   "not absolute",
                                   candidate));
    }
    const auto normalized{path.lexically_normal()};
    if (normalized.parent_path().string() != paths.parent) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance scratch path '{}' is "
                                   "not a sibling of the world",
                                   candidate));
    }
    const auto filename{normalized.filename().string()};
    const auto prefix{fmt::format(".{}.palette-maintenance-", paths.name)};
    const auto ending{fmt::format(".{}", suffix)};
    if (!filename.starts_with(prefix) || !filename.ends_with(ending)) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance scratch path '{}' has "
                                   "a foreign name",
                                   candidate));
    }
    auto middle{std::string_view{filename}.substr(prefix.size())};
    middle.remove_suffix(ending.size());
    const auto dash{middle.find('-')};
    if (dash == std::string_view::npos || dash == 0) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance scratch path '{}' has "
                                   "no pid/attempt suffix",
                                   candidate));
    }
    const auto pid{parse_unsigned(middle.substr(0, dash))};
    const auto attempt{parse_unsigned(middle.substr(dash + 1))};
    if (!pid || !attempt || *attempt >= max_scratch_attempts) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance scratch path '{}' has "
                                   "an invalid pid/attempt suffix",
                                   candidate));
    }
    return scratch_identity{.pid = *pid,
                            .attempt = static_cast<unsigned>(*attempt)};
}

/// Parse and fully validate `<root>.palette-maintenance.json`.
/// `std::nullopt` means "no marker" (a successful no-op for recovery).
[[nodiscard]] auto load_marker(const world_paths &paths)
    -> result<std::optional<marker_record>> {
    auto status{io::status_at(paths.marker)};
    if (!status) {
        return std::unexpected{
            wrap(status.error(), "cannot inspect palette maintenance marker")};
    }
    if (!status->exists && !status->symlink) {
        return std::optional<marker_record>{};
    }
    if (status->symlink || !status->regular) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' is not a "
                                   "regular file",
                                   paths.marker));
    }
    auto text{io::read_text(
        paths.marker, {.max_bytes = max_marker_bytes, .allow_symlink = false})};
    if (!text) {
        if (text.error().code == error_code::invalid_argument) {
            // Over the size cap (or otherwise rejected as input): the marker is
            // corrupt, not an I/O problem.
            return failing(error_code::parse_error,
                           fmt::format("palette maintenance marker '{}' "
                                       "exceeds {} bytes",
                                       paths.marker, max_marker_bytes));
        }
        return std::unexpected{
            wrap(text.error(), "cannot read palette maintenance marker")};
    }
    auto document{data::json::value::parse(*text)};
    if (!document) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' is not "
                                   "valid JSON",
                                   paths.marker));
    }
    auto object{document->as_object()};
    if (!object) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' is not a "
                                   "JSON object",
                                   paths.marker));
    }
    const auto &members{object->get()};
    if (members.size() != 6U) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' has "
                                   "unexpected members",
                                   paths.marker));
    }
    const auto member =
        [&document](std::string_view key) -> const data::json::value * {
        return document->find(key);
    };
    const auto string_member =
        [&](std::string_view key) -> result<std::string_view> {
        const auto *value{member(key)};
        if (value == nullptr) {
            return failing(error_code::parse_error,
                           fmt::format("palette maintenance marker is missing "
                                       "'{}'",
                                       key));
        }
        auto text_value{value->as_string()};
        if (!text_value || text_value->empty()) {
            return failing(error_code::parse_error,
                           fmt::format("palette maintenance marker '{}' must "
                                       "be a non-empty string",
                                       key));
        }
        return *text_value;
    };

    const auto schema{string_member("schema")};
    if (!schema) {
        return std::unexpected{schema.error()};
    }
    if (*schema != marker_schema) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' has "
                                   "unsupported schema '{}'",
                                   paths.marker, *schema));
    }
    const auto phase_text{string_member("phase")};
    if (!phase_text) {
        return std::unexpected{phase_text.error()};
    }
    const auto phase{parse_phase(*phase_text)};
    if (!phase) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' has "
                                   "unknown phase '{}'",
                                   paths.marker, *phase_text));
    }
    const auto world_text{string_member("world")};
    if (!world_text) {
        return std::unexpected{world_text.error()};
    }
    if (*world_text != paths.root) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' belongs to "
                                   "another world ('{}')",
                                   paths.marker, *world_text));
    }
    const auto temp_text{string_member("temp")};
    if (!temp_text) {
        return std::unexpected{temp_text.error()};
    }
    const auto backup_text{string_member("backup")};
    if (!backup_text) {
        return std::unexpected{backup_text.error()};
    }
    const auto *pid_value{member("pid")};
    if (pid_value == nullptr) {
        return failing(error_code::parse_error,
                       "palette maintenance marker is missing 'pid'");
    }
    const auto pid{pid_value->as_int64()};
    if (!pid || *pid <= 0) {
        return failing(error_code::parse_error,
                       "palette maintenance marker 'pid' must be a positive "
                       "integer");
    }
    const auto marker_pid{static_cast<std::uint64_t>(*pid)};
    const auto temp{parse_scratch(paths, *temp_text, "tmp")};
    if (!temp) {
        return std::unexpected{temp.error()};
    }
    const auto backup{parse_scratch(paths, *backup_text, "bak")};
    if (!backup) {
        return std::unexpected{backup.error()};
    }
    if (temp->pid != marker_pid || backup->pid != marker_pid) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' pid does "
                                   "not match its scratch paths",
                                   paths.marker));
    }
    if (temp->attempt != backup->attempt) {
        return failing(error_code::parse_error,
                       fmt::format("palette maintenance marker '{}' has "
                                   "mismatched temp/backup attempts",
                                   paths.marker));
    }
    return marker_record{.phase = *phase,
                         .pid = marker_pid,
                         .world = std::string{*world_text},
                         .temp = std::string{*temp_text},
                         .backup = std::string{*backup_text}};
}

// --- process-local state ----------------------------------------------------

/// Roots currently being rebuilt by this process. `world_store::open` is called
/// by `anvil::rebuild_palette` for the source world; without this guard the
/// rebuild would see its own `rebuilding` marker and roll itself back. The set
/// is exact (absolute, lexically normal root), process-local and scoped to the
/// `rebuild_palette_in_place` call stack (RAII), so it never disables recovery
/// for the root outside that call.
class active_maintenance_set {
  public:
    [[nodiscard]] static auto instance() -> active_maintenance_set & {
        static active_maintenance_set value;
        return value;
    }

    [[nodiscard]] auto contains(const std::string &root) -> bool {
        const std::lock_guard lock{mutex_};
        return roots_.contains(root);
    }

    void add(const std::string &root) {
        const std::lock_guard lock{mutex_};
        roots_.insert(root);
    }

    void remove(const std::string &root) noexcept {
        const std::lock_guard lock{mutex_};
        roots_.erase(root);
    }

  private:
    std::mutex mutex_{};
    std::set<std::string> roots_{};
};

class maintenance_scope {
  public:
    explicit maintenance_scope(std::string root) : root_{std::move(root)} {
        active_maintenance_set::instance().add(root_);
    }
    ~maintenance_scope() { active_maintenance_set::instance().remove(root_); }

    maintenance_scope(const maintenance_scope &) = delete;
    auto operator=(const maintenance_scope &) -> maintenance_scope & = delete;
    maintenance_scope(maintenance_scope &&) = delete;
    auto operator=(maintenance_scope &&) -> maintenance_scope & = delete;

  private:
    std::string root_;
};

/// Serializes recovery across threads (same-root concurrent recovery would
/// otherwise race on the marker). Recursive so a recovery that opens a world
/// for validation can re-enter on another root; same-root re-entry is skipped
/// by `recovering_roots`.
[[nodiscard]] auto recovery_mutex() -> std::recursive_mutex & {
    static std::recursive_mutex value;
    return value;
}

/// Roots being recovered by the current thread. A nested
/// `world_store::open(root)` during validation must not recurse into the same
/// marker.
thread_local std::set<std::string> recovering_roots;

// --- filesystem helpers -----------------------------------------------------

[[nodiscard]] auto probe(const std::string &path) -> result<path_kind> {
    auto status{io::status_at(path)};
    if (!status) {
        return std::unexpected{
            wrap(status.error(), fmt::format("cannot inspect '{}'", path))};
    }
    if (status->symlink) {
        return path_kind::other;
    }
    if (!status->exists) {
        return path_kind::absent;
    }
    if (!status->directory) {
        return path_kind::other;
    }
    return path_kind::directory;
}

/// A world is valid when it is a real directory with `meta.rfw` and a
/// `world_store::open` succeeds. Used to decide roll-forward/rollback: a
/// rebuilt temp without meta is a partial import (meta is written last).
[[nodiscard]] auto valid_world(const std::string &path) -> bool {
    if (!io::is_directory(path) || io::is_symlink(path)) {
        return false;
    }
    auto meta{io::status_at(join_path(path, "meta.rfw"))};
    if (!meta || !meta->exists || !meta->regular || meta->symlink) {
        return false;
    }
    auto store{world_store::open(path)};
    return static_cast<bool>(store);
}

[[nodiscard]] auto remove_tree(const std::string &path) -> status {
    std::error_code error;
    static_cast<void>(fs::remove_all(fs::path{path}, error));
    if (error) {
        return failing(
            error_code::io_error,
            fmt::format("cannot remove '{}': {}", path, error.message()));
    }
    return status_ok();
}

[[nodiscard]] auto remove_file(const std::string &path) -> status {
    std::error_code error;
    static_cast<void>(fs::remove(fs::path{path}, error));
    if (error) {
        return failing(
            error_code::io_error,
            fmt::format("cannot remove '{}': {}", path, error.message()));
    }
    return status_ok();
}

/// True when anything (including a broken symlink) already occupies `path`.
[[nodiscard]] auto path_occupied(const std::string &path) -> result<bool> {
    auto status{io::status_at(path)};
    if (!status) {
        return std::unexpected{status.error()};
    }
    return status->exists || status->symlink;
}

/// Rename that refuses to replace an existing destination (a pre-existing path
/// is never deleted). POSIX `rename` would replace; the existence check is the
/// portable guard and single-writer maintenance is a documented precondition.
[[nodiscard]] auto rename_no_replace(const std::string &from,
                                     const std::string &to) -> status {
    auto destination{io::status_at(to)};
    if (!destination) {
        return std::unexpected{
            wrap(destination.error(),
                 fmt::format("cannot inspect rename target '{}'", to))};
    }
    if (destination->exists || destination->symlink) {
        return failing(error_code::already_exists,
                       fmt::format("refusing to replace existing '{}'", to));
    }
    if (auto renamed{file::rename_replace(from, to)}; !renamed) {
        return std::unexpected{
            wrap(renamed.error(),
                 fmt::format("cannot rename '{}' to '{}'", from, to))};
    }
    return status_ok();
}

/// fsync every file and directory of the rebuilt tree. Region records are
/// already synced by their commits; this covers the directory entries created
/// by the import (and the meta/blob files) before the swap makes them visible
/// at the world root. Symlinks and special files fail closed: a world tree must
/// not contain them.
[[nodiscard]] auto sync_tree(const std::string &directory, unsigned depth)
    -> status {
    if (depth > max_sync_depth) {
        return failing(error_code::invalid_argument,
                       fmt::format("rebuilt world tree under '{}' is deeper "
                                   "than {} levels",
                                   directory, max_sync_depth));
    }
    auto entries{io::list_directory(directory)};
    if (!entries) {
        return std::unexpected{
            wrap(entries.error(), fmt::format("cannot list '{}'", directory))};
    }
    for (const auto &name : *entries) {
        const auto child{join_path(directory, name)};
        auto status{io::status_at(child)};
        if (!status) {
            return std::unexpected{wrap(
                status.error(), fmt::format("cannot inspect '{}'", child))};
        }
        if (!status->exists) {
            continue;
        }
        if (status->symlink) {
            return failing(error_code::invalid_argument,
                           fmt::format("rebuilt world contains a symbolic link "
                                       "'{}'",
                                       child));
        }
        if (status->directory) {
            if (auto synced{sync_tree(child, depth + 1)}; !synced) {
                return synced;
            }
            continue;
        }
        if (!status->regular) {
            return failing(error_code::invalid_argument,
                           fmt::format("rebuilt world contains a non-regular "
                                       "file '{}'",
                                       child));
        }
        auto handle{file::random_access_file::open(
            child, file::open_mode::read_write, {.no_follow = true})};
        if (!handle) {
            return std::unexpected{
                wrap(handle.error(),
                     fmt::format("cannot open '{}' for fsync", child))};
        }
        if (auto synced{handle->sync()}; !synced) {
            return std::unexpected{
                wrap(synced.error(), fmt::format("cannot fsync '{}'", child))};
        }
    }
    return file::sync_directory(directory);
}

[[nodiscard]] auto rollback_scratch(const world_paths &paths,
                                    const std::string &temp) -> status {
    if (auto removed{remove_tree(temp)}; !removed) {
        return removed;
    }
    if (auto removed{remove_file(paths.marker)}; !removed) {
        return removed;
    }
    return file::sync_directory(paths.parent);
}

// --- recovery ---------------------------------------------------------------

[[nodiscard]] auto promote_temp(const world_paths &paths,
                                const marker_record &marker) -> status {
    if (auto renamed{rename_no_replace(marker.temp, paths.root)}; !renamed) {
        return renamed;
    }
    if (auto synced{file::sync_directory(paths.parent)}; !synced) {
        return std::unexpected{wrap(synced.error(),
                                    "cannot fsync the world parent after "
                                    "promoting the rebuilt world")};
    }
    if (auto removed{remove_tree(marker.backup)}; !removed) {
        return removed;
    }
    if (auto removed{remove_file(paths.marker)}; !removed) {
        return removed;
    }
    return file::sync_directory(paths.parent);
}

[[nodiscard]] auto restore_backup(const world_paths &paths,
                                  const marker_record &marker) -> status {
    if (auto renamed{rename_no_replace(marker.backup, paths.root)}; !renamed) {
        return renamed;
    }
    if (auto synced{file::sync_directory(paths.parent)}; !synced) {
        return std::unexpected{wrap(synced.error(),
                                    "cannot fsync the world parent after "
                                    "restoring the backup")};
    }
    if (auto removed{remove_tree(marker.temp)}; !removed) {
        return removed;
    }
    if (auto removed{remove_file(paths.marker)}; !removed) {
        return removed;
    }
    return file::sync_directory(paths.parent);
}

[[nodiscard]] auto roll_forward(const world_paths &paths,
                                const marker_record &marker) -> status {
    if (auto renamed{rename_no_replace(paths.root, marker.backup)}; !renamed) {
        return renamed;
    }
    if (auto synced{file::sync_directory(paths.parent)}; !synced) {
        return std::unexpected{wrap(synced.error(),
                                    "cannot fsync the world parent after "
                                    "moving the world to its backup")};
    }
    return promote_temp(paths, marker);
}

[[nodiscard]] auto recover_marker(const world_paths &paths,
                                  const marker_record &marker) -> status {
    auto root_kind{probe(paths.root)};
    if (!root_kind) {
        return std::unexpected{root_kind.error()};
    }
    auto backup_kind{probe(marker.backup)};
    if (!backup_kind) {
        return std::unexpected{backup_kind.error()};
    }
    auto temp_kind{probe(marker.temp)};
    if (!temp_kind) {
        return std::unexpected{temp_kind.error()};
    }
    if (*root_kind == path_kind::other || *backup_kind == path_kind::other ||
        *temp_kind == path_kind::other) {
        return failing(error_code::io_error,
                       fmt::format("palette maintenance marker '{}' names a "
                                   "path that exists but is not a directory",
                                   paths.marker));
    }
    const bool root{*root_kind == path_kind::directory};
    const bool backup{*backup_kind == path_kind::directory};
    const bool temp{*temp_kind == path_kind::directory};

    if (marker.phase != maintenance_phase::swapping) {
        // The swap never started: the root must still be the source world.
        if (!root) {
            return failing(error_code::io_error,
                           fmt::format("palette maintenance marker '{}' is in "
                                       "phase '{}' but the world root is "
                                       "missing",
                                       paths.marker, phase_name(marker.phase)));
        }
        if (backup) {
            return failing(error_code::io_error,
                           fmt::format("palette maintenance marker '{}' is in "
                                       "phase '{}' but a backup already exists",
                                       paths.marker, phase_name(marker.phase)));
        }
        if (temp) {
            if (auto removed{remove_tree(marker.temp)}; !removed) {
                return removed;
            }
        }
        if (auto removed{remove_file(paths.marker)}; !removed) {
            return removed;
        }
        return file::sync_directory(paths.parent);
    }

    if (root && backup && temp) {
        return failing(error_code::io_error,
                       fmt::format("palette maintenance marker '{}' is "
                                   "ambiguous: world, backup and temp all "
                                   "exist",
                                   paths.marker));
    }
    if (root && backup) {
        // The temp was already promoted; finish by dropping the backup. Never
        // drop the only good copy when the promoted root is not a valid world.
        if (!valid_world(paths.root)) {
            return failing(error_code::io_error,
                           fmt::format("palette maintenance marker '{}' has a "
                                       "backup but '{}' is not a valid world; "
                                       "refusing to drop the backup",
                                       paths.marker, paths.root));
        }
        if (auto removed{remove_tree(marker.backup)}; !removed) {
            return removed;
        }
        if (auto removed{remove_file(paths.marker)}; !removed) {
            return removed;
        }
        return file::sync_directory(paths.parent);
    }
    if (root && temp) {
        // The swap did not start (or root->backup failed). Only roll forward
        // when the root really is the marker's source world; an unrecognised
        // root is never renamed.
        if (!valid_world(paths.root)) {
            return failing(error_code::io_error,
                           fmt::format("palette maintenance marker '{}' names "
                                       "'{}' but it is not a valid world; "
                                       "refusing to roll forward",
                                       paths.marker, paths.root));
        }
        if (!valid_world(marker.temp)) {
            // The rebuilt temp is incomplete: discard it, keep the source.
            if (auto removed{remove_tree(marker.temp)}; !removed) {
                return removed;
            }
            if (auto removed{remove_file(paths.marker)}; !removed) {
                return removed;
            }
            return file::sync_directory(paths.parent);
        }
        return roll_forward(paths, marker);
    }
    if (root) {
        // Swap finished and the backup is gone; only the marker is left. An
        // invalid root means the swap produced something unusable: fail closed
        // and keep the marker instead of declaring success.
        if (!valid_world(paths.root)) {
            return failing(error_code::io_error,
                           fmt::format("palette maintenance marker '{}' is in "
                                       "phase 'swapping' but '{}' is not a "
                                       "valid world",
                                       paths.marker, paths.root));
        }
        if (auto removed{remove_file(paths.marker)}; !removed) {
            return removed;
        }
        return file::sync_directory(paths.parent);
    }
    if (backup && temp) {
        if (valid_world(marker.temp)) {
            return promote_temp(paths, marker);
        }
        if (valid_world(marker.backup)) {
            return restore_backup(paths, marker);
        }
        return failing(error_code::io_error,
                       fmt::format("palette maintenance marker '{}' has "
                                   "neither a valid temp nor a valid backup",
                                   paths.marker));
    }
    if (backup) {
        if (!valid_world(marker.backup)) {
            return failing(error_code::io_error,
                           fmt::format("palette maintenance backup '{}' is not "
                                       "a valid world",
                                       marker.backup));
        }
        return restore_backup(paths, marker);
    }
    if (temp) {
        if (!valid_world(marker.temp)) {
            return failing(error_code::io_error,
                           fmt::format("palette maintenance temp '{}' is not a "
                                       "valid world",
                                       marker.temp));
        }
        return promote_temp(paths, marker);
    }
    return failing(error_code::not_found,
                   fmt::format("palette maintenance marker '{}' refers to a "
                               "missing world, backup and temp",
                               paths.marker));
}

} // namespace

auto palette_maintenance_marker_path(std::string_view world_root)
    -> std::string {
    auto paths{describe_root(world_root)};
    return paths ? paths->marker : std::string{};
}

auto palette_maintenance_temp_path(std::string_view world_root,
                                   unsigned attempt) -> std::string {
    auto paths{describe_root(world_root)};
    return paths ? scratch_path(*paths, attempt, "tmp") : std::string{};
}

auto palette_maintenance_backup_path(std::string_view world_root,
                                     unsigned attempt) -> std::string {
    auto paths{describe_root(world_root)};
    return paths ? scratch_path(*paths, attempt, "bak") : std::string{};
}

auto recover_palette_maintenance(std::string_view world_root) -> result<bool> {
    if (world_root.empty()) {
        return failing(error_code::invalid_argument, "world path is empty");
    }
    auto paths{describe_root(world_root)};
    if (!paths) {
        if (paths.error().code == error_code::invalid_argument) {
            // `.`/`..`/a path without a filename component cannot have a
            // sibling marker; keep `world_store::open` working for such roots.
            return false;
        }
        return std::unexpected{paths.error()};
    }
    // Fast path: the overwhelmingly common case (no marker) never takes the
    // active-set/recovery mutex, so opening an unrelated world during a
    // recovery that validates another tree cannot deadlock. A permission/IO
    // error while inspecting the marker fails closed instead of silently
    // opening.
    auto marker_status{io::status_at(paths->marker)};
    if (!marker_status) {
        return std::unexpected{marker_status.error()};
    }
    if (!marker_status->exists && !marker_status->symlink) {
        return false;
    }
    if (active_maintenance_set::instance().contains(paths->root)) {
        return false;
    }
    const std::lock_guard lock{recovery_mutex()};
    if (active_maintenance_set::instance().contains(paths->root) ||
        recovering_roots.contains(paths->root)) {
        return false;
    }
    auto marker{load_marker(*paths)};
    if (!marker) {
        return std::unexpected{marker.error()};
    }
    if (!*marker) {
        return false;
    }
    recovering_roots.insert(paths->root);
    struct reentry_guard {
        std::string root;
        ~reentry_guard() { recovering_roots.erase(root); }
    } guard{paths->root};
    if (auto recovered{recover_marker(*paths, **marker)}; !recovered) {
        return std::unexpected{recovered.error()};
    }
    return true;
}

auto rebuild_palette_in_place(std::string_view world_root,
                              const palette_maintenance_options &options)
    -> result<palette_maintenance_report> {
    if (options.zstd_level < rwf::min_zstd_level ||
        options.zstd_level > rwf::max_zstd_level) {
        return failing(error_code::invalid_argument,
                       fmt::format("zstd level {} is outside [{}, {}]",
                                   options.zstd_level, rwf::min_zstd_level,
                                   rwf::max_zstd_level));
    }
    auto paths{describe_root(world_root)};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    if (!io::is_directory(paths->root)) {
        return failing(
            error_code::not_found,
            fmt::format("'{}' is not a world directory", paths->root));
    }
    if (io::is_symlink(paths->root)) {
        return failing(error_code::invalid_argument,
                       fmt::format("world root '{}' must not be a symbolic "
                                   "link",
                                   paths->root));
    }
    if (active_maintenance_set::instance().contains(paths->root)) {
        return failing(error_code::already_exists,
                       fmt::format("palette maintenance is already running for "
                                   "'{}'",
                                   paths->root));
    }

    // 1. Resolve a crash marker left by an earlier attempt before touching
    // anything. This is the only recovery call that runs before the marker of
    // this attempt exists.
    bool recovered{false};
    {
        auto prior{recover_palette_maintenance(paths->root)};
        if (!prior) {
            return std::unexpected{prior.error()};
        }
        recovered = *prior;
    }

    // 2. Deterministic scratch stem: first attempt whose temp and backup are
    // both absent. Pre-existing paths are never deleted.
    std::string temp{};
    std::string backup{};
    for (unsigned attempt{0}; attempt < max_scratch_attempts; ++attempt) {
        auto candidate_temp{
            palette_maintenance_temp_path(paths->root, attempt)};
        auto candidate_backup{
            palette_maintenance_backup_path(paths->root, attempt)};
        if (candidate_temp.empty() || candidate_backup.empty()) {
            break;
        }
        auto temp_taken{path_occupied(candidate_temp)};
        if (!temp_taken) {
            return std::unexpected{temp_taken.error()};
        }
        auto backup_taken{path_occupied(candidate_backup)};
        if (!backup_taken) {
            return std::unexpected{backup_taken.error()};
        }
        if (!*temp_taken && !*backup_taken) {
            temp = std::move(candidate_temp);
            backup = std::move(candidate_backup);
            break;
        }
    }
    if (temp.empty()) {
        return failing(error_code::already_exists,
                       fmt::format("cannot allocate a palette maintenance "
                                   "scratch path next to '{}'",
                                   paths->root));
    }

    // 3. From here on every `world_store::open(root)` (including the ones
    // inside `anvil::rebuild_palette`) skips recovery for this root only.
    const maintenance_scope scope{paths->root};

    marker_record record{.phase = maintenance_phase::planned,
                         .pid = platform::process_id(),
                         .world = paths->root,
                         .temp = temp,
                         .backup = backup};
    if (auto written{write_marker(*paths, record)}; !written) {
        static_cast<void>(remove_file(paths->marker));
        return std::unexpected{written.error()};
    }
    record.phase = maintenance_phase::rebuilding;
    if (auto written{write_marker(*paths, record)}; !written) {
        static_cast<void>(rollback_scratch(*paths, temp));
        return std::unexpected{written.error()};
    }

    // 4. Rebuild through the Anvil round-trip. The source is opened briefly by
    // `anvil::rebuild_palette` and closed before the swap.
    auto rebuilt{anvil::rebuild_palette(paths->root, temp, false,
                                        options.threads, options.zstd_level)};
    if (!rebuilt) {
        if (auto cleanup{rollback_scratch(*paths, temp)}; !cleanup) {
            return std::unexpected{combine(rebuilt.error(), cleanup.error())};
        }
        return std::unexpected{rebuilt.error()};
    }

    // 5. Make the rebuilt tree durable before the marker announces the swap.
    if (auto synced{sync_tree(temp, 0)}; !synced) {
        if (auto cleanup{rollback_scratch(*paths, temp)}; !cleanup) {
            return std::unexpected{combine(synced.error(), cleanup.error())};
        }
        return std::unexpected{synced.error()};
    }
    record.phase = maintenance_phase::swapping;
    if (auto written{write_marker(*paths, record)}; !written) {
        if (auto cleanup{rollback_scratch(*paths, temp)}; !cleanup) {
            return std::unexpected{combine(written.error(), cleanup.error())};
        }
        return std::unexpected{written.error()};
    }

    // 6. Swap. root -> backup, temp -> root; both renames are fsynced.
    if (auto renamed{rename_no_replace(paths->root, backup)}; !renamed) {
        if (auto cleanup{rollback_scratch(*paths, temp)}; !cleanup) {
            return std::unexpected{combine(renamed.error(), cleanup.error())};
        }
        return std::unexpected{renamed.error()};
    }
    if (auto synced{file::sync_directory(paths->parent)}; !synced) {
        auto restored{rename_no_replace(backup, paths->root)};
        if (!restored) {
            return std::unexpected{combine(synced.error(), restored.error())};
        }
        if (auto cleanup{rollback_scratch(*paths, temp)}; !cleanup) {
            return std::unexpected{combine(synced.error(), cleanup.error())};
        }
        return std::unexpected{synced.error()};
    }
    if (auto promoted{rename_no_replace(temp, paths->root)}; !promoted) {
        auto restored{rename_no_replace(backup, paths->root)};
        if (!restored) {
            // The marker stays; the next recovery sees !root, backup, temp.
            return std::unexpected{combine(promoted.error(), restored.error())};
        }
        if (auto synced{file::sync_directory(paths->parent)}; !synced) {
            return std::unexpected{combine(promoted.error(), synced.error())};
        }
        if (auto cleanup{rollback_scratch(*paths, temp)}; !cleanup) {
            return std::unexpected{combine(promoted.error(), cleanup.error())};
        }
        return std::unexpected{promoted.error()};
    }
    if (auto synced{file::sync_directory(paths->parent)}; !synced) {
        // New root is in place but its rename is not durable yet: keep the
        // marker and backup so recovery can finish or roll back.
        return std::unexpected{synced.error()};
    }

    // 7. Validate the new root before dropping the backup. `world_store::open`
    // skips recovery for the active root; a broken rebuild is rolled back.
    {
        auto opened{world_store::open(paths->root)};
        if (!opened) {
            auto removed_new{remove_tree(paths->root)};
            if (!removed_new) {
                return std::unexpected{
                    combine(opened.error(), removed_new.error())};
            }
            auto restored{rename_no_replace(backup, paths->root)};
            if (!restored) {
                return std::unexpected{
                    combine(opened.error(), restored.error())};
            }
            if (auto synced{file::sync_directory(paths->parent)}; !synced) {
                return std::unexpected{combine(opened.error(), synced.error())};
            }
            if (auto cleanup{rollback_scratch(*paths, temp)}; !cleanup) {
                return std::unexpected{
                    combine(opened.error(), cleanup.error())};
            }
            return std::unexpected{
                wrap(opened.error(), "the rebuilt world cannot be opened")};
        }
    }

    // 8. Drop the backup, then the marker. Each removal is followed by a
    // parent fsync; a failure keeps the marker so recovery retries.
    if (auto removed{remove_tree(backup)}; !removed) {
        return std::unexpected{
            wrap(removed.error(),
                 "the swap succeeded but the backup could not be removed")};
    }
    if (auto synced{file::sync_directory(paths->parent)}; !synced) {
        return std::unexpected{
            wrap(synced.error(),
                 "the swap succeeded but the backup removal is not durable")};
    }
    if (auto removed{remove_file(paths->marker)}; !removed) {
        return std::unexpected{
            wrap(removed.error(),
                 "the swap succeeded but the marker could not be removed")};
    }
    if (auto synced{file::sync_directory(paths->parent)}; !synced) {
        return std::unexpected{
            wrap(synced.error(),
                 "the swap succeeded but the marker removal is not durable")};
    }

    palette_maintenance_report report{};
    report.entries_before = rebuilt->entries_before;
    report.entries_after = rebuilt->entries_after;
    report.chunks = static_cast<std::uint64_t>(rebuilt->chunks);
    report.regions = static_cast<std::uint64_t>(rebuilt->regions);
    report.bytes_before = rebuilt->bytes_before;
    report.bytes_after = rebuilt->bytes_after;
    report.recovered = recovered;
    return report;
}

} // namespace refrain::world
