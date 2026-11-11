// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Location of the locally generated worldgen datapack snapshot. The snapshot
// is produced from the user's own server jar and is not committed, so a test
// that reads it skips with one diagnostic when the checkout has no copy.

#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace refrain::test_support {

/// Datapack `data` root for the shipped game version.
[[nodiscard]] inline auto vanilla_data_root() -> std::string {
    return std::string{REFRAIN_SOURCE_DIR} + "/content/vanilla/26.3/data";
}

struct vanilla_data_snapshot {
    std::string path{};
    bool available{false};
};

/// Returns the datapack root and whether it is installed. When it is absent,
/// prints one line with the command that generates it, so a content-backed
/// test can return on `available == false` without passing silently.
[[nodiscard]] inline auto vanilla_data() -> vanilla_data_snapshot {
    auto path = vanilla_data_root();
    std::error_code error;
    const bool available =
        std::filesystem::is_directory(path + "/minecraft/worldgen", error) &&
        std::filesystem::is_directory(path + "/minecraft/tags", error);
    if (!available) {
        static const bool announced = [] {
            std::fprintf(stderr, "worldgen: datapack snapshot missing; run "
                                 "scripts/fetch-content.sh <server.jar>\n");
            return true;
        }();
        static_cast<void>(announced);
    }
    return vanilla_data_snapshot{.path = std::move(path),
                                 .available = available};
}

} // namespace refrain::test_support
