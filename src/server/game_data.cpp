// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/game_data.hpp>

#include <refrain/archive/game_jar.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/platform/subprocess.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::server {

namespace {

constexpr refrain::log::logger log_game_data{"game-data"};

namespace io = refrain::core::io;
namespace json = refrain::data::json;

/// A marker is a few hundred bytes; the ceiling only guards a corrupt file.
constexpr std::uint64_t max_marker_bytes = 1ULL * 1024ULL * 1024ULL;

/// Upper bound for one tracked report or table copied from the installed
/// content; the largest report is below 2 MiB.
constexpr std::uint64_t max_copied_file_bytes = 16ULL * 1024ULL * 1024ULL;

/// Ceiling for one datapack document read back for the content digest. The
/// largest worldgen documents are far below this bound.
constexpr std::uint64_t max_data_file_bytes = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] auto target_version() -> std::string {
    return std::string{game_data_version};
}

[[nodiscard]] auto join(std::string_view left, std::string_view right)
    -> std::string {
    return refrain::data::join_path(left, right);
}

/// True when `path` ends with `suffix` (byte comparison).
[[nodiscard]] auto ends_with(std::string_view value,
                             std::string_view suffix) noexcept -> bool {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] auto starts_with(std::string_view value,
                               std::string_view prefix) noexcept -> bool {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] auto ascii_lower(std::string_view text) -> std::string {
    std::string folded{text};
    for (auto &character : folded) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return folded;
}

[[nodiscard]] auto is_hex_sha1(std::string_view value) -> bool {
    if (value.size() != 40) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

// --- directory inspection --------------------------------------------------

/// Every regular file below `root`, as paths relative to `root`, sorted so a
/// digest of the tree is independent of directory iteration order.
[[nodiscard]] auto list_files(std::string_view root)
    -> result<std::vector<std::string>> {
    std::vector<std::string> files{};
    std::error_code error;
    const std::filesystem::path base{std::string{root}};
    if (!std::filesystem::is_directory(base, error) || error) {
        return std::unexpected{
            make_error(error_code::not_found,
                       fmt::format("'{}' is not a directory", root))};
    }
    for (std::filesystem::recursive_directory_iterator iterator{base, error},
         end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("failed to list '{}': {}", root, error.message()))};
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                return std::unexpected{make_error(
                    error_code::io_error,
                    fmt::format("failed to inspect '{}': {}",
                                iterator->path().string(), error.message()))};
            }
            continue;
        }
        const auto relative{
            iterator->path().lexically_relative(base).generic_string()};
        files.push_back(relative);
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// A namespace is present when it has a `worldgen` directory; that is the
/// minimum a datapack needs for the generator to load.
[[nodiscard]] auto has_worldgen_documents(std::string_view data_root) -> bool {
    if (!io::is_directory(data_root)) {
        return false;
    }
    auto namespaces{io::list_directory(data_root)};
    if (!namespaces) {
        return false;
    }
    for (const auto &name : *namespaces) {
        if (io::is_directory(join(data_root, name + "/worldgen"))) {
            return true;
        }
    }
    return false;
}

/// Digest of a datapack tree: BLAKE3 over the sorted `path\0file-digest\0`
/// listing, so a moved or renamed file changes the digest while the directory
/// iteration order never does.
[[nodiscard]] auto tree_digest(std::string_view root) -> result<std::string> {
    auto files{list_files(root)};
    if (!files) {
        return std::unexpected{files.error()};
    }
    std::string listing{};
    for (const auto &relative : *files) {
        auto bytes{
            io::read_file(join(root, relative),
                          io::read_options{.max_bytes = max_data_file_bytes,
                                           .allow_symlink = false})};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        listing += relative;
        listing.push_back('\0');
        listing += refrain::data::to_hex(refrain::data::blake3(*bytes));
        listing.push_back('\0');
    }
    const auto digest{refrain::data::blake3(std::as_bytes(std::span{listing}))};
    return refrain::data::to_hex(digest);
}

// --- marker ----------------------------------------------------------------

/// What the marker (if any) says about the directory.
struct marker_state {
    /// True when a marker file exists.
    bool present{false};
    /// False when the marker cannot be trusted (schema, shape).
    bool valid{false};
    /// Game version the marker names.
    std::string game_version{};
    /// Data version the marker names.
    std::int32_t data_version{0};
    /// Why the marker is missing, invalid or stale, for diagnostics.
    std::string problem{};
};

