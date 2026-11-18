// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/install_layout.hpp>

#include <refrain/core/io.hpp>
#include <refrain/platform/process.hpp>

#include <fmt/format.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::server {

namespace {

namespace io = refrain::core::io;

/// One place the layout may live, with the label the summary shows.
struct layout_root {
    std::string path{};
    std::string label{};
};

/// Candidate roots in priority order: the working directory first (an operator
/// running from an instance directory overrides the installation), then the
/// executable's directory and up to three parents (release archive layout and
/// build-tree layout).
[[nodiscard]] auto candidate_roots(std::string_view working,
                                   std::string_view executable)
    -> std::vector<layout_root> {
    std::vector<layout_root> roots{};
    const auto add = [&roots](std::string path, std::string label) {
        if (path.empty()) {
            return;
        }
        for (const auto &existing : roots) {
            if (existing.path == path) {
                return;
            }
        }
        roots.push_back(
            layout_root{.path = std::move(path), .label = std::move(label)});
    };

    add(std::string{working}, "working directory");

    if (executable.empty()) {
        return roots;
    }
    std::string directory{io::parent_directory(executable)};
    for (std::size_t depth{0}; depth < 4 && !directory.empty(); ++depth) {
        add(directory,
            depth == 0 ? "next to the executable" : "above the executable");
        const auto parent{io::parent_directory(directory)};
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return roots;
}

[[nodiscard]] auto regular_file(std::string_view path) -> bool {
    return io::is_regular_file(path);
}

[[nodiscard]] auto content_source(const std::string &root,
                                  const std::vector<layout_root> &roots)
    -> std::string {
    for (const auto &candidate : roots) {
        if (candidate.path == root) {
            return candidate.label;
        }
    }
    return "installed layout";
}

/// Windows binaries carry the `.exe` suffix; the tools binary next to the
/// server has the same one.
[[nodiscard]] auto executable_suffix(std::string_view executable)
    -> std::string_view {
    if (executable.size() >= 4 &&
        (executable.ends_with(".exe") || executable.ends_with(".EXE"))) {
        return ".exe";
    }
    return {};
}

} // namespace

auto discover_install_layout(const install_layout_options &options)
    -> result<install_layout> {
    std::string working{options.working_directory};
    if (working.empty()) {
        if (auto current{io::current_path()}; current) {
            working = std::move(*current);
        }
    }
    std::string executable{options.executable};
    if (executable.empty()) {
        if (auto image{refrain::platform::executable_path()}; image) {
            executable = std::move(*image);
        }
    }
    const auto roots{candidate_roots(working, executable)};

    install_layout layout{};
    if (!executable.empty()) {
        layout.bin_directory = io::parent_directory(executable);
    }
    if (!options.gamepack.empty()) {
        // An explicit composition still benefits from an installed content
        // snapshot: the tracked tables and reports are not part of the file.
        layout.gamepack = options.gamepack;
    }
    std::vector<std::string> searched{};
    for (const auto &candidate : roots) {
        if (layout.gamepack.empty()) {
            const auto gamepack{candidate.path + "/gamepack/gamepack.toml"};
            searched.push_back(gamepack);
            if (regular_file(gamepack)) {
                layout.gamepack = gamepack;
                layout.root = candidate.path;
            }
        }
        const auto content_root{candidate.path + "/content/vanilla/" +
                                options.game_version};
        if (layout.content_root.empty() && io::is_directory(content_root)) {
            layout.content_root = content_root;
            layout.content_source = content_source(candidate.path, roots);
        }
        if (layout.generation_script.empty()) {
            const auto script{candidate.path + "/tools/content/generate.sh"};
            if (regular_file(script)) {
                layout.generation_script = script;
            }
        }
    }
    if (!layout.bin_directory.empty()) {
        const auto tools{layout.bin_directory + "/refrain-tools" +
                         std::string{executable_suffix(executable)}};
        if (regular_file(tools)) {
            layout.tools_program = tools;
        }
    }
    if (layout.gamepack.empty()) {
        std::string paths{};
        for (const auto &path : searched) {
            if (!paths.empty()) {
                paths += ", ";
            }
            paths += "'" + path + "'";
        }
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("could not find the default composition; looked for "
                        "{} under the working directory and the executable, "
                        "pass --gamepack <file> to name it explicitly",
                        paths))};
    }
    return layout;
}

} // namespace refrain::server
