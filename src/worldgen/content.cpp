// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/content.hpp>

#include <refrain/archive/zip.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>

#include <filesystem>
#include <mutex>
#include <span>
#include <string>

namespace refrain::worldgen {

namespace {

auto is_zip(std::string_view path) -> bool {
    return path.size() > 4 && path.substr(path.size() - 4) == ".zip";
}

} // namespace

auto materialize_worldgen_data(std::string_view path) -> result<std::string> {
    if (!is_zip(path)) {
        return std::string{path};
    }
    static std::mutex mutex;
    const std::lock_guard lock{mutex};

    auto archive = archive::zip_reader::open(path);
    if (!archive) {
        return std::unexpected{archive.error()};
    }
    const auto name = std::filesystem::path{std::string{path}}.stem().string();
    // The cache identity is the archive content, not its name and size: two
    // different packs with the same basename and byte count must not share a
    // directory, and a pre-created marker must not redirect the loader.
    auto archive_bytes{
        core::io::read_file(path, {.max_bytes = 256ULL * 1024ULL * 1024ULL,
                                   .allow_symlink = false})};
    if (!archive_bytes) {
        return std::unexpected{archive_bytes.error()};
    }
    const auto digest{data::to_hex(data::blake3(*archive_bytes))};
    const auto root = std::filesystem::temp_directory_path() /
                      ("refrain-worldgen-" + name + "-" + digest.substr(0, 32));
    const auto marker =
        root.parent_path() / (root.filename().string() + ".ready");
    if (std::filesystem::is_regular_file(marker)) {
        auto existing{core::io::read_text(
            marker.string(), {.max_bytes = 128ULL, .allow_symlink = false})};
        if (existing && *existing == digest) {
            return root.string();
        }
        // A marker for different content cannot vouch for this tree.
        std::error_code removed;
        std::filesystem::remove(marker, removed);
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (const auto created = core::io::create_directories(root.string());
        !created) {
        return std::unexpected{created.error()};
    }
    for (const auto &entry : archive->entries()) {
        if (entry.directory) {
            continue;
        }
        if (const auto extracted = archive->extract(entry, root.string());
            !extracted) {
            return std::unexpected{extracted.error()};
        }
    }
    if (const auto written{core::io::write_text(marker.string(), digest)};
        !written) {
        return std::unexpected{written.error()};
    }
    return root.string();
}

} // namespace refrain::worldgen