[[nodiscard]] auto read_marker(std::string_view root) -> result<marker_state> {
    marker_state state{};
    const auto path{join(root, game_data_marker_name)};
    if (!io::exists(path)) {
        return state;
    }
    state.present = true;
    auto text{
        io::read_text(path, io::read_options{.max_bytes = max_marker_bytes,
                                             .allow_symlink = false})};
    if (!text) {
        state.problem = text.error().message;
        return state;
    }
    auto document{json::value::parse(*text)};
    if (!document) {
        state.problem = fmt::format("the marker is not valid JSON: {}",
                                    document.error().message);
        return state;
    }
    if (!document->is_object()) {
        state.problem = "the marker is not a JSON object";
        return state;
    }
    if (const auto *schema{document->find("schema")}; schema != nullptr) {
        auto schema_text{schema->as_string()};
        if (!schema_text) {
            state.problem = "the marker schema is not a string";
            return state;
        }
        if (*schema_text != game_data_marker_schema) {
            state.problem = fmt::format(
                "the marker uses unsupported schema '{}', expected '{}'",
                *schema_text, game_data_marker_schema);
            return state;
        }
    }
    const auto *version{document->find("game_version")};
    if (version == nullptr) {
        state.problem = "the marker names no game version";
        return state;
    }
    auto version_text{version->as_string()};
    if (!version_text) {
        state.problem = "the marker game version is not a string";
        return state;
    }
    state.game_version = std::string{*version_text};
    const auto *data_version{document->find("data_version")};
    if (data_version == nullptr) {
        state.problem = "the marker names no data version";
        return state;
    }
    auto data_version_value{data_version->as_int64()};
    if (!data_version_value || *data_version_value < 0 ||
        *data_version_value > std::numeric_limits<std::int32_t>::max()) {
        state.problem = "the marker data version is not a valid integer";
        return state;
    }
    state.data_version = static_cast<std::int32_t>(*data_version_value);
    state.valid = true;
    return state;
}

/// A directory the server can generate from: the marker (when present) names
/// this game version, or no marker exists and a worldgen datapack is staged.
[[nodiscard]] auto cache_is_current(const marker_state &marker,
                                    std::string_view root) -> bool {
    return marker.present && marker.valid &&
           marker.game_version == game_data_version &&
           marker.data_version == game_data_data_version &&
           has_worldgen_documents(join(root, "data"));
}

// --- small file helpers ----------------------------------------------------

[[nodiscard]] auto copy_tree(std::string_view source,
                             std::string_view destination) -> status {
    auto files{list_files(source)};
    if (!files) {
        return std::unexpected{files.error()};
    }
    for (const auto &relative : *files) {
        auto bytes{
            io::read_file(join(source, relative),
                          io::read_options{.max_bytes = max_copied_file_bytes,
                                           .allow_symlink = false})};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        const auto target{join(destination, relative)};
        if (auto directories{
                io::create_directories(io::parent_directory(target))};
            !directories) {
            return directories;
        }
        if (auto written{io::write_file(target, *bytes)}; !written) {
            return written;
        }
    }
    return {};
}

// --- archive descriptor ----------------------------------------------------

struct archive_descriptor {
    std::string url{};
    std::string sha1{};
    std::uint64_t size{0};
};

[[nodiscard]] auto fetch_metadata(net::download_transport &transport,
                                  std::string_view url,
                                  std::chrono::milliseconds timeout)
    -> result<std::string> {
    auto text{transport.fetch_text(url, net::max_metadata_bytes, timeout)};
    if (!text) {
        return std::unexpected{make_error(
            text.error().code, fmt::format("failed to fetch '{}': {}", url,
                                           text.error().message))};
    }
    return text;
}

/// Find the target version in the launcher manifest and read its archive.
[[nodiscard]] auto select_archive(net::download_transport &transport,
                                  const game_data_options &options,
                                  const game_data_limits &limits)
    -> result<archive_descriptor> {
    auto manifest_text{fetch_metadata(transport, options.manifest_url,
                                      limits.metadata_timeout)};
    if (!manifest_text) {
        return std::unexpected{manifest_text.error()};
    }
    auto manifest{json::value::parse(*manifest_text)};
    if (!manifest) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("the version manifest from '{}' is not valid JSON: {}",
                        options.manifest_url, manifest.error().message))};
    }
    const auto *versions{manifest->find("versions")};
    if (versions == nullptr || !versions->is_array()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("the version manifest from '{}' lists no versions",
                        options.manifest_url))};
    }
    std::string version_url{};
    for (const auto &entry : *versions->as_array()) {
        if (!entry.is_object()) {
            continue;
        }
        const auto *id{entry.find("id")};
        const auto *url{entry.find("url")};
        if (id == nullptr || url == nullptr) {
            continue;
        }
        auto id_text{id->as_string()};
        auto url_text{url->as_string()};
        if (!id_text || !url_text || *id_text != game_data_version) {
            continue;
        }
        version_url = std::string{*url_text};
        break;
    }
    if (version_url.empty()) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("the version manifest from '{}' has no Minecraft {} "
                        "release",
                        options.manifest_url, game_data_version))};
    }

    auto version_text{
        fetch_metadata(transport, version_url, limits.metadata_timeout)};
    if (!version_text) {
        return std::unexpected{version_text.error()};
    }
    auto version{json::value::parse(*version_text)};
    if (!version) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("the version document for Minecraft {} is not valid "
                        "JSON: {}",
                        game_data_version, version.error().message))};
    }
    const auto *downloads{version->find("downloads")};
    const auto *server{downloads == nullptr ? nullptr
                                            : downloads->find("server")};
    if (server == nullptr) {
        return std::unexpected{
            make_error(error_code::version_mismatch,
                       fmt::format("Minecraft {} publishes no server archive",
                                   game_data_version))};
    }
    const auto *url{server->find("url")};
    const auto *sha1{server->find("sha1")};
    if (url == nullptr || sha1 == nullptr) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("the Minecraft {} server download entry names no url "
                        "or SHA-1",
                        game_data_version))};
    }
    auto url_text{url->as_string()};
    auto sha1_text{sha1->as_string()};
    if (!url_text || !sha1_text) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("the Minecraft {} server download entry is malformed",
                        game_data_version))};
    }
    archive_descriptor descriptor{.url = std::string{*url_text},
                                  .sha1 = std::string{*sha1_text}};
    if (!is_hex_sha1(descriptor.sha1)) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("the Minecraft {} server archive SHA-1 is malformed",
                        game_data_version))};
    }
    if (const auto *size{server->find("size")}; size != nullptr) {
        auto size_value{size->as_int64()};
        if (!size_value || *size_value < 0) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("the Minecraft {} server archive size is "
                            "malformed",
                            game_data_version))};
        }
        descriptor.size = static_cast<std::uint64_t>(*size_value);
    }
    return descriptor;
}

