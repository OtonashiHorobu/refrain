// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <boost/ut.hpp>
#include <refrain/core/io.hpp>
#include <refrain/mod/package.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

using namespace boost::ut;

namespace {

namespace fs = std::filesystem;
namespace ut = boost::ut;

class package_temp {
  public:
    package_temp() {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        const auto base{fs::temp_directory_path(error)};
        if (error) {
            return;
        }
        for (int attempt{0}; attempt < 16; ++attempt) {
            const auto candidate{base / ("refrain-package-" +
                                         std::to_string(sequence.fetch_add(
                                             1, std::memory_order_relaxed)))};
            if (fs::create_directories(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                return;
            }
            error.clear();
        }
    }
    package_temp(const package_temp &) = delete;
    auto operator=(const package_temp &) -> package_temp & = delete;
    ~package_temp() {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    [[nodiscard]] auto path() const -> const fs::path & { return path_; }
    [[nodiscard]] auto sub(std::string_view name) const -> std::string {
        return (path_ / std::string{name}).string();
    }

  private:
    fs::path path_;
};

[[nodiscard]] auto as_text(const std::vector<std::byte> &bytes) -> std::string {
    return std::string{reinterpret_cast<const char *>(bytes.data()),
                       bytes.size()};
}

} // namespace

suite mod_package = [] {
    "directory package reads regular members"_test = [] {
        const package_temp temporary;
        const auto root{temporary.sub("mods/test")};
        ut::expect(static_cast<bool>(
            refrain::core::io::create_directories(root + "/assets")));
        ut::expect(static_cast<bool>(refrain::core::io::write_text(
            root + "/manifest.toml", "id = \"test\"\n")));
        ut::expect(static_cast<bool>(refrain::core::io::write_text(
            root + "/assets/hello.txt", "hello")));
        auto source{refrain::mod::make_directory_package_source(root)};
        ut::expect(static_cast<bool>(source));
        if (!source) {
            return;
        }
        auto manifest{(*source)->read("manifest.toml")};
        ut::expect(static_cast<bool>(manifest));
        if (manifest) {
            ut::expect(as_text(*manifest) == "id = \"test\"\n");
        }
        auto text{(*source)->read("assets/hello.txt")};
        ut::expect(static_cast<bool>(text));
        if (text) {
            ut::expect(as_text(*text) == "hello");
        }
        ut::expect(!(*source)->read("assets/missing.txt"));
        ut::expect(!(*source)->read("../outside.txt"));
        ut::expect(!(*source)->read("/etc/passwd"));
    };

    "directory package rejects symlinked components"_test = [] {
        const package_temp temporary;
        const auto root{temporary.sub("mods/test")};
        ut::expect(static_cast<bool>(
            refrain::core::io::create_directories(root + "/assets")));
        ut::expect(static_cast<bool>(refrain::core::io::write_text(
            root + "/manifest.toml", "id = \"test\"\n")));
        ut::expect(static_cast<bool>(refrain::core::io::write_text(
            root + "/assets/hello.txt", "hello")));
        const auto outside{temporary.sub("outside")};
        ut::expect(
            static_cast<bool>(refrain::core::io::create_directories(outside)));
        ut::expect(static_cast<bool>(
            refrain::core::io::write_text(outside + "/secret.txt", "secret")));

        std::error_code error;
        fs::create_directory_symlink(outside, root + "/linked", error);
        if (error) {
            // Filesystem without symlink support (Windows without developer
            // mode): the POSIX CI job covers this case.
            return;
        }
        auto source{refrain::mod::make_directory_package_source(root)};
        ut::expect(static_cast<bool>(source));
        if (!source) {
            return;
        }
        const auto through_component{(*source)->read("linked/secret.txt")};
        ut::expect(!through_component);
        if (!through_component) {
            ut::expect(through_component.error().code ==
                       refrain::error_code::permission_denied);
        }

        fs::create_symlink(root + "/assets/hello.txt", root + "/leaf.txt",
                           error);
        ut::expect(!error);
        if (!error) {
            const auto through_leaf{(*source)->read("leaf.txt")};
            ut::expect(!through_leaf);
            if (!through_leaf) {
                ut::expect(through_leaf.error().code ==
                           refrain::error_code::permission_denied);
            }
        }
    };
};
