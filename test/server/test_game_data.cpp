// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/json.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/download.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/server/game_data.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <fmt/format.h>
#include <zip.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;
using refrain::server::ensure_game_data;
using refrain::server::game_data_options;
using refrain::server::game_data_source;

namespace fs = std::filesystem;

/// Compare a returned path string with an expected path independently of the
/// platform's directory separator: Windows stores `\`, while the layout and
/// data helpers return the canonical `/` form.
[[nodiscard]] auto same_path(std::string_view actual,
                             const std::filesystem::path &expected) -> bool {
    return std::filesystem::path{std::string{actual}}
               .lexically_normal()
               .generic_string() ==
           expected.lexically_normal().generic_string();
}

namespace io = refrain::core::io;

constexpr std::string_view manifest_url{
    "https://example.invalid/version_manifest_v2.json"};
constexpr std::string_view version_url{"https://example.invalid/26.3.json"};
constexpr std::string_view archive_url{"https://example.invalid/server.jar"};

// --- synthetic archives ----------------------------------------------------

struct archive_file {
    std::string name;
    std::string content;
};

/// Write a synthetic zip; tests never ship a real Minecraft archive.
[[nodiscard]] auto make_zip(const fs::path &path,
                            const std::vector<archive_file> &files) -> bool {
    int error_code_value{0};
    auto *archive{zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE,
                           &error_code_value)};
    if (archive == nullptr) {
        return false;
    }
    for (const auto &file : files) {
        auto *source{zip_source_buffer(archive, file.content.data(),
                                       file.content.size(), 0)};
        if (source == nullptr) {
            zip_discard(archive);
            return false;
        }
        const auto index{zip_file_add(archive, file.name.c_str(), source,
                                      ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE)};
        if (index < 0) {
            zip_source_free(source);
            zip_discard(archive);
            return false;
        }
    }
    return zip_close(archive) == 0;
}

[[nodiscard]] auto read_bytes(const fs::path &path) -> std::string {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] auto sample_data_files() -> std::vector<archive_file> {
    return {
        {.name = "data/minecraft/worldgen/biome/plains.json",
         .content = R"({"temperature":0.8})"},
        {.name = "data/minecraft/worldgen/noise/continents.json",
         .content = R"({"firstOctave":-11})"},
        {.name = "data/minecraft/tags/block/dirt.json",
         .content = R"({"values":["minecraft:dirt"]})"},
        {.name = "data/minecraft/tags/worldgen/biome/forest.json",
         .content = R"({"values":["minecraft:forest"]})"},
        {.name = "data/minecraft/structure/village/plains/house.nbt",
         .content = "synthetic-template-bytes"},
        {.name = "data/minecraft/recipe/stone.json",
         .content = R"({"type":"minecraft:crafting_shaped"})"},
        {.name = "data/minecraft/loot_table/blocks/stone.json",
         .content = R"({"type":"minecraft:block"})"},
        {.name = "assets/minecraft/texts/en_us.txt", .content = "hello"},
        {.name = "version.json", .content = R"({"id":"26.3"})"},
    };
}

/// A server archive that carries `data/` at its root.
[[nodiscard]] auto make_plain_jar(const fs::path &path) -> bool {
    return make_zip(path, sample_data_files());
}

/// A bundler archive: the data tree lives in
/// `META-INF/versions/26.3/server-26.3.jar`.
[[nodiscard]] auto make_bundler_jar(const fs::path &path,
                                    const fs::path &scratch) -> bool {
    const auto inner_path{scratch / "inner-server.jar"};
    if (!make_plain_jar(inner_path)) {
        return false;
    }
    const auto inner_bytes{read_bytes(inner_path)};
    return make_zip(path, {
                              {.name = "version.json",
                               .content = R"({"id":"26.3","name":"26.3"})"},
                              {.name = "META-INF/MANIFEST.MF",
                               .content = "Manifest-Version: 1.0\n"},
                              {.name = "META-INF/versions/26.3/server-26.3.jar",
                               .content = inner_bytes},
                          });
}

[[nodiscard]] auto sha1_hex(std::string_view bytes) -> std::string {
    const auto digest{refrain::net::sha1(bytes)};
    expect(digest.has_value());
    if (!digest) {
        return {};
    }
    return refrain::data::to_hex(
        std::span<const std::byte>{digest->data(), digest->size()});
}

[[nodiscard]] auto version_document(std::string_view sha1,
                                    std::string_view bytes) -> std::string {
    return fmt::format(
        R"({{"id":"26.3","downloads":{{"server":{{"sha1":"{}","size":{},"url":"{}"}}}}}})",
        sha1, bytes.size(), archive_url);
}