// --- transfer --------------------------------------------------------------

/// Streams a response body into a file below the data directory.
class archive_sink final : public net::download_sink {
  public:
    explicit archive_sink(std::FILE *file) : file_{file} {}

    [[nodiscard]] auto write(std::span<const std::byte> chunk)
        -> status override {
        if (chunk.empty()) {
            return {};
        }
        const auto written{std::fwrite(chunk.data(), 1, chunk.size(), file_)};
        if (written != chunk.size()) {
            return std::unexpected{
                make_error(error_code::io_error,
                           "failed to write the downloaded server archive")};
        }
        written_ += written;
        return {};
    }

    [[nodiscard]] auto written() const noexcept -> std::uint64_t {
        return written_;
    }

  private:
    std::FILE *file_{nullptr};
    std::uint64_t written_{0};
};

/// Download the archive named by `descriptor` and verify it before anything is
/// extracted. Returns the received byte count.
[[nodiscard]] auto download_archive(const archive_descriptor &descriptor,
                                    std::string_view destination,
                                    net::download_transport &transport,
                                    const game_data_limits &limits)
    -> result<std::uint64_t> {
    if (auto directories{
            io::create_directories(io::parent_directory(destination))};
        !directories) {
        return std::unexpected{directories.error()};
    }
    struct file_closer {
        void operator()(std::FILE *handle) const noexcept {
            if (handle != nullptr) {
                static_cast<void>(std::fclose(handle));
            }
        }
    };
    std::unique_ptr<std::FILE, file_closer> file{
        std::fopen(std::string{destination}.c_str(), "wb")};
    if (!file) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("failed to create the archive temporary file '{}'",
                        destination))};
    }
    archive_sink sink{file.get()};
    net::download_request request{.url = descriptor.url,
                                  .max_bytes = descriptor.size > 0
                                                   ? descriptor.size
                                                   : net::max_archive_bytes,
                                  .timeout = limits.archive_timeout};
    auto result{transport.download(request, sink)};
    if (!result) {
        return std::unexpected{result.error()};
    }
    if (std::fclose(file.release()) != 0) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("failed to flush the archive temporary file '{}'",
                        destination))};
    }
    if (descriptor.size > 0 && result->bytes != descriptor.size) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("the downloaded server archive is {} bytes, the "
                        "manifest declares {}",
                        result->bytes, descriptor.size))};
    }
    const auto received{ascii_lower(refrain::data::to_hex(
        std::span<const std::byte>{result->sha1.data(), result->sha1.size()}))};
    if (received != ascii_lower(descriptor.sha1)) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("the downloaded server archive for Minecraft {} "
                        "failed its SHA-1 check: expected {}, received {}",
                        game_data_version, ascii_lower(descriptor.sha1),
                        received))};
    }
    return result->bytes;
}

// --- extraction ------------------------------------------------------------

struct extraction_report {
    std::size_t datapack_files{0};
    std::size_t templates{0};
};

[[nodiscard]] auto ensure_parent(std::string_view path) -> status {
    return io::create_directories(io::parent_directory(path));
}

[[nodiscard]] auto write_relative(std::string_view staging_root,
                                  std::string_view relative,
                                  std::span<const std::byte> bytes) -> status {
    const auto target{join(staging_root, relative)};
    if (auto directories{ensure_parent(target)}; !directories) {
        return directories;
    }
    return io::write_file(target, bytes);
}

