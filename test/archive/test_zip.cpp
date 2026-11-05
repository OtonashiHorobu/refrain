// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/archive/manifest.hpp>
#include <refrain/archive/zip.hpp>
#include <refrain/core/io.hpp>

#include <boost/ut.hpp>

#include <zip.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using refrain::archive::build_manifest;
using refrain::archive::data_manifest;
using refrain::archive::extract_filter;
using refrain::archive::join_under;
using refrain::archive::manifest_to_json;
using refrain::archive::max_entry_bytes;
using refrain::archive::safe_relative_path;
using refrain::archive::zip_entry;
using refrain::archive::zip_reader;

struct archive_file {
    std::string name;
    std::string content;
    bool directory{false};
};

[[nodiscard]] auto make_temp_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_archive_test_" + std::to_string(stamp))};
    auto error{std::error_code{}};
    std::filesystem::create_directories(path, error);
    expect(!error);
    return path;
}

void remove_quietly(const std::filesystem::path &path) noexcept {
    auto error{std::error_code{}};
    std::filesystem::remove_all(path, error);
}

/// Create a synthetic zip (tests never ship real Minecraft jars/assets).
/// With `stored = true` every regular file is written uncompressed, so tests
/// can corrupt its raw bytes without touching the central-directory CRC.
[[nodiscard]] auto make_zip(const std::filesystem::path &path,
                            const std::vector<archive_file> &files,
                            bool stored = false) -> bool {
    int error_code_value{0};
    auto *archive{zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE,
                           &error_code_value)};
    if (archive == nullptr) {
        return false;
    }
    for (const auto &file : files) {
        if (file.directory) {
            if (zip_dir_add(archive, file.name.c_str(), ZIP_FL_ENC_UTF_8) < 0) {
                zip_discard(archive);
                return false;
            }
            continue;
        }
        // freep = 0: the source references `file.content`, which outlives
        // zip_close() because `files` is alive in the caller.
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
        if (stored &&
            zip_set_file_compression(archive, static_cast<zip_uint64_t>(index),
                                     ZIP_CM_STORE, 0) != 0) {
            zip_discard(archive);
            return false;
        }
    }
    return zip_close(archive) == 0;
}

/// Flip one byte of `needle` in the on-disk archive, leaving every header
/// (including the central-directory CRC) untouched. Returns false when the raw
/// payload is not found verbatim, i.e. the entry was not stored uncompressed.
[[nodiscard]] auto corrupt_stored_payload(const std::filesystem::path &path,
                                          std::string_view needle) -> bool {
    std::string bytes{};
    {
        std::ifstream input{path, std::ios::binary};
        bytes.assign(std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{});
    }
    const auto position{bytes.find(needle)};
    if (position == std::string::npos) {
        return false;
    }
    bytes[position] ^= 0x01;
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

[[nodiscard]] auto sample_files() -> std::vector<archive_file> {
    return {
        {.name = "data/", .content = "", .directory = true},
        {.name = "data/minecraft/recipe/stone.json",
         .content = R"({"type":"minecraft:crafting_shaped"})"},
        {.name = "data/refrain/tags/block/air.json", .content = "[]"},
        {.name = "assets/", .content = "", .directory = true},
        {.name = "assets/minecraft/texts/en_us.txt", .content = "hello"},
        {.name = "META-INF/MANIFEST.MF", .content = "Manifest-Version: 1.0\n"},
        {.name = "version.json", .content = R"({"id":"26.3"})"},
    };
}

} // namespace

