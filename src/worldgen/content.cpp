// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/content.hpp>

#include <refrain/archive/zip.hpp>
#include <refrain/core/io.hpp>

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
    const auto digest =
        static_cast<std::uint64_t>(std::filesystem::file_size(path));
    const auto root =
        std::filesystem::temp_directory_path() /
        ("refrain-worldgen-" + name + "-" + std::to_string(digest));
    const auto marker =
        root.parent_path() / (root.filename().string() + ".ready");
    if (std::filesystem::is_regular_file(marker)) {
        return root.string();
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
    const std::string marker_text{"ok"};
    const auto marker_bytes =
        std::as_bytes(std::span{marker_text.data(), marker_text.size()});
    (void)core::io::write_file(marker.string(), marker_bytes);
    return root.string();
}

} // namespace refrain::worldgen