/// Unpack the datapack documents and the structure templates from a verified
/// server archive into a staging directory, then swap the result into place.
/// Only the game data the generator reads is written; recipes, loot tables,
/// advancements and assets stay in the archive.
[[nodiscard]] auto install_from_archive(std::string_view archive,
                                        std::string_view root)
    -> result<extraction_report> {
    auto jar{refrain::archive::game_jar::open(archive)};
    if (!jar) {
        return std::unexpected{jar.error()};
    }
    if (!jar->version_id().empty() && jar->version_id() != game_data_version) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("the downloaded archive is Minecraft {}, expected {}",
                        jar->version_id(), game_data_version))};
    }
    const auto staging_dir{join(root, ".extract.tmp")};
    std::error_code remove_error;
    std::filesystem::remove_all(std::string{staging_dir}, remove_error);
    const auto data_staging{join(staging_dir, "data")};
    const auto templates_staging{join(staging_dir, "templates")};
    if (auto directories{io::create_directories(data_staging)}; !directories) {
        return std::unexpected{directories.error()};
    }

    extraction_report report{};
    const auto &reader{jar->reader()};
    for (const auto &entry : reader.entries()) {
        if (entry.directory || !starts_with(entry.name, "data/")) {
            continue;
        }
        const std::string_view relative{entry.name.data() + 5,
                                        entry.name.size() - 5};
        const bool datapack{
            relative.find("/worldgen/") != std::string_view::npos ||
            relative.find("/tags/block/") != std::string_view::npos ||
            relative.find("/tags/worldgen/") != std::string_view::npos};
        const bool is_template{relative.find("/structure/") !=
                                   std::string_view::npos &&
                               ends_with(relative, ".nbt")};
        if (!datapack && !is_template) {
            continue;
        }
        if (!refrain::archive::safe_relative_path(relative)) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("the server archive has an unsafe data entry '{}'",
                            entry.name))};
        }
        auto bytes{reader.read(entry)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (datapack) {
            if (auto written{write_relative(data_staging, relative, *bytes)};
                !written) {
                return std::unexpected{written.error()};
            }
            ++report.datapack_files;
        } else {
            if (auto written{
                    write_relative(templates_staging, relative, *bytes)};
                !written) {
                return std::unexpected{written.error()};
            }
            ++report.templates;
        }
    }
    if (report.datapack_files == 0) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("the server archive for Minecraft {} holds no "
                        "worldgen data",
                        game_data_version))};
    }

    // Swap the staged trees into place. The marker is written after this, so a
    // crash between the two leaves a markerless (pre-staged) directory at
    // worst, never a marker that claims data that is not there.
    const auto data_target{join(root, "data")};
    std::filesystem::remove_all(std::string{data_target}, remove_error);
    if (remove_error) {
        return std::unexpected{
            make_error(error_code::io_error,
                       fmt::format("failed to replace '{}': {}", data_target,
                                   remove_error.message()))};
    }
    std::filesystem::rename(std::string{data_staging}, std::string{data_target},
                            remove_error);
    if (remove_error) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("failed to move the extracted datapack into '{}': {}",
                        data_target, remove_error.message()))};
    }
    if (report.templates > 0) {
        const auto templates_target{join(root, "templates")};
        std::filesystem::remove_all(std::string{templates_target},
                                    remove_error);
        remove_error.clear();
        std::filesystem::rename(std::string{templates_staging},
                                std::string{templates_target}, remove_error);
        if (remove_error) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("failed to move the extracted templates into "
                            "'{}': {}",
                            templates_target, remove_error.message()))};
        }
    }
    std::filesystem::remove_all(std::string{staging_dir}, remove_error);
    return report;
}

// --- marker write ----------------------------------------------------------

[[nodiscard]] auto write_marker(std::string_view root,
                                const archive_descriptor &descriptor,
                                std::uint64_t archive_bytes,
                                const extraction_report &report,
                                std::string_view data_digest) -> status {
    json::value::object_type members{};
    members.emplace("schema", json::value{game_data_marker_schema});
    members.emplace("game_version", json::value{game_data_version});
    members.emplace("data_version", json::value{static_cast<std::int64_t>(
                                        game_data_data_version)});
    members.emplace("datapack", json::value{"data"});
    members.emplace("source", json::value{"official-server-archive"});
    members.emplace("archive_sha1", json::value{ascii_lower(descriptor.sha1)});
    members.emplace("archive_bytes",
                    json::value{static_cast<std::int64_t>(archive_bytes)});
    members.emplace("entry_count", json::value{static_cast<std::int64_t>(
                                       report.datapack_files)});
    members.emplace("content_blake3", json::value{data_digest});
    json::value::object_type templates{};
    templates.emplace("root", json::value{"templates"});
    templates.emplace("entry_count",
                      json::value{static_cast<std::int64_t>(report.templates)});
    members.emplace("templates", json::value{std::move(templates)});
    json::value::array_type kinds{};
    kinds.emplace_back(json::value{"worldgen"});
    kinds.emplace_back(json::value{"tags/block"});
    kinds.emplace_back(json::value{"tags/worldgen"});
    members.emplace("kinds", json::value{std::move(kinds)});
    const json::value document{std::move(members)};
    const auto path{join(root, game_data_marker_name)};
    return io::atomic_write_text(path, document.dump_pretty() + "\n");
}

// --- resolution ------------------------------------------------------------

/// Resolve which data directory the run uses before acquiring anything.
[[nodiscard]] auto resolve_root(const game_data_options &options)
    -> result<std::string> {
    if (!options.data_dir.empty()) {
        return options.data_dir;
    }
    std::string working{options.working_directory};
    if (working.empty()) {
        auto current{io::current_path()};
        if (!current) {
            return std::unexpected{current.error()};
        }
        working = std::move(*current);
    }
    const auto default_root{join(working, "data")};
    auto marker{read_marker(default_root)};
    if (marker && cache_is_current(*marker, default_root)) {
        return default_root;
    }
    if (!options.installed_content_root.empty() &&
        has_worldgen_documents(join(options.installed_content_root, "data"))) {
        return options.installed_content_root;
    }
    return default_root;
}