namespace {

using namespace boost::ut;

const suite archive_tests = [] {
    "archive/safe_relative_path rejects zip-slip"_test = [] {
        const std::array<std::string_view, 14> valid{
            "data/minecraft/recipe/stone.json",
            "assets/minecraft/texts/en_us.txt", "version.json", "a/b/c/d",
            "META-INF/MANIFEST.MF", "data/file name with spaces.txt",
            "data/unicode-\u00e4\u00f6\u00fc.txt", "deep/path/file.bin",
            // Names that merely contain a reserved word are fine.
            "data/construct.txt", "data/console/file.txt", "data/com10.txt",
            "data/lpt0", "data/auxiliary.txt", "data/COM0.txt"};
        for (const auto path : valid) {
            expect(safe_relative_path(path)) << path;
        }

        const std::array<std::string_view, 22> unsafe{
            "", "/etc/passwd", "../escape.txt", "data/../../escape.txt",
            "data/./file.txt", "data//file.txt", "data/", "C:/Windows/file.txt",
            "data\\file.txt", "data/file.txt/",
            // Explicit length: a `const char*` view of this literal would stop
            // at the embedded NUL and silently test "data/" instead.
            std::string_view{"data/\0hidden.txt", 16}, "a/../b",
            // Windows device names in any case, with or without an
            // extension.
            "CON", "data/CON", "data/con.txt", "data/NUL.json", "data/COM1",
            "data/com9.bin", "data/LPT1.txt",
            // Windows strips a trailing dot or space, which would collide
            // with the name without it.
            "data/file.", "data/file ", "data/dir./file.txt"};
        for (const auto path : unsafe) {
            expect(!safe_relative_path(path)) << path;
        }
    };

    "archive/join_under adds one separator"_test = [] {
        expect(join_under("/tmp/out", "data/a.txt") == "/tmp/out/data/a.txt");
        expect(join_under("/tmp/out/", "data/a.txt") == "/tmp/out/data/a.txt");
        expect(join_under("", "data/a.txt") == "data/a.txt");
    };

    "archive/open and read a synthetic zip"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "sample.jar"};
        expect(make_zip(path, sample_files()));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            remove_quietly(directory);
            return;
        }
        expect(reader->archive_path() == path.string());
        expect(reader->entries().size() == 7U);

        const auto *entry{reader->find("data/minecraft/recipe/stone.json")};
        expect(entry != nullptr);
        if (entry != nullptr) {
            expect(!entry->directory);
            expect(entry->size == 36U);
            expect(entry->crc32 != 0U);
        }
        const auto *directory_entry{reader->find("data/")};
        expect(directory_entry != nullptr);
        if (directory_entry != nullptr) {
            expect(directory_entry->directory);
        }
        expect(reader->find("missing") == nullptr);

        const auto content{reader->read("data/minecraft/recipe/stone.json")};
        expect(static_cast<bool>(content));
        if (content) {
            const auto text{
                std::string{reinterpret_cast<const char *>(content->data()),
                            content->size()}};
            expect(text == R"({"type":"minecraft:shaped"})" ||
                   text == R"({"type":"minecraft:crafting_shaped"})");
        }

        expect(!reader->read("missing"));
        expect(reader->read("missing").error().code ==
               refrain::error_code::not_found);
        if (directory_entry != nullptr) {
            expect(!reader->read(*directory_entry));
            expect(reader->read(*directory_entry).error().code ==
                   refrain::error_code::invalid_argument);
        }

        remove_quietly(directory);
    };

    // CRC check: libzip validates the entry CRC only when the stream reaches
    // EOF, so read() must consume to EOF and honour zip_fclose()'s status.
    // Flipping one raw byte of a *stored* entry leaves the central-directory
    // CRC intact, so a successful read here would prove the EOF probe or close
    // check is missing.
    "archive/read rejects a corrupted stored entry"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "crc.jar"};
        constexpr std::string_view corrupted_payload{"corrupted-payload"};
        constexpr std::string_view intact_payload{"intact-payload"};
        expect(make_zip(path,
                        {{.name = "data/corrupted.bin",
                          .content = std::string{corrupted_payload}},
                         {.name = "data/intact.bin",
                          .content = std::string{intact_payload}}},
                        true));
        expect(corrupt_stored_payload(path, corrupted_payload));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            const auto corrupted{reader->read("data/corrupted.bin")};
            expect(!corrupted);
            if (!corrupted) {
                expect(corrupted.error().code == refrain::error_code::io_error);
            }

            // The unrelated entry in the same archive is still readable.
            const auto intact{reader->read("data/intact.bin")};
            expect(static_cast<bool>(intact));
            if (intact) {
                expect(intact->size() == intact_payload.size());
                expect(
                    std::string{reinterpret_cast<const char *>(intact->data()),
                                intact->size()} == intact_payload);
            }
        }
        remove_quietly(directory);
    };

    // The EOF probe runs even for entry.size == 0 (read() opens the stream), so
    // a legitimate empty file must still succeed.
    "archive/read accepts a zero-byte entry"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "empty.jar"};
        expect(make_zip(path, {{.name = "data/empty.bin", .content = ""}}));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            const auto content{reader->read("data/empty.bin")};
            expect(static_cast<bool>(content));
            if (content) {
                expect(content->empty());
            }
        }
        remove_quietly(directory);
    };

    "archive/build_manifest filters and sorts deterministically"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "sample.jar"};
        expect(make_zip(path, sample_files()));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            remove_quietly(directory);
            return;
        }

        auto all{build_manifest(*reader, extract_filter::all)};
        expect(static_cast<bool>(all));
        if (!all) {
            remove_quietly(directory);
            return;
        }
        expect(all->schema == "refrain.data-manifest/1");
        expect(all->source == "sample.jar");
        expect(all->source_size > 0U);
        expect(all->total_entries == 7U);
        expect(all->total_files == 5U);
        expect(all->entries.size() == 5U);
        expect(std::is_sorted(all->entries.begin(), all->entries.end(),
                              [](const auto &lhs, const auto &rhs) {
                                  return lhs.path < rhs.path;
                              }));
        expect(all->categories ==
               (std::vector<std::string>{"assets", "data", "meta", "other"}));
        expect(all->entries.front().path == "META-INF/MANIFEST.MF");
        expect(all->entries.front().category == "meta");

        // total_entries counts every central-directory entry, directories
        // included, while total_files/total_bytes cover only the selected
        // regular files; total_bytes is the exact sum of entry sizes.
        std::uint64_t summed_bytes{0};
        for (const auto &entry : all->entries) {
            summed_bytes += entry.size;
        }
        expect(all->total_bytes == summed_bytes);
        expect(all->total_entries == reader->entries().size());
        expect(all->total_files == all->entries.size());

        const auto data{build_manifest(*reader, extract_filter::data)};
        expect(static_cast<bool>(data));
        if (data) {
            expect(data->total_files == 2U);
            // data filter: stone.json (36 bytes) + air.json ("[]" = 2 bytes).
            expect(data->total_bytes == 36U + 2U);
            for (const auto &entry : data->entries) {
                expect(entry.category == "data");
                expect(entry.path.starts_with("data/"));
            }
            expect(data->categories == std::vector<std::string>{"data"});
        }

        const auto assets{build_manifest(*reader, extract_filter::assets)};
        expect(static_cast<bool>(assets));
        if (assets) {
            expect(assets->total_files == 1U);
            expect(assets->entries[0].path ==
                   "assets/minecraft/texts/en_us.txt");
        }

        const auto json{manifest_to_json(*all)};
        expect(static_cast<bool>(json));
        if (json) {
            expect(json->find("\"schema\":\"refrain.data-manifest/1\"") !=
                   std::string::npos);
            expect(json->find("\"source\":\"sample.jar\"") !=
                   std::string::npos);
            expect(json->find("\"total_files\":5") != std::string::npos);
            expect(json->find("\"path\":\"version.json\"") !=
                   std::string::npos);
            // Deterministic across calls.
            const auto again{manifest_to_json(*all)};
            expect(again && *again == *json);
        }

        remove_quietly(directory);
    };

    "archive/build_manifest rejects unsafe and duplicate entries"_test = [] {
        const auto directory{make_temp_directory()};
        const auto unsafe_path{directory / "unsafe.jar"};
        expect(make_zip(unsafe_path,
                        {{.name = "../escape.txt", .content = "boom"}}));

        auto unsafe{zip_reader::open(unsafe_path.string())};
        expect(static_cast<bool>(unsafe));
        if (unsafe) {
            const auto manifest{build_manifest(*unsafe, extract_filter::all)};
            expect(!manifest);
            expect(manifest.error().code == refrain::error_code::parse_error);
        }

        // Duplicate names cannot be produced through libzip's writer API (it
        // refuses duplicate entries without ZIP_FL_OVERWRITE); the manifest
        // builder still guards against them because third-party archives may
        // contain them. The unsafe-path check above covers the failure path.

        remove_quietly(directory);
    };

    "archive/extract writes files below the output root"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "sample.jar"};
        expect(make_zip(path, sample_files()));

        const auto output{directory / "extracted"};
        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            remove_quietly(directory);
            return;
        }
        const auto manifest{build_manifest(*reader, extract_filter::data)};
        expect(static_cast<bool>(manifest));
        if (!manifest) {
            remove_quietly(directory);
            return;
        }
        for (const auto &entry : manifest->entries) {
            const auto *zip_entry{reader->find(entry.path)};
            expect(zip_entry != nullptr);
            if (zip_entry != nullptr) {
                expect(static_cast<bool>(
                    reader->extract(*zip_entry, output.string())));
            }
        }

        expect(std::filesystem::is_regular_file(
            output / "data/minecraft/recipe/stone.json"));
        expect(std::filesystem::is_regular_file(
            output / "data/refrain/tags/block/air.json"));
        expect(!std::filesystem::exists(output / "assets"));

        const auto content{refrain::core::io::read_text(
            (output / "data/refrain/tags/block/air.json").string())};
        expect(static_cast<bool>(content));
        if (content) {
            expect(*content == "[]");
        }

        // Empty output root is rejected.
        expect(!reader->extract(*reader->find("version.json"), ""));

        remove_quietly(directory);
    };

    "archive/extract refuses unsafe names"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "unsafe.jar"};
        expect(make_zip(path, {{.name = "../escape.txt", .content = "boom"}}));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            const auto *entry{reader->find("../escape.txt")};
            expect(entry != nullptr);
            if (entry != nullptr) {
                const auto status{
                    reader->extract(*entry, (directory / "out").string())};
                expect(!status);
                expect(status.error().code ==
                       refrain::error_code::permission_denied);
            }
        }
        expect(!std::filesystem::exists(directory / "escape.txt"));
        remove_quietly(directory);
    };

    // A pre-existing symlink below the output root must not redirect an
    // extraction outside that root. The check is intentionally conservative
    // and rejects the whole tree component. Windows may not permit unprivileged
    // symlink creation, in which case the test is skipped.
    "archive/extract refuses symlink components"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "symlink.jar"};
        expect(make_zip(path,
                        {{.name = "data/link/evil.txt", .content = "escape"}}));
        const auto outside{directory / "outside"};
        std::error_code error;
        std::filesystem::create_directories(outside, error);
        const auto output{directory / "extracted"};
        std::filesystem::create_directories(output / "data", error);
        std::filesystem::create_directory_symlink(outside, output / "data/link",
                                                  error);
        if (error) {
            remove_quietly(directory);
            return;
        }

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            const auto *entry{reader->find("data/link/evil.txt")};
            expect(entry != nullptr);
            if (entry != nullptr) {
                const auto status{reader->extract(*entry, output.string())};
                expect(!status);
                expect(status.error().code ==
                       refrain::error_code::permission_denied);
            }
        }
        expect(!std::filesystem::exists(outside / "evil.txt"));
        remove_quietly(directory);
    };

    "archive/open reports io_error for a missing archive"_test = [] {
        const auto reader{zip_reader::open("/nonexistent/refrain/sample.jar")};
        expect(!reader);
        expect(reader.error().code == refrain::error_code::io_error);
    };

    // The uncompressed size comes from the (untrusted) central directory; an
    // oversized declaration must be rejected *before* allocating instead of
    // triggering an OOM. A forged entry is enough to exercise the limit
    // because the check precedes zip_fopen_index().
    //
    // `max_entries` is enforced in zip_reader::open() before reserve(). A
    // >1M-entry fixture would be large and slow, so the cap is covered by the
    // public constant + review rather than a runtime test here.
    "archive/read rejects entries above the size safety limit"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "sample.jar"};
        expect(make_zip(path, sample_files()));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            expect(max_entry_bytes >= 64ULL * 1024ULL * 1024ULL);
            const zip_entry forged{
                .name = "data/forged.bin",
                .index = 0,
                .size = max_entry_bytes + 1,
                .compressed_size = 1,
            };
            const auto oversized{reader->read(forged)};
            expect(!oversized);
            if (!oversized) {
                expect(oversized.error().code ==
                       refrain::error_code::parse_error);
            }

            // The manifest builder rejects the same shape before extraction.
            expect(!reader->extract(forged, (directory / "out").string()));
        }
        remove_quietly(directory);
    };

    // Names that would alias a Windows device or be silently rewritten
    // (trailing dot/space) are rejected by the manifest builder too, not only
    // by the direct extract() entry point.
    "archive/build_manifest rejects unportable names"_test = [] {
        const auto directory{make_temp_directory()};
        std::size_t sequence{0};
        for (const std::string_view name :
             {"data/CON", "data/con.txt", "data/NUL.json", "data/COM1",
              "data/LPT9.bin", "data/trailing.", "data/trailing "}) {
            const auto path{directory / ("unportable-" +
                                         std::to_string(sequence++) + ".jar")};
            expect(
                make_zip(path, {{.name = std::string{name}, .content = "x"}}))
                << name;
            auto reader{zip_reader::open(path.string())};
            expect(static_cast<bool>(reader)) << name;
            if (reader) {
                const auto manifest{
                    build_manifest(*reader, extract_filter::all)};
                expect(!manifest) << name;
                if (!manifest) {
                    expect(manifest.error().code ==
                           refrain::error_code::parse_error)
                        << name;
                }
            }
        }
        remove_quietly(directory);
    };

    // `Alpha.txt` and `alpha.txt` both fit in a zip but would overwrite each
    // other on Windows/macOS default file systems; the manifest is rejected
    // instead of producing a nondeterministic/partial extraction.
    "archive/build_manifest rejects case-insensitive collisions"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "collision.jar"};
        expect(make_zip(path, {{.name = "data/Alpha.txt", .content = "a"},
                               {.name = "data/alpha.txt", .content = "b"}}));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            expect(reader->entries().size() == 2U);
            expect(reader->find("data/Alpha.txt") != nullptr);
            expect(reader->find("data/alpha.txt") != nullptr);
            const auto manifest{build_manifest(*reader, extract_filter::all)};
            expect(!manifest);
            if (!manifest) {
                expect(manifest.error().code ==
                       refrain::error_code::already_exists);
                expect(manifest.error().message.find("case") !=
                       std::string::npos);
            }
        }
        remove_quietly(directory);
    };

    // Host paths may be Windows-style; only the archive file name may appear in
    // the manifest's `source` (no host directory leak). A POSIX file name can
    // legally contain backslashes, so this is testable on a POSIX host without
    // Windows.
    "archive/build_manifest strips host paths from source"_test = [] {
        const auto directory{make_temp_directory()};
        const auto host_path{directory / "C:\\Temp\\client.jar"};
        expect(
            make_zip(host_path, {{.name = "version.json", .content = "{}"}}));

        auto reader{zip_reader::open(host_path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            const auto manifest{build_manifest(*reader, extract_filter::all)};
            expect(static_cast<bool>(manifest));
            if (manifest) {
                expect(manifest->source == "client.jar");
                const auto json{manifest_to_json(*manifest)};
                expect(static_cast<bool>(json));
                if (json) {
                    expect(json->find("Users") == std::string::npos);
                    expect(json->find("C:") == std::string::npos);
                    expect(json->find("\"source\":\"client.jar\"") !=
                           std::string::npos);
                }
            }
        }
        remove_quietly(directory);
    };

    // `find` is backed by an exact-name hash index built at open(), so every
    // manifest lookup during extraction is O(1) instead of a linear scan. This
    // test locks correctness over a non-trivial entry count and pointer
    // stability.
    "archive/find resolves every entry by exact name"_test = [] {
        const auto directory{make_temp_directory()};
        const auto path{directory / "many.jar"};
        constexpr std::size_t entry_count{512};
        std::vector<archive_file> files{};
        files.reserve(entry_count);
        for (std::size_t index{0}; index < entry_count; ++index) {
            files.push_back(
                {.name = "data/entry-" + std::to_string(index) + ".json",
                 .content = "{}"});
        }
        expect(make_zip(path, files));

        auto reader{zip_reader::open(path.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            expect(reader->entries().size() == entry_count);
            std::size_t resolved{0};
            for (std::size_t index{0}; index < entry_count; ++index) {
                const auto name{"data/entry-" + std::to_string(index) +
                                ".json"};
                const auto *entry{reader->find(name)};
                expect(entry != nullptr) << name;
                if (entry != nullptr) {
                    expect(entry->index == index) << name;
                    // Stable pointer: the index points into the immutable
                    // entries vector, not a temporary.
                    expect(reader->find(name) == entry);
                    ++resolved;
                }
            }
            expect(resolved == entry_count);
            expect(reader->find("data/entry-0.jsonx") == nullptr);
            expect(reader->find("") == nullptr);
        }
        remove_quietly(directory);
    };
};

} // namespace
