// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/server/install_layout.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace {

using namespace boost::ut;
using refrain::server::discover_install_layout;
using refrain::server::install_layout_options;

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

void write_file(const fs::path &path) {
    const auto written =
        refrain::core::io::write_text(path.string(), "eula = false\n");
    expect(written.has_value());
}

void make_gamepack(const fs::path &root) {
    fs::create_directories(root / "gamepack");
    write_file(root / "gamepack" / "gamepack.toml");
}

void make_content(const fs::path &root, std::string_view version = "26.3") {
    fs::create_directories(root / "content" / "vanilla" / std::string{version});
}

} // namespace

namespace {

using namespace boost::ut;

suite<"server/layout"> layout_tests = [] {
    "layout/default composition is found in the working directory"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto working = scratch.path() / "instance";
        make_gamepack(working);
        const auto layout = discover_install_layout(install_layout_options{
            .working_directory = working.string(),
            .executable =
                (scratch.path() / "elsewhere" / "bin" / "refrain-server")
                    .string()});
        expect(layout.has_value());
        if (!layout) {
            return;
        }
        expect(same_path(layout->gamepack,
                         working / "gamepack" / "gamepack.toml"));
        expect(same_path(layout->root, working));
        expect(layout->content_root.empty());
    };

    "layout/installed content is found next to the executable"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto install = scratch.path() / "refrain-0.1.0";
        make_gamepack(install);
        make_content(install);
        const auto layout = discover_install_layout(install_layout_options{
            .working_directory = (scratch.path() / "empty").string(),
            .executable = (install / "bin" / "refrain-server").string()});
        expect(layout.has_value());
        if (!layout) {
            return;
        }
        expect(same_path(layout->gamepack,
                         install / "gamepack" / "gamepack.toml"));
        expect(same_path(layout->content_root,
                         install / "content" / "vanilla" / "26.3"));
        expect(layout->content_source == "above the executable");
    };

    "layout/build tree layout is found three directories above the binary"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto checkout = scratch.path() / "checkout";
            make_gamepack(checkout);
            make_content(checkout);
            const auto layout = discover_install_layout(install_layout_options{
                .working_directory = (scratch.path() / "empty").string(),
                .executable =
                    (checkout / "build" / "Release" / "bin" / "refrain-server")
                        .string()});
            expect(layout.has_value());
            if (!layout) {
                return;
            }
            expect(same_path(layout->gamepack,
                             checkout / "gamepack" / "gamepack.toml"));
            expect(!layout->content_root.empty());
        };

    "layout/the working directory wins over the installation"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto installation = scratch.path() / "install";
        make_gamepack(installation);
        const auto instance = scratch.path() / "instance";
        make_gamepack(instance);
        const auto layout = discover_install_layout(install_layout_options{
            .working_directory = instance.string(),
            .executable = (installation / "bin" / "refrain-server").string()});
        expect(layout.has_value());
        if (layout) {
            expect(same_path(layout->gamepack,
                             instance / "gamepack" / "gamepack.toml"));
        }
    };

    "layout/composition and content may come from different roots"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto instance = scratch.path() / "instance";
        make_gamepack(instance);
        const auto installation = scratch.path() / "install";
        make_content(installation);
        const auto layout = discover_install_layout(install_layout_options{
            .working_directory = instance.string(),
            .executable = (installation / "bin" / "refrain-server").string()});
        expect(layout.has_value());
        if (layout) {
            expect(same_path(layout->gamepack,
                             instance / "gamepack" / "gamepack.toml"));
            expect(same_path(layout->content_root,
                             installation / "content" / "vanilla" / "26.3"));
        }
    };

    "layout/explicit gamepack skips the search"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto explicit_path = scratch.path() / "custom.toml";
        write_file(explicit_path);
        const auto layout = discover_install_layout(install_layout_options{
            .working_directory = (scratch.path() / "missing").string(),
            .executable =
                (scratch.path() / "missing" / "bin" / "server").string(),
            .gamepack = explicit_path.string()});
        expect(layout.has_value());
        if (layout) {
            expect(same_path(layout->gamepack, explicit_path));
            expect(layout->root.empty());
        }
    };

    "layout/missing composition fails closed naming --gamepack"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto layout = discover_install_layout(install_layout_options{
            .working_directory = (scratch.path() / "empty").string(),
            .executable = (scratch.path() / "empty" / "bin" / "refrain-server")
                              .string()});
        expect(!layout.has_value());
        if (!layout) {
            expect(layout.error().code == refrain::error_code::not_found);
            expect(layout.error().message.find("--gamepack") !=
                   std::string::npos);
            expect(layout.error().message.find("gamepack.toml") !=
                   std::string::npos);
        }
    };

    "layout/content version directory selects the requested game version"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto install = scratch.path() / "install";
            make_gamepack(install);
            make_content(install, "27.1");
            const auto layout = discover_install_layout(install_layout_options{
                .working_directory = (scratch.path() / "empty").string(),
                .executable = (install / "bin" / "refrain-server").string(),
                .game_version = "27.1"});
            expect(layout.has_value());
            if (layout) {
                expect(same_path(layout->content_root,
                                 install / "content" / "vanilla" / "27.1"));
            }
        };
    "layout/the generation entry point and its tools binary are found"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto install = scratch.path() / "install";
            make_gamepack(install);
            make_content(install);
            fs::create_directories(install / "bin");
            fs::create_directories(install / "tools" / "content");
            write_file(install / "bin" / "refrain-tools");
            write_file(install / "tools" / "content" / "generate.sh");
            const auto layout = discover_install_layout(install_layout_options{
                .working_directory = (scratch.path() / "instance").string(),
                .executable = (install / "bin" / "refrain-server").string()});
            expect(layout.has_value());
            if (!layout) {
                return;
            }
            expect(same_path(layout->bin_directory, install / "bin"));
            expect(same_path(layout->tools_program,
                             install / "bin" / "refrain-tools"));
            expect(same_path(layout->generation_script,
                             install / "tools" / "content" / "generate.sh"));
        };

    "layout/a Windows binary does not discover the POSIX generation entry"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto install = scratch.path() / "install";
            make_gamepack(install);
            make_content(install);
            fs::create_directories(install / "bin");
            fs::create_directories(install / "tools" / "content");
            write_file(install / "bin" / "refrain-tools.exe");
            write_file(install / "tools" / "content" / "generate.sh");
            const auto layout = discover_install_layout(install_layout_options{
                .working_directory = (scratch.path() / "instance").string(),
                .executable =
                    (install / "bin" / "refrain-server.exe").string()});
            expect(layout.has_value());
            if (!layout) {
                return;
            }
            expect(same_path(layout->tools_program,
                             install / "bin" / "refrain-tools.exe"));
            expect(layout->generation_script.empty());
        };

    "layout/a packaged install carries the tools but no generation entry"_test =
        [] {
            refrain::test_support::scratch_directory scratch{};
            const auto install = scratch.path() / "install";
            make_gamepack(install);
            make_content(install);
            fs::create_directories(install / "bin");
            write_file(install / "bin" / "refrain-tools");
            const auto layout = discover_install_layout(install_layout_options{
                .working_directory = (scratch.path() / "instance").string(),
                .executable = (install / "bin" / "refrain-server").string()});
            expect(layout.has_value());
            if (!layout) {
                return;
            }
            expect(!layout->tools_program.empty());
            expect(layout->generation_script.empty());
        };
};

} // namespace