/// Tracked tables and the protocol registry: the data directory's copy first,
/// then the installed content snapshot's, else empty so the runtime warns.
[[nodiscard]] auto tracked_file(std::string_view root,
                                std::string_view installed,
                                std::string_view name) -> std::string {
    const auto local{join(root, name)};
    if (io::is_regular_file(local)) {
        return local;
    }
    if (!installed.empty()) {
        const auto shared{join(installed, name)};
        if (io::is_regular_file(shared)) {
            return shared;
        }
    }
    return {};
}

/// The reports world generation reads are loaded as
/// `<datapack-root>/../reports` and `<root>/reports` is the source of truth. A
/// data directory that only holds a downloaded datapack receives them from the
/// installed content snapshot on first start; an incomplete local copy is
/// completed from the snapshot the same way.
[[nodiscard]] auto ensure_reports(std::string_view root,
                                  const game_data_options &options)
    -> result<std::string> {
    const auto target{join(root, "reports")};
    const auto installed_reports{
        options.installed_content_root.empty()
            ? std::string{}
            : join(options.installed_content_root, "reports")};
    const bool installed_available{!installed_reports.empty() &&
                                   io::is_directory(installed_reports)};
    bool copy{false};
    if (!io::is_directory(target)) {
        if (!installed_available) {
            log_game_data.warn(
                "found no biome parameter reports for Minecraft {}; world "
                "generation will report missing report documents",
                game_data_version);
            return std::string{};
        }
        copy = true;
    } else if (installed_available && root != options.installed_content_root) {
        // A partially populated local directory is completed from the
        // snapshot; an existing and complete copy stays untouched.
        copy = !io::is_regular_file(
            join(target, "biome_parameters/overworld.json"));
    }
    if (!copy) {
        return target;
    }
    if (auto copied{copy_tree(installed_reports, target)}; !copied) {
        return std::unexpected{copied.error()};
    }
    // The reports are game-derived data: carry the notice that accompanies the
    // tracked copy next to the new directory.
    const auto notice{join(io::parent_directory(options.installed_content_root),
                           "NOTICE.md")};
    if (io::is_regular_file(notice)) {
        auto bytes{io::read_file(notice)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (auto written{io::write_file(join(root, "NOTICE.md"), *bytes)};
            !written) {
            return std::unexpected{written.error()};
        }
    }
    const auto copied_files{list_files(installed_reports)};
    log_game_data.info(
        "copied {} report files from the installed content into '{}'",
        copied_files ? copied_files->size() : std::size_t{0}, target);
    return target;
}

/// The tracked artifacts one run resolved, with the source of each.
struct tracked_artifacts {
    std::string tables{};
    std::string protocol_registry{};
    std::string reports_root{};
    std::string biome_overworld{};
    std::string biome_nether{};
};

/// Biome reports the noise dimensions load when the first world is generated.
/// The end selects its biomes from island bands and reads no report.
constexpr std::array<std::string_view, 2> required_biome_reports{
    "biome_parameters/overworld.json", "biome_parameters/nether.json"};

[[nodiscard]] auto resolve_tracked_artifacts(std::string_view root,
                                             const game_data_options &options)
    -> result<tracked_artifacts> {
    tracked_artifacts artifacts{};
    artifacts.tables =
        tracked_file(root, options.installed_content_root, "tables.rvtb");
    artifacts.protocol_registry = tracked_file(
        root, options.installed_content_root, "protocol_registry.json");
    auto reports{ensure_reports(root, options)};
    if (!reports) {
        return std::unexpected{reports.error()};
    }
    artifacts.reports_root = std::move(*reports);
    if (!artifacts.reports_root.empty()) {
        const auto overworld{
            join(artifacts.reports_root, required_biome_reports[0])};
        if (io::is_regular_file(overworld)) {
            artifacts.biome_overworld = overworld;
        }
        const auto nether{
            join(artifacts.reports_root, required_biome_reports[1])};
        if (io::is_regular_file(nether)) {
            artifacts.biome_nether = nether;
        }
    }
    return artifacts;
}

/// One artifact a new world cannot be generated without.
struct missing_artifact {
    std::string expected{};
};

/// Required tracked artifacts that no source produced, in the order the
/// startup message names them. The protocol registry is shipped by the release
/// archive and cannot be derived from a server archive; a source checkout
/// carries the versioned copy the generation entry point installs.
[[nodiscard]] auto missing_artifacts(const tracked_artifacts &artifacts,
                                     std::string_view root)
    -> std::vector<missing_artifact> {
    std::vector<missing_artifact> missing{};
    if (artifacts.tables.empty()) {
        missing.push_back(
            missing_artifact{.expected = join(root, "tables.rvtb")});
    }
    if (artifacts.protocol_registry.empty()) {
        missing.push_back(
            missing_artifact{.expected = join(root, "protocol_registry.json")});
    }
    const auto reports_root{join(root, "reports")};
    if (artifacts.biome_overworld.empty()) {
        missing.push_back(missing_artifact{
            .expected = join(reports_root, required_biome_reports[0])});
    }
    if (artifacts.biome_nether.empty()) {
        missing.push_back(missing_artifact{
            .expected = join(reports_root, required_biome_reports[1])});
    }
    return missing;
}

[[nodiscard]] auto joined_paths(const std::vector<missing_artifact> &missing)
    -> std::string {
    std::string joined{};
    for (const auto &artifact : missing) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += "'" + artifact.expected + "'";
    }
    return joined;
}