[[nodiscard]] auto manifest_document() -> std::string {
    return fmt::format(
        R"({{"latest":{{"release":"26.3","snapshot":"26.3"}},"versions":[)"
        R"({{"id":"25.0","url":"https://example.invalid/25.0.json"}},)"
        R"({{"id":"26.3","url":"{}"}}]}})",
        version_url);
}

/// Fake transport: serves the metadata documents and one local archive file.
class fake_transport final : public refrain::net::download_transport {
  public:
    fake_transport(std::string manifest, std::string version, fs::path archive)
        : manifest_{std::move(manifest)}, version_{std::move(version)},
          archive_{std::move(archive)} {}

    [[nodiscard]] auto fetch_text(std::string_view url, std::size_t max_bytes,
                                  std::chrono::milliseconds /*timeout*/)
        -> refrain::result<std::string> override {
        ++text_calls;
        if (max_bytes == 0) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::invalid_argument, "zero limit")};
        }
        if (url == manifest_url) {
            return manifest_;
        }
        if (url == version_url) {
            return version_;
        }
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            fmt::format("fake transport has no document for '{}'", url))};
    }

    [[nodiscard]] auto download(const refrain::net::download_request &request,
                                refrain::net::download_sink &sink)
        -> refrain::result<refrain::net::download_result> override {
        ++download_calls;
        const auto bytes{read_bytes(archive_)};
        if (bytes.size() > request.max_bytes) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::io_error, "archive exceeds the limit")};
        }
        const auto chunk{std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()}};
        if (auto written{sink.write(chunk)}; !written) {
            return std::unexpected{written.error()};
        }
        refrain::net::download_result result{};
        result.bytes = bytes.size();
        const auto digest{refrain::net::sha1(bytes)};
        if (!digest) {
            return std::unexpected{digest.error()};
        }
        result.sha1 = *digest;
        return result;
    }

    int text_calls{0};
    int download_calls{0};

  private:
    std::string manifest_;
    std::string version_;
    fs::path archive_;
};

void write_text(const fs::path &path, std::string_view text) {
    expect(io::create_directories(io::parent_directory(path.string()))
               .has_value());
    const auto written = io::write_text(path.string(), text);
    expect(written.has_value());
}

[[nodiscard]] auto read_text(const fs::path &path) -> std::string {
    auto text = io::read_text(path.string());
    expect(text.has_value());
    return text ? *text : std::string{};
}

/// The platform has no portable compile-time OS query in this test tree; the
/// executable suffix distinguishes the Windows backend. The generation entry
/// point is a POSIX shell script, so its tests skip on Windows.
[[nodiscard]] auto on_windows() -> bool {
    const auto executable{refrain::platform::executable_path()};
    if (!executable) {
        return false;
    }
    const auto &path{*executable};
    return path.size() >= 4 &&
           (path.ends_with(".exe") || path.ends_with(".EXE"));
}

/// Stage a datapack tree the resolver accepts as pre-staged.
void stage_datapack(const fs::path &data_dir) {
    write_text(data_dir / "data/minecraft/worldgen/biome/plains.json",
               R"({"temperature":0.8})");
}

/// Write a synthetic generation entry point. It records its arguments and
/// produces the two tracked artifacts the startup path validates.
void write_generation_script(const fs::path &path) {
    write_text(path, R"SH(#!/bin/sh
all="$*"
out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        *) shift ;;
    esac
done
if [ -z "$out" ]; then
    exit 2
fi
mkdir -p "$out/reports/biome_parameters"
printf '%s\n' "$all" > "$out/generation-args.txt"
printf 'tables' > "$out/tables.rvtb"
printf '{}' > "$out/protocol_registry.json"
printf '{}\n' > "$out/reports/biome_parameters/overworld.json"
printf '{}\n' > "$out/reports/biome_parameters/nether.json"
files=$(find "$out/data" -type f | wc -l)
printf '{"game_version":"26.3","data_version":5023,"datapack":"data","entry_count":%s}\n' \
    "$files" > "$out/MANIFEST.json"
)SH");
    std::error_code error;
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::owner_exec,
                    fs::perm_options::add, error);
    expect(!error) << "cannot make the generation script executable";
}

