// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/server/install_layout.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <filesystem>
#include <string>

namespace {

using namespace boost::ut;
using refrain::server::discover_install_layout;
using refrain::server::install_layout_options;

namespace fs = std::filesystem;

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
        expect(layout->gamepack ==
               (working / "gamepack" / "gamepack.toml").string());
        expect(layout->root == working.string());
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
        expect(layout->gamepack ==
               (install / "gamepack" / "gamepack.toml").string());
        expect(layout->content_root ==
               (install / "content" / "vanilla" / "26.3").string());
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
            expect(layout->gamepack ==
                   (checkout / "gamepack" / "gamepack.toml").string());
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
            expect(layout->gamepack ==
                   (instance / "gamepack" / "gamepack.toml").string());
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
            expect(layout->gamepack ==
                   (instance / "gamepack" / "gamepack.toml").string());
            expect(layout->content_root ==
                   (installation / "content" / "vanilla" / "26.3").string());
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
            expect(layout->gamepack == explicit_path.string());
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
                expect(layout->content_root ==
                       (install / "content" / "vanilla" / "27.1").string());
            }
        };
};

} // namespace