/// Inputs the optional generation fallback needs, with the reason it is
/// unavailable when one is missing.
struct generation_plan {
    bool available{false};
    std::string script{};
    std::string jar{};
    std::string tools{};
    std::string java{};
    std::chrono::milliseconds timeout{};
    std::string problem{};
};

/// Detect the generation inputs: the source entry point, the tools binary and
/// a server archive. The Java runtime is checked by the entry point itself,
/// which reports a prerequisite exit code with its own diagnostic.
[[nodiscard]] auto plan_generation(const game_data_options &options,
                                   std::string_view downloaded_jar)
    -> generation_plan {
    generation_plan plan{.script = options.generation.script,
                         .tools = options.generation.tools,
                         .java = options.generation.java,
                         .timeout = options.generation.timeout};
    if (plan.script.empty() || !io::is_regular_file(plan.script)) {
        plan.problem = "this install carries no data generation entry point";
        return plan;
    }
    if (plan.tools.empty() || !io::is_regular_file(plan.tools)) {
        plan.problem = "refrain-tools is not installed next to the server";
        return plan;
    }
    if (!downloaded_jar.empty() && io::is_regular_file(downloaded_jar)) {
        plan.jar = std::string{downloaded_jar};
    } else if (!options.generation.server_jar.empty()) {
        if (!io::is_regular_file(options.generation.server_jar)) {
            plan.problem = fmt::format("the server archive '{}' does not exist",
                                       options.generation.server_jar);
            return plan;
        }
        plan.jar = options.generation.server_jar;
    } else {
        plan.problem =
            "no server archive was named, pass --server-jar <server.jar>";
        return plan;
    }
    plan.available = true;
    return plan;
}

/// Run the generation entry point for one data directory. The entry point
/// prints its own progress and diagnostics; the wall clock is bounded so a
/// stuck data generator cannot hang startup.
[[nodiscard]] auto run_generation(const generation_plan &plan,
                                  std::string_view root) -> status {
    std::vector<std::string> arguments{
        "--server-jar", plan.jar,   "--out",     std::string{root},
        "--tools",      plan.tools, "--version", target_version()};
    arguments.push_back("--data-version");
    arguments.push_back(std::to_string(game_data_data_version));
    // The server only generates when a required artifact is missing, so an
    // earlier partial run must be repairable without an operator flag.
    arguments.emplace_back("--force");
    if (!plan.java.empty()) {
        arguments.push_back("--java");
        arguments.push_back(plan.java);
    }
    log_game_data.info(
        "generating the tracked game data from '{}' into '{}', this can take "
        "a few minutes",
        plan.jar, root);
    auto run{refrain::platform::run_process(
        refrain::platform::subprocess_options{.program = plan.script,
                                              .arguments = std::move(arguments),
                                              .timeout = plan.timeout})};
    if (!run) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("failed to start the data generation entry point '{}': "
                        "{}",
                        plan.script, run.error().message))};
    }
    if (run->timed_out) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format(
                "data generation for Minecraft {} did not finish "
                "within {}s and was stopped",
                game_data_version,
                std::chrono::duration_cast<std::chrono::seconds>(plan.timeout)
                    .count()))};
    }
    if (run->exit_code != 0) {
        if (run->exit_code == 3) {
            return std::unexpected{make_error(
                error_code::unsupported,
                "the data generation prerequisites are missing, see the "
                "diagnostic above")};
        }
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("data generation for Minecraft {} failed with exit "
                        "code {}, see the output above",
                        game_data_version, run->exit_code))};
    }
    log_game_data.info("generated the tracked game data in '{}'", root);
    return {};
}

/// The one error a new world start returns when the tracked data no source
/// produced. It names the missing artifact and the command that creates it.
[[nodiscard]] auto
missing_game_data_error(const game_data_options &options, std::string_view root,
                        const std::vector<missing_artifact> &missing,
                        const std::optional<error> &generation_error) -> error {
    std::string message{fmt::format(
        "cannot create a world without the tracked Minecraft {} game data; "
        "missing: {}",
        game_data_version, joined_paths(missing))};
    if (generation_error.has_value()) {
        message += fmt::format("; the generation fallback failed: {}",
                               generation_error->message);
    }
    if (!options.generation.script.empty() &&
        io::is_regular_file(options.generation.script)) {
        message += fmt::format(
            "; generate it from your own server.jar with '{} --server-jar "
            "<server.jar> --out {}'",
            options.generation.script, root);
    } else {
        message += fmt::format(
            "; start from a complete release archive that ships "
            "'content/vanilla/{}', or generate the tables and reports from "
            "your own server.jar with 'tools/content/generate.sh --server-jar "
            "<server.jar> --out <data-dir>'",
            game_data_version);
    }
    if (!options.download_allowed) {
        message += "; data download is disabled by --no-data-download";
    }
    return make_error(error_code::not_found, std::move(message));
}

