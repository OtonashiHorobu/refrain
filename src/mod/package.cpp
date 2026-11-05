// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/package.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/path.hpp>

#include <fmt/format.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::mod {
namespace {

namespace data = refrain::data;
namespace io = core::io;

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

[[nodiscard]] auto annotate(error original, std::string_view context) -> error {
    if (original.message.find(context) != std::string::npos) {
        return original;
    }
    original.message = std::string{context} + ": " + original.message;
    return original;
}

class builtin_package_source final : public package_source {
  public:
    builtin_package_source(std::shared_ptr<const data::builtin_image> image,
                           std::string mod_id)
        : image_{std::move(image)}, mod_id_{std::move(mod_id)} {}

    [[nodiscard]] auto kind() const noexcept -> package_kind override {
        return package_kind::builtin;
    }

    [[nodiscard]] auto read(std::string_view relative_path) const
        -> result<std::vector<std::byte>> override {
        if (!data::is_safe_relative_path(relative_path)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("builtin package: unsafe path '{}'",
                                 relative_path))};
        }
        if (mod_id_.empty()) {
            return image_->read(relative_path);
        }
        return image_->read(fmt::format("mods/{}/{}", mod_id_, relative_path));
    }

    [[nodiscard]] auto image_bytes() const noexcept
        -> std::span<const std::byte> override {
        return image_bytes_;
    }

    void set_image_bytes(std::span<const std::byte> bytes) {
        image_bytes_ = bytes;
    }

  private:
    std::shared_ptr<const data::builtin_image> image_{};
    std::string mod_id_{};
    std::span<const std::byte> image_bytes_{};
};

class directory_package_source final : public package_source {
  public:
    explicit directory_package_source(std::string root)
        : root_{std::move(root)} {}

    [[nodiscard]] auto kind() const noexcept -> package_kind override {
        return package_kind::directory;
    }

    [[nodiscard]] auto read(std::string_view relative_path) const
        -> result<std::vector<std::byte>> override {
        if (!data::is_safe_relative_path(relative_path)) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("directory package '{}': unsafe member path '{}'",
                            root_, relative_path))};
        }
        // Reject a symlink at *any* component, not just the final one: a mod
        // directory shipping `assets -> ../../..` would otherwise escape the
        // package root through `read_file`'s final-component check.
        auto full{root_};
        std::size_t start{0};
        while (start <= relative_path.size()) {
            const auto slash{relative_path.find('/', start)};
            const auto component{relative_path.substr(
                start, slash == std::string_view::npos ? std::string_view::npos
                                                       : slash - start)};
            full = data::join_path(full, component);
            if (io::is_symlink(full)) {
                return std::unexpected{
                    fail(error_code::permission_denied,
                         fmt::format("directory package '{}': refusing to "
                                     "follow symlink component in '{}'",
                                     root_, relative_path))};
            }
            if (slash == std::string_view::npos) {
                break;
            }
            start = slash + 1;
        }
        if (!io::is_regular_file(full)) {
            return std::unexpected{fail(
                error_code::not_found,
                fmt::format("directory package '{}' has no regular file '{}'",
                            root_, relative_path))};
        }
        auto bytes{io::read_file(full, {.max_bytes = package_max_member_bytes,
                                        .allow_symlink = false})};
        if (!bytes) {
            return std::unexpected{
                annotate(std::move(bytes.error()),
                         fmt::format("directory package '{}'", root_))};
        }
        return bytes;
    }

  private:
    std::string root_{};
};

} // namespace

auto make_builtin_package_source(std::span<const std::byte> image,
                                 std::string_view mod_id)
    -> result<std::unique_ptr<package_source>> {
    auto opened{data::builtin_image::open(image)};
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    auto source{std::make_unique<builtin_package_source>(
        std::make_shared<const data::builtin_image>(std::move(*opened)),
        std::string{mod_id})};
    source->set_image_bytes(image);
    return std::unique_ptr<package_source>{std::move(source)};
}

auto make_directory_package_source(std::string root)
    -> result<std::unique_ptr<package_source>> {
    if (root.empty()) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "directory package: empty root")};
    }
    if (!io::is_directory(root)) {
        return std::unexpected{fail(
            error_code::not_found,
            fmt::format("directory package: '{}' is not a directory", root))};
    }
    return std::unique_ptr<package_source>{
        std::make_unique<directory_package_source>(std::move(root))};
}

auto make_package_resolver(std::span<const std::byte> builtin_image)
    -> result<package_resolver> {
    std::shared_ptr<const data::builtin_image> image{};
    if (!builtin_image.empty()) {
        auto opened{data::builtin_image::open(builtin_image)};
        if (!opened) {
            return std::unexpected{std::move(opened.error())};
        }
        image = std::make_shared<const data::builtin_image>(std::move(*opened));
    }
    const auto bytes{builtin_image};
    return package_resolver{
        [image = std::move(image), bytes](
            const candidate &mod) -> result<std::unique_ptr<package_source>> {
            switch (mod.kind) {
            case package_kind::builtin: {
                if (!image) {
                    return std::unexpected{
                        fail(error_code::not_found,
                             fmt::format("mod '{}' is a builtin package but no "
                                         "builtin image was supplied",
                                         mod.id))};
                }
                auto source{
                    std::make_unique<builtin_package_source>(image, mod.id)};
                source->set_image_bytes(bytes);
                return std::unique_ptr<package_source>{std::move(source)};
            }
            case package_kind::directory: {
                // `path` is `<root>/mods/<id>/manifest.toml`; the package root
                // is the directory that holds the manifest.
                const auto manifest_path{mod.path};
                const auto slash{manifest_path.rfind('/')};
                if (slash == std::string::npos || slash == 0) {
                    return std::unexpected{fail(
                        error_code::invalid_argument,
                        fmt::format("mod '{}' has an unusable manifest path "
                                    "'{}'",
                                    mod.id, manifest_path))};
                }
                return make_directory_package_source(
                    manifest_path.substr(0, slash));
            }
            case package_kind::rfmod:
                return std::unexpected{
                    fail(error_code::unsupported,
                         fmt::format("mod '{}': .rfmod package sources are not "
                                     "implemented; install the package as a "
                                     "directory tree",
                                     mod.id))};
            }
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("mod '{}' has an unknown package kind", mod.id))};
        }};
}

} // namespace refrain::mod