/// The generation inputs for the synthetic entry point: a script, a jar and a
/// tools binary only need to exist as regular files.
[[nodiscard]] auto generation_options(const fs::path &script,
                                      const fs::path &jar,
                                      const fs::path &tools)
    -> game_data_options {
    return game_data_options{
        .generation = {.script = script.string(),
                       .server_jar = jar.string(),
                       .tools = tools.string(),
                       .timeout = std::chrono::seconds{30}}};
}

[[nodiscard]] auto data_options(const fs::path &working,
                                const fs::path &data_dir,
                                const fs::path &installed_content = {})
    -> game_data_options {
    return game_data_options{.data_dir = data_dir.string(),
                             .installed_content_root =
                                 installed_content.string(),
                             .download_allowed = true,
                             .manifest_url = std::string{manifest_url},
                             .working_directory = working.string()};
}

[[nodiscard]] auto marker_path(const fs::path &data_dir) -> fs::path {
    return data_dir / "MANIFEST.json";
}

} // namespace

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

suite<"server/game-data"> game_data_tests = [] {
    "game-data/first start downloads, verifies and extracts the datapack"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto archive{scratch.path() / "server.jar"};
            expect(make_plain_jar(archive));
            const auto archive_bytes{read_bytes(archive)};
            fake_transport transport{
                manifest_document(),
                version_document(sha1_hex(archive_bytes), archive_bytes),
                archive};
            const auto data_dir{scratch.path() / "data"};
            const auto paths = ensure_game_data(
                data_options(scratch.path(), data_dir), &transport);
            expect(paths.has_value()) << "download failed";
            if (!paths) {
                return;
            }
            expect(paths->source == game_data_source::downloaded);
            expect(transport.text_calls == 2);
            expect(transport.download_calls == 1);
            expect(same_path(paths->datapack_root, data_dir / "data"));
            expect(fs::is_regular_file(
                data_dir / "data/minecraft/worldgen/biome/plains.json"));
            expect(fs::is_regular_file(data_dir /
                                       "data/minecraft/tags/block/dirt.json"));
            expect(fs::is_regular_file(
                data_dir / "data/minecraft/tags/worldgen/biome/forest.json"));
            // Only the generator's data is unpacked; recipes, loot tables,
            // assets and the archive itself stay out of the data directory.
            expect(!fs::exists(data_dir / "data/minecraft/recipe/stone.json"));
            expect(!fs::exists(data_dir /
                               "data/minecraft/loot_table/blocks/stone.json"));
            expect(!fs::exists(data_dir / "assets"));
            expect(!fs::exists(data_dir / ".server.jar.tmp"));
            expect(fs::is_regular_file(data_dir /
                                       "templates/minecraft/structure/village/"
                                       "plains/house.nbt"));
            expect(fs::is_regular_file(marker_path(data_dir)));
            expect(paths->game_version == "26.3");
            expect(paths->data_version == 5023);
            expect(!paths->detail.empty());
        };

    "game-data/the marker records version, archive identity and digest"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto archive{scratch.path() / "server.jar"};
            expect(make_plain_jar(archive));
            const auto archive_bytes{read_bytes(archive)};
            fake_transport transport{
                manifest_document(),
                version_document(sha1_hex(archive_bytes), archive_bytes),
                archive};
            const auto data_dir{scratch.path() / "data"};
            const auto paths = ensure_game_data(
                data_options(scratch.path(), data_dir), &transport);
            expect(paths.has_value());
            if (!paths) {
                return;
            }
            const auto marker_text{read_text(marker_path(data_dir))};
            expect(marker_text.find(sha1_hex(archive_bytes)) !=
                   std::string::npos);
            const auto marker{refrain::data::json::value::parse(marker_text)};
            expect(marker.has_value());
            if (!marker) {
                return;
            }
            expect(marker->find("schema") != nullptr);
            expect(marker->find("game_version") != nullptr);
            expect(marker->find("data_version") != nullptr);
            expect(marker->find("archive_sha1") != nullptr);
            expect(marker->find("content_blake3") != nullptr);
            expect(marker->find("templates") != nullptr);
            if (const auto *count{marker->find("entry_count")};
                count != nullptr) {
                const auto value{count->as_int64()};
                expect(value.has_value() && *value == 4);
            }
        };

    "game-data/a cached directory is reused without the network"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        const auto archive_bytes{read_bytes(archive)};
        const auto data_dir{scratch.path() / "data"};
        {
            fake_transport transport{
                manifest_document(),
                version_document(sha1_hex(archive_bytes), archive_bytes),
                archive};
            const auto first = ensure_game_data(
                data_options(scratch.path(), data_dir), &transport);
            expect(first.has_value());
        }
        // A transport whose very first call would be counted: reusing the
        // cache must not issue a single request.
        const auto unavailable{scratch.path() / "missing.jar"};
        fake_transport transport{manifest_document(), "", unavailable};
        const auto second = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(second.has_value());
        if (!second) {
            return;
        }
        expect(second->source == game_data_source::cache);
        expect(transport.text_calls == 0);
        expect(transport.download_calls == 0);
    };

    "game-data/a bundler archive is unwrapped before extraction"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_bundler_jar(archive, scratch.path()));
        const auto archive_bytes{read_bytes(archive)};
        fake_transport transport{
            manifest_document(),
            version_document(sha1_hex(archive_bytes), archive_bytes), archive};
        const auto data_dir{scratch.path() / "data"};
        const auto paths = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(paths.has_value()) << "bundler download failed";
        if (!paths) {
            return;
        }
        expect(paths->source == game_data_source::downloaded);
        expect(fs::is_regular_file(
            data_dir / "data/minecraft/worldgen/biome/plains.json"));
        expect(fs::is_regular_file(data_dir /
                                   "templates/minecraft/structure/village/"
                                   "plains/house.nbt"));
    };

    "game-data/a SHA-1 mismatch fails closed"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        const auto archive_bytes{read_bytes(archive)};
        fake_transport transport{
            manifest_document(),
            version_document(std::string(40, 'a'), archive_bytes), archive};
        const auto data_dir{scratch.path() / "data"};
        const auto paths = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(!paths.has_value());
        if (!paths) {
            expect(paths.error().message.find("SHA-1") != std::string::npos);
        }
        expect(!fs::exists(marker_path(data_dir)));
        expect(!fs::exists(data_dir / "data"));
        expect(!fs::exists(data_dir / ".server.jar.tmp"));
    };

    "game-data/a declared size mismatch fails closed"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        const auto archive_bytes{read_bytes(archive)};
        const auto version{fmt::format(
            R"({{"id":"26.3","downloads":{{"server":{{"sha1":"{}","size":{},"url":"{}"}}}}}})",
            sha1_hex(archive_bytes), archive_bytes.size() + 1, archive_url)};
        fake_transport transport{manifest_document(), version, archive};
        const auto data_dir{scratch.path() / "data"};
        const auto paths = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(!paths.has_value());
        if (!paths) {
            expect(paths.error().message.find("manifest declares") !=
                   std::string::npos);
        }
    };

    "game-data/a manifest without the target version fails naming it"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        fake_transport transport{
            R"({"latest":{"release":"25.0"},"versions":[{"id":"25.0","url":"https://example.invalid/25.0.json"}]})",
            "", archive};
        const auto data_dir{scratch.path() / "data"};
        const auto paths = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(!paths.has_value());
        if (!paths) {
            expect(paths.error().code == refrain::error_code::version_mismatch);
            expect(paths.error().message.find("26.3") != std::string::npos);
        }
    };

    "game-data/a client-only version has no server archive"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        fake_transport transport{manifest_document(),
                                 R"({"id":"26.3","downloads":{}})", archive};
        const auto data_dir{scratch.path() / "data"};
        const auto paths = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(!paths.has_value());
        if (!paths) {
            expect(paths.error().message.find("no server archive") !=
                   std::string::npos);
        }
    };

    "game-data/offline with no data fails with an actionable error"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        auto options = data_options(scratch.path(), scratch.path() / "data");
        options.download_allowed = false;
        const auto paths = ensure_game_data(options, nullptr);
        expect(!paths.has_value());
        if (!paths) {
            expect(paths.error().code == refrain::error_code::not_found);
            expect(paths.error().message.find("--no-data-download") !=
                   std::string::npos);
        }
    };

    "game-data/a pre-staged datapack is accepted without a marker"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto data_dir{scratch.path() / "data"};
        expect(io::create_directories(
                   (data_dir / "data/minecraft/worldgen").string())
                   .has_value());
        write_text(data_dir / "data/minecraft/worldgen/biome/plains.json",
                   R"({"temperature":0.8})");
        auto options = data_options(scratch.path(), data_dir);
        options.download_allowed = false;
        const auto paths = ensure_game_data(options, nullptr);
        expect(paths.has_value());
        if (!paths) {
            return;
        }
        expect(paths->source == game_data_source::pre_staged);
        expect(same_path(paths->datapack_root, data_dir / "data"));
    };

    "game-data/a fetch-content marker is recognised as a local cache"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto data_dir{scratch.path() / "data"};
        expect(io::create_directories(
                   (data_dir / "data/minecraft/worldgen").string())
                   .has_value());
        write_text(data_dir / "data/minecraft/worldgen/biome/plains.json",
                   R"({"temperature":0.8})");
        // The layout `scripts/fetch-content.sh` writes: a marker without
        // our schema field but with the same version information.
        write_text(
            marker_path(data_dir),
            R"({"game_version":"26.3","data_version":5023,"datapack":"data","content_sha256":"00","entry_count":1,"kinds":["worldgen"],"source":"server.jar datapack"})");
        auto options = data_options(scratch.path(), data_dir);
        options.download_allowed = false;
        const auto paths = ensure_game_data(options, nullptr);
        expect(paths.has_value());
        if (paths) {
            expect(paths->source == game_data_source::cache);
        }
    };

    "game-data/a stale marker is a version mismatch when offline"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto data_dir{scratch.path() / "data"};
        expect(io::create_directories(
                   (data_dir / "data/minecraft/worldgen").string())
                   .has_value());
        write_text(
            marker_path(data_dir),
            R"({"schema":"refrain.game-data/1","game_version":"25.0","data_version":4700})");
        auto options = data_options(scratch.path(), data_dir);
        options.download_allowed = false;
        const auto paths = ensure_game_data(options, nullptr);
        expect(!paths.has_value());
        if (!paths) {
            expect(paths.error().code == refrain::error_code::version_mismatch);
            expect(paths.error().message.find("25.0") != std::string::npos);
        }
    };

    "game-data/a stale marker is replaced when downloads are allowed"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        const auto archive_bytes{read_bytes(archive)};
        const auto data_dir{scratch.path() / "data"};
        expect(io::create_directories(
                   (data_dir / "data/minecraft/worldgen").string())
                   .has_value());
        write_text(
            marker_path(data_dir),
            R"({"schema":"refrain.game-data/1","game_version":"25.0","data_version":4700})");
        fake_transport transport{
            manifest_document(),
            version_document(sha1_hex(archive_bytes), archive_bytes), archive};
        const auto paths = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(paths.has_value());
        if (!paths) {
            return;
        }
        expect(paths->source == game_data_source::downloaded);
        expect(transport.download_calls == 1);
        const auto marker_text{read_text(marker_path(data_dir))};
        const auto marker{refrain::data::json::value::parse(marker_text)};
        expect(marker.has_value());
        if (marker) {
            const auto *version{marker->find("game_version")};
            expect(version != nullptr);
            if (version != nullptr) {
                const auto text{version->as_string()};
                expect(text.has_value() && *text == "26.3");
            }
        }
    };

    "game-data/a downloaded directory receives the installed reports"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto installed{scratch.path() / "content/vanilla/26.3"};
            expect(io::create_directories(
                       (installed / "reports/biome_parameters").string())
                       .has_value());
            write_text(installed / "reports/biome_parameters/overworld.json",
                       R"({"entries":[]})");
            write_text(installed / "reports/biome_parameters/nether.json",
                       R"({"entries":[]})");
            write_text(installed / "tables.rvtb", "tables");
            write_text(installed / "protocol_registry.json", "{}");
            write_text(scratch.path() / "content/vanilla/NOTICE.md",
                       "notice\n");
            const auto archive{scratch.path() / "server.jar"};
            expect(make_plain_jar(archive));
            const auto archive_bytes{read_bytes(archive)};
            fake_transport transport{
                manifest_document(),
                version_document(sha1_hex(archive_bytes), archive_bytes),
                archive};
            const auto data_dir{scratch.path() / "data"};
            const auto paths = ensure_game_data(
                data_options(scratch.path(), data_dir, installed), &transport);
            expect(paths.has_value());
            if (!paths) {
                return;
            }
            expect(fs::is_regular_file(
                data_dir / "reports/biome_parameters/overworld.json"));
            expect(fs::is_regular_file(data_dir / "NOTICE.md"));
            // Tracked tables are read from the installed content root when the
            // data directory has no copy.
            expect(same_path(paths->tables_path, installed / "tables.rvtb"));
            expect(same_path(paths->protocol_registry_path,
                             installed / "protocol_registry.json"));
        };

    "game-data/installed content snapshot is used when no data exists"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto installed{scratch.path() / "content/vanilla/26.3"};
            expect(io::create_directories(
                       (installed / "data/minecraft/worldgen").string())
                       .has_value());
            write_text(installed / "data/minecraft/worldgen/biome/plains.json",
                       R"({"temperature":0.8})");
            expect(io::create_directories((installed / "reports").string())
                       .has_value());
            write_text(installed / "reports/placeholder.json", "{}");
            const auto working{scratch.path() / "empty"};
            expect(io::create_directories(working.string()).has_value());
            auto options = data_options(working, {}, installed);
            options.download_allowed = false;
            const auto paths = ensure_game_data(options, nullptr);
            expect(paths.has_value());
            if (!paths) {
                return;
            }
            expect(paths->source == game_data_source::installed);
            expect(same_path(paths->root, installed));
            expect(same_path(paths->datapack_root, installed / "data"));
        };

    "game-data/a data directory copy wins over the installed tables"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto installed{scratch.path() / "content/vanilla/26.3"};
        expect(io::create_directories(installed.string()).has_value());
        write_text(installed / "tables.rvtb", "installed-tables");
        write_text(installed / "protocol_registry.json", "installed");
        const auto data_dir{scratch.path() / "data"};
        expect(io::create_directories(
                   (data_dir / "data/minecraft/worldgen").string())
                   .has_value());
        write_text(data_dir / "tables.rvtb", "local-tables");
        write_text(data_dir / "protocol_registry.json", "local");
        write_text(
            marker_path(data_dir),
            R"({"schema":"refrain.game-data/1","game_version":"26.3","data_version":5023})");
        auto options = data_options(scratch.path(), data_dir, installed);
        options.download_allowed = false;
        const auto paths = ensure_game_data(options, nullptr);
        expect(paths.has_value());
        if (!paths) {
            return;
        }
        expect(paths->source == game_data_source::cache);
        expect(same_path(paths->tables_path, data_dir / "tables.rvtb"));
        expect(same_path(paths->protocol_registry_path,
                         data_dir / "protocol_registry.json"));
    };

    "game-data/entries outside the datapack are not extracted"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        const auto archive_bytes{read_bytes(archive)};
        fake_transport transport{
            manifest_document(),
            version_document(sha1_hex(archive_bytes), archive_bytes), archive};
        const auto data_dir{scratch.path() / "data"};
        const auto paths = ensure_game_data(
            data_options(scratch.path(), data_dir), &transport);
        expect(paths.has_value());
        if (!paths) {
            return;
        }
        std::vector<std::string> extracted{};
        for (const auto &entry :
             fs::recursive_directory_iterator{data_dir / "data"}) {
            if (entry.is_regular_file()) {
                extracted.push_back(entry.path().generic_string());
            }
        }
        expect(extracted.size() == 4U);
        for (const auto &path : extracted) {
            expect(path.find("/worldgen/") != std::string::npos ||
                   path.find("/tags/block/") != std::string::npos ||
                   path.find("/tags/worldgen/") != std::string::npos);
            expect(path.find("/recipe/") == std::string::npos);
            expect(path.find("/loot_table/") == std::string::npos);
        }
    };

    "game-data/a new world without tracked data fails naming it and the command"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto data_dir{scratch.path() / "data"};
            stage_datapack(data_dir);
            auto options = data_options(scratch.path(), data_dir);
            options.download_allowed = false;
            options.require_tracked_artifacts = true;
            const auto paths = ensure_game_data(options, nullptr);
            expect(!paths.has_value());
            if (!paths) {
                expect(paths.error().code == refrain::error_code::not_found);
                const auto &message{paths.error().message};
                expect(message.find("tables.rvtb") != std::string::npos)
                    << message;
                expect(message.find("biome_parameters/overworld.json") !=
                       std::string::npos)
                    << message;
                expect(message.find("biome_parameters/nether.json") !=
                       std::string::npos)
                    << message;
                expect(message.find("protocol_registry.json") !=
                       std::string::npos)
                    << message;
                expect(message.find("tools/content/generate.sh") !=
                       std::string::npos)
                    << message;
                expect(message.find("--server-jar") != std::string::npos)
                    << message;
            }
        };

    "game-data/without the requirement the degraded resolution is kept"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto data_dir{scratch.path() / "data"};
            stage_datapack(data_dir);
            auto options = data_options(scratch.path(), data_dir);
            options.download_allowed = false;
            const auto paths = ensure_game_data(options, nullptr);
            expect(paths.has_value());
            if (paths) {
                expect(paths->source == game_data_source::pre_staged);
                expect(paths->tables_path.empty());
                expect(!fs::exists(data_dir / "reports"));
            }
        };

    "game-data/a complete installed snapshot satisfies a new world"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto installed{scratch.path() / "content/vanilla/26.3"};
        write_text(installed / "tables.rvtb", "installed-tables");
        write_text(installed / "protocol_registry.json", "installed-registry");
        write_text(installed / "reports/biome_parameters/overworld.json", "{}");
        write_text(installed / "reports/biome_parameters/nether.json", "{}");
        const auto data_dir{scratch.path() / "data"};
        stage_datapack(data_dir);
        auto options = data_options(scratch.path(), data_dir, installed);
        options.download_allowed = false;
        options.require_tracked_artifacts = true;
        const auto paths = ensure_game_data(options, nullptr);
        expect(paths.has_value()) << (paths ? "" : paths.error().message);
        if (!paths) {
            return;
        }
        expect(paths->source == game_data_source::pre_staged);
        expect(same_path(paths->tables_path, installed / "tables.rvtb"));
        expect(same_path(paths->protocol_registry_path,
                         installed / "protocol_registry.json"));
        expect(fs::is_regular_file(data_dir /
                                   "reports/biome_parameters/overworld.json"));
        expect(fs::is_regular_file(data_dir /
                                   "reports/biome_parameters/nether.json"));
    };

    "game-data/generation fills the missing tracked data"_test = [] {
        if (on_windows()) {
            return;
        }
        refrain::test_support::scratch_directory scratch{};
        const auto data_dir{scratch.path() / "data"};
        stage_datapack(data_dir);
        const auto script{scratch.path() / "generate.sh"};
        write_generation_script(script);
        const auto jar{scratch.path() / "server.jar"};
        const auto tools{scratch.path() / "refrain-tools"};
        write_text(jar, "jar");
        write_text(tools, "tools");
        auto options = data_options(scratch.path(), data_dir);
        options.download_allowed = false;
        options.require_tracked_artifacts = true;
        options.generation = generation_options(script, jar, tools).generation;
        const auto paths = ensure_game_data(options, nullptr);
        expect(paths.has_value()) << (paths ? "" : paths.error().message);
        if (!paths) {
            return;
        }
        expect(same_path(paths->tables_path, data_dir / "tables.rvtb"));
        expect(same_path(paths->protocol_registry_path,
                         data_dir / "protocol_registry.json"));
        expect(fs::is_regular_file(data_dir /
                                   "reports/biome_parameters/overworld.json"));
        expect(fs::is_regular_file(data_dir /
                                   "reports/biome_parameters/nether.json"));
        const auto arguments{read_text(data_dir / "generation-args.txt")};
        expect(arguments.find("--server-jar") != std::string::npos)
            << arguments;
        expect(arguments.find("--out") != std::string::npos) << arguments;
    };

    "game-data/a failed generation is reported with the missing artifact"_test =
        [] {
            if (on_windows()) {
                return;
            }
            refrain::test_support::scratch_directory scratch{};
            const auto data_dir{scratch.path() / "data"};
            stage_datapack(data_dir);
            const auto script{scratch.path() / "generate.sh"};
            write_text(script, "#!/bin/sh\nexit 4\n");
            std::error_code permissions_error;
            fs::permissions(script,
                            fs::perms::owner_read | fs::perms::owner_write |
                                fs::perms::owner_exec,
                            fs::perm_options::add, permissions_error);
            expect(!permissions_error);
            const auto jar{scratch.path() / "server.jar"};
            const auto tools{scratch.path() / "refrain-tools"};
            write_text(jar, "jar");
            write_text(tools, "tools");
            auto options = data_options(scratch.path(), data_dir);
            options.download_allowed = false;
            options.require_tracked_artifacts = true;
            options.generation =
                generation_options(script, jar, tools).generation;
            const auto paths = ensure_game_data(options, nullptr);
            expect(!paths.has_value());
            if (!paths) {
                const auto &message{paths.error().message};
                expect(message.find("tables.rvtb") != std::string::npos)
                    << message;
                expect(message.find("exit code 4") != std::string::npos)
                    << message;
            }
        };

    "game-data/a generation that leaves a partial datapack fails closed"_test =
        [] {
            if (on_windows()) {
                return;
            }
            refrain::test_support::scratch_directory scratch{};
            const auto data_dir{scratch.path() / "data"};
            stage_datapack(data_dir);
            const auto script{scratch.path() / "generate.sh"};
            write_text(script, R"SH(#!/bin/sh
out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        *) shift ;;
    esac
done
mkdir -p "$out/reports/biome_parameters"
printf 'tables' > "$out/tables.rvtb"
printf '{}' > "$out/protocol_registry.json"
printf '{}\n' > "$out/reports/biome_parameters/overworld.json"
printf '{}\n' > "$out/reports/biome_parameters/nether.json"
printf '{"game_version":"26.3","data_version":5023,"datapack":"data","entry_count":999}\n' \
    > "$out/MANIFEST.json"
)SH");
            std::error_code permissions_error;
            fs::permissions(script,
                            fs::perms::owner_read | fs::perms::owner_write |
                                fs::perms::owner_exec,
                            fs::perm_options::add, permissions_error);
            expect(!permissions_error);
            const auto jar{scratch.path() / "server.jar"};
            const auto tools{scratch.path() / "refrain-tools"};
            write_text(jar, "jar");
            write_text(tools, "tools");
            auto options = data_options(scratch.path(), data_dir);
            options.download_allowed = false;
            options.require_tracked_artifacts = true;
            options.generation =
                generation_options(script, jar, tools).generation;
            const auto paths = ensure_game_data(options, nullptr);
            expect(!paths.has_value());
            if (!paths) {
                expect(paths.error().message.find("incomplete datapack") !=
                       std::string::npos)
                    << paths.error().message;
            }
        };

    "game-data/a generation that exceeds its timeout fails closed"_test = [] {
        if (on_windows()) {
            return;
        }
        refrain::test_support::scratch_directory scratch{};
        const auto data_dir{scratch.path() / "data"};
        stage_datapack(data_dir);
        const auto script{scratch.path() / "generate.sh"};
        write_text(script, R"SH(#!/bin/sh
work=""
while [ $# -gt 0 ]; do
    case "$1" in
        --work) work="$2"; shift 2 ;;
        *) shift ;;
    esac
done
if [ -n "$work" ]; then
    mkdir -p "$work"
    printf 'started' > "$work/marker.txt"
fi
sleep 30
)SH");
        std::error_code permissions_error;
        fs::permissions(script,
                        fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::owner_exec,
                        fs::perm_options::add, permissions_error);
        expect(!permissions_error);
        const auto jar{scratch.path() / "server.jar"};
        const auto tools{scratch.path() / "refrain-tools"};
        write_text(jar, "jar");
        write_text(tools, "tools");
        auto options = data_options(scratch.path(), data_dir);
        options.download_allowed = false;
        options.require_tracked_artifacts = true;
        options.generation = generation_options(script, jar, tools).generation;
        options.generation.timeout = std::chrono::milliseconds{200};
        const auto started{std::chrono::steady_clock::now()};
        const auto paths = ensure_game_data(options, nullptr);
        const auto elapsed{std::chrono::steady_clock::now() - started};
        expect(!paths.has_value());
        if (!paths) {
            expect(paths.error().message.find("did not finish") !=
                   std::string::npos)
                << paths.error().message;
        }
        expect(!fs::exists(data_dir / ".data-generation.tmp"))
            << "the generation scratch directory was not removed";
        expect(elapsed < std::chrono::seconds{10});
    };

    "game-data/a verified download feeds the generation fallback"_test = [] {
        if (on_windows()) {
            return;
        }
        refrain::test_support::scratch_directory scratch{};
        const auto archive{scratch.path() / "server.jar"};
        expect(make_plain_jar(archive));
        const auto archive_bytes{read_bytes(archive)};
        fake_transport transport{
            manifest_document(),
            version_document(sha1_hex(archive_bytes), archive_bytes), archive};
        const auto data_dir{scratch.path() / "data"};
        const auto script{scratch.path() / "generate.sh"};
        write_generation_script(script);
        const auto tools{scratch.path() / "refrain-tools"};
        write_text(tools, "tools");
        auto options = data_options(scratch.path(), data_dir);
        options.require_tracked_artifacts = true;
        options.generation.script = script.string();
        options.generation.tools = tools.string();
        options.generation.timeout = std::chrono::seconds{30};
        const auto paths = ensure_game_data(options, &transport);
        expect(paths.has_value()) << (paths ? "" : paths.error().message);
        if (!paths) {
            return;
        }
        expect(transport.download_calls == 1);
        expect(same_path(paths->tables_path, data_dir / "tables.rvtb"));
        expect(same_path(paths->protocol_registry_path,
                         data_dir / "protocol_registry.json"));
        expect(fs::is_regular_file(data_dir /
                                   "reports/biome_parameters/overworld.json"));
        // The archive is removed once the generation fallback has read it.
        expect(!fs::exists(data_dir / ".server.jar.tmp"));
    };
};

} // namespace