/// One line that names the origin of the data for the startup summary.
[[nodiscard]] auto source_detail(game_data_source source, std::string_view root)
    -> std::string {
    switch (source) {
    case game_data_source::downloaded:
        return fmt::format("downloaded from the official Minecraft {} server "
                           "archive and verified",
                           game_data_version);
    case game_data_source::cache:
        return fmt::format("reused from the local cache in '{}'", root);
    case game_data_source::pre_staged:
        return fmt::format("read from the pre-staged data directory '{}'",
                           root);
    case game_data_source::installed:
        return fmt::format("read from the installed content snapshot '{}'",
                           root);
    }
    return {};
}

/// Process-wide handoff from the composition root to the in-process runtime
/// provider. Startup is single-threaded up to the moment the runtime starts, so
/// the storage needs no synchronization; callers publish once.
std::optional<game_data_paths> g_published_paths{};

} // namespace

void publish_game_data_paths(const game_data_paths &paths) {
    g_published_paths = paths;
}

auto published_game_data_paths() noexcept
    -> const std::optional<game_data_paths> & {
    return g_published_paths;
}

void clear_game_data_paths() noexcept { g_published_paths.reset(); }

auto to_string(game_data_source source) noexcept -> std::string_view {
    switch (source) {
    case game_data_source::downloaded:
        return "downloaded";
    case game_data_source::cache:
        return "cache";
    case game_data_source::pre_staged:
        return "pre-staged";
    case game_data_source::installed:
        return "installed";
    }
    return "unknown";
}

auto ensure_game_data(const game_data_options &options,
                      net::download_transport *transport,
                      const game_data_limits &limits)
    -> result<game_data_paths> {
    auto root{resolve_root(options)};
    if (!root) {
        return std::unexpected{root.error()};
    }
    game_data_source source{game_data_source::cache};
    bool fetch_required{false};
    if (*root == options.installed_content_root) {
        source = game_data_source::installed;
    } else {
        auto marker{read_marker(*root)};
        if (!marker) {
            return std::unexpected{marker.error()};
        }
        if (cache_is_current(*marker, *root)) {
            source = game_data_source::cache;
        } else if (marker->present) {
            // A marker that names another version (or cannot be trusted) is
            // stale: replace it when the network is allowed.
            if (!options.download_allowed) {
                return std::unexpected{make_error(
                    error_code::version_mismatch,
                    fmt::format(
                        "the game data in '{}' is not usable for Minecraft {}: "
                        "{}; data download is disabled, stage a matching data "
                        "directory or drop --no-data-download",
                        *root, game_data_version,
                        marker->valid
                            ? fmt::format(
                                  "the marker records game version {} data "
                                  "version {}",
                                  marker->game_version, marker->data_version)
                            : marker->problem))};
            }
            fetch_required = true;
        } else if (has_worldgen_documents(join(*root, "data"))) {
            // A datapack directory staged by hand or by the content script:
            // accept it and say so, without inventing a marker.
            source = game_data_source::pre_staged;
        } else {
            fetch_required = true;
        }
    }

    /// A downloaded archive is kept until the generation fallback has read it;
    /// every early return below removes it first.
    std::string downloaded_jar{};
    const auto remove_downloaded = [&downloaded_jar] {
        if (downloaded_jar.empty()) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(downloaded_jar, ignored);
        downloaded_jar.clear();
    };

    std::optional<error> generation_error{};
    bool generated{false};
    /// Try the optional generation fallback once. The returned flag reports
    /// whether the entry point produced the data; a failure is remembered for
    /// the startup diagnostic and never thrown away silently.
    const auto attempt_generation = [&]() -> bool {
        const auto plan{plan_generation(options, downloaded_jar)};
        if (!plan.available) {
            return false;
        }
        auto result{run_generation(plan, *root)};
        if (!result) {
            generation_error = result.error();
            log_game_data.warn("generating the tracked game data failed: {}",
                               result.error().message);
            return false;
        }
        generated = true;
        return true;
    };

    /// Why the generation fallback could not run, for the startup error when
    /// the caller asked for a server archive. A failed run already carries the
    /// entry point's diagnostic.
    const auto generation_detail = [&]() -> std::string {
        if (generation_error.has_value()) {
            return fmt::format("; the generation fallback failed: {}",
                               generation_error->message);
        }
        if (!options.generation.server_jar.empty()) {
            const auto plan{plan_generation(options, downloaded_jar)};
            if (!plan.available && !plan.problem.empty()) {
                return fmt::format("; {}", plan.problem);
            }
        }
        return {};
    };

    if (fetch_required) {
        const bool can_download{options.download_allowed &&
                                transport != nullptr};
        if (!can_download) {
            // The operator's own server archive produces the datapack as well
            // as the tracked data, so the generation fallback is the only way
            // to start offline without a staged directory.
            if (attempt_generation()) {
                if (!has_worldgen_documents(join(*root, "data"))) {
                    return std::unexpected{make_error(
                        error_code::io_error,
                        fmt::format("the data generation entry point produced "
                                    "no datapack in '{}'",
                                    join(*root, "data")))};
                }
                auto marker{read_marker(*root)};
                source = marker && cache_is_current(*marker, *root)
                             ? game_data_source::cache
                             : game_data_source::pre_staged;
            } else if (options.download_allowed) {
                return std::unexpected{make_error(
                    error_code::unsupported,
                    fmt::format(
                        "data download is enabled but this build has no HTTPS "
                        "transport; stage the data directory or pass "
                        "--server-jar <server.jar> to generate it{}",
                        generation_detail()))};
            } else {
                return std::unexpected{make_error(
                    error_code::not_found,
                    fmt::format(
                        "found no game data for Minecraft {} in '{}': '{}' is "
                        "missing and data download is disabled; stage the data "
                        "directory, pass --server-jar <server.jar> to generate "
                        "it, or run without --no-data-download{}",
                        game_data_version, *root, join(*root, "data"),
                        generation_detail()))};
            }
        } else {
            if (transport == nullptr) {
                return std::unexpected{make_error(
                    error_code::unsupported,
                    "data download is enabled but this build has no HTTPS "
                    "transport")};
            }
            log_game_data.info(
                "found no game data in '{}', downloading the Minecraft {} "
                "server archive",
                *root, game_data_version);
            auto descriptor{select_archive(*transport, options, limits)};
            if (!descriptor) {
                return std::unexpected{descriptor.error()};
            }
            const auto temporary{join(*root, ".server.jar.tmp")};
            if (auto directories{io::create_directories(*root)}; !directories) {
                return std::unexpected{directories.error()};
            }
            auto downloaded{
                download_archive(*descriptor, temporary, *transport, limits)};
            if (!downloaded) {
                std::error_code ignored;
                std::filesystem::remove(std::string{temporary}, ignored);
                return std::unexpected{downloaded.error()};
            }
            auto installed{install_from_archive(temporary, *root)};
            if (!installed) {
                std::error_code ignored;
                std::filesystem::remove(std::string{temporary}, ignored);
                return std::unexpected{installed.error()};
            }
            auto digest{tree_digest(join(*root, "data"))};
            if (!digest) {
                std::error_code ignored;
                std::filesystem::remove(std::string{temporary}, ignored);
                return std::unexpected{digest.error()};
            }
            if (auto marker{write_marker(*root, *descriptor, *downloaded,
                                         *installed, *digest)};
                !marker) {
                std::error_code ignored;
                std::filesystem::remove(std::string{temporary}, ignored);
                return std::unexpected{marker.error()};
            }
            // Keep the verified archive until the tracked data is resolved:
            // when the install carries no content snapshot the generation
            // fallback derives the tables and reports from this same archive.
            downloaded_jar = temporary;
            source = game_data_source::downloaded;
            log_game_data.info(
                "verified the archive SHA-1, extracted {} datapack files and "
                "{} structure templates, and wrote '{}'",
                installed->datapack_files, installed->templates,
                join(*root, game_data_marker_name));
        }
    } else if (source == game_data_source::cache) {
        log_game_data.info("using cached game data for Minecraft {} from '{}'",
                           game_data_version, *root);
    } else if (source == game_data_source::installed) {
        log_game_data.info(
            "using the installed game data snapshot for Minecraft {} from '{}'",
            game_data_version, *root);
    } else {
        log_game_data.info(
            "using pre-staged game data for Minecraft {} from '{}'",
            game_data_version, *root);
    }

    auto artifacts{resolve_tracked_artifacts(*root, options)};
    if (!artifacts) {
        remove_downloaded();
        return std::unexpected{artifacts.error()};
    }
    auto missing{missing_artifacts(*artifacts, *root)};
    if (!missing.empty()) {
        if (!generated) {
            static_cast<void>(attempt_generation());
        }
        // A run that reported a failure can still have written part of the
        // data before the failing step; resolve again so only what is actually
        // missing decides the start.
        auto refreshed{resolve_tracked_artifacts(*root, options)};
        if (!refreshed) {
            remove_downloaded();
            return std::unexpected{refreshed.error()};
        }
        artifacts = std::move(*refreshed);
        missing = missing_artifacts(*artifacts, *root);
    }
    remove_downloaded();
    if (!missing.empty() && options.require_tracked_artifacts) {
        return std::unexpected{
            missing_game_data_error(options, *root, missing, generation_error)};
    }

    game_data_paths paths{};
    paths.source = source;
    paths.root = *root;
    paths.datapack_root = join(*root, "data");
    const auto templates{join(*root, "templates")};
    paths.templates_root =
        io::is_directory(templates) ? templates : std::string{};
    paths.tables_path = std::move(artifacts->tables);
    paths.protocol_registry_path = std::move(artifacts->protocol_registry);
    paths.reports_root = std::move(artifacts->reports_root);
    paths.game_version = target_version();
    paths.data_version = game_data_data_version;
    paths.detail = source_detail(source, *root);
    return paths;
}

} // namespace refrain::server
