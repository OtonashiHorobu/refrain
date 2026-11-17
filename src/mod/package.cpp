// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/package.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>
#include <refrain/mod/rfmod.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
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

/// Installed `.rfmod` package source: members are read from the content store
/// through the installation receipt.
///
/// This is the runtime external-mod source. `receipt_path` is
/// `<store>/mods/<id>/<version>.json`; the store root is derived from it and
/// the blob table (`<store>/content/blobs.idx`) is opened on first read. Every
/// member read is checked against the receipt's `files[]` digest, so the bytes
/// a mod loads are exactly the bytes that were installed and audited.
class receipt_package_source final : public package_source {
  public:
    receipt_package_source(std::string receipt_path, mod_receipt receipt)
        : receipt_path_{std::move(receipt_path)}, receipt_{std::move(receipt)} {
    }

    [[nodiscard]] auto kind() const noexcept -> package_kind override {
        return package_kind::rfmod;
    }

    [[nodiscard]] auto read(std::string_view relative_path) const
        -> result<std::vector<std::byte>> override {
        if (!data::is_safe_relative_path(relative_path)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("installed package '{}': unsafe member path "
                                 "'{}'",
                                 receipt_.id, relative_path))};
        }
        const auto file{
            std::find_if(receipt_.files.begin(), receipt_.files.end(),
                         [relative_path](const receipt_file &entry) {
                             return entry.path == relative_path;
                         })};
        if (file == receipt_.files.end()) {
            return std::unexpected{fail(
                error_code::not_found,
                fmt::format("installed package '{}@{}' has no member '{}'",
                            receipt_.id, receipt_.version, relative_path))};
        }
        if (!table_.has_value()) {
            if (auto opened{open_store()}; !opened) {
                return std::unexpected{std::move(opened.error())};
            }
        }
        const auto location{table_->find(file->blake3)};
        if (!location) {
            return std::unexpected{
                fail(error_code::not_found,
                     fmt::format("installed package '{}@{}': blob {} for '{}' "
                                 "is missing from the content store",
                                 receipt_.id, receipt_.version,
                                 data::to_hex(file->blake3), relative_path))};
        }
        auto bytes{
            store_->read(data::blob_location_from_table_entry(*location))};
        if (!bytes) {
            return std::unexpected{
                annotate(std::move(bytes.error()),
                         fmt::format("installed package '{}'", receipt_path_))};
        }
        if (bytes->size() != file->size) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("installed package '{}@{}': member '{}' is {} "
                                 "bytes in the content store but the receipt "
                                 "records {}",
                                 receipt_.id, receipt_.version, relative_path,
                                 bytes->size(), file->size))};
        }
        return bytes;
    }

  private:
    [[nodiscard]] auto open_store() const -> status {
        const auto receipt{receipt_path_};
        const auto version_slash{receipt.rfind('/')};
        const auto id_slash{version_slash == std::string::npos
                                ? std::string::npos
                                : receipt.rfind('/', version_slash - 1)};
        const auto mods_slash{id_slash == std::string::npos
                                  ? std::string::npos
                                  : receipt.rfind('/', id_slash - 1)};
        if (mods_slash == std::string::npos || mods_slash == 0) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("installed package: receipt path '{}' is not "
                                 "<store>/mods/<id>/<version>.json",
                                 receipt))};
        }
        store_root_ = receipt.substr(0, mods_slash);
        const auto table_path{fmt::format("{}/content/{}", store_root_,
                                          data::content_blob_table_filename)};
        auto table{data::mapped_blob_table::open(table_path)};
        if (!table) {
            return std::unexpected{
                annotate(std::move(table.error()),
                         fmt::format("installed package '{}'", receipt_path_))};
        }
        auto store{data::content_store::open_existing(store_root_)};
        if (!store) {
            return std::unexpected{
                annotate(std::move(store.error()),
                         fmt::format("installed package '{}'", receipt_path_))};
        }
        table_.emplace(std::move(*table));
        store_.emplace(std::move(*store));
        return status_ok();
    }

    std::string receipt_path_{};
    mod_receipt receipt_{};
    mutable std::string store_root_{};
    mutable std::optional<data::mapped_blob_table> table_{};
    mutable std::optional<data::content_store> store_{};
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
    return package_resolver{[image = std::move(image),
                             bytes](const candidate &mod)
                                -> result<std::unique_ptr<package_source>> {
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
            // Directory side-loading is not a runtime source: a directory is a
            // packaging input, not an install, so it has no receipt, no
            // archive digest and no signature to verify. Loading one through
            // the runtime resolver would bypass the install transaction, so it
            // is refused here instead of being wrapped into a package source.
            // Tools and tests that inspect a manifest tree construct
            // `make_directory_package_source()` explicitly.
            return std::unexpected{fail(
                error_code::unsupported,
                fmt::format("mod '{}' is a directory candidate; directory "
                            "side-loading is not a runtime source, install "
                            "the package to load it",
                            mod.id))};
        }
        case package_kind::rfmod: {
            // Installed package: the receipt is the source of truth and
            // every member is read back from the content store.
            auto receipt{read_receipt(mod.path)};
            if (!receipt) {
                return std::unexpected{annotate(
                    std::move(receipt.error()),
                    fmt::format("mod '{}' receipt '{}'", mod.id, mod.path))};
            }
            if (receipt->id != mod.id || receipt->version != mod.version) {
                return std::unexpected{fail(
                    error_code::parse_error,
                    fmt::format("mod '{}': receipt '{}' records {}@{}", mod.id,
                                mod.path, receipt->id, receipt->version))};
            }
            return std::unique_ptr<package_source>{
                std::make_unique<receipt_package_source>(mod.path,
                                                         std::move(*receipt))};
        }
        }
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod '{}' has an unknown package kind", mod.id))};
    }};
}

auto make_receipt_package_source(std::string receipt_path)
    -> result<std::unique_ptr<package_source>> {
    auto receipt{read_receipt(receipt_path)};
    if (!receipt) {
        return std::unexpected{
            annotate(std::move(receipt.error()),
                     fmt::format("installed package '{}'", receipt_path))};
    }
    return std::unique_ptr<package_source>{
        std::make_unique<receipt_package_source>(std::move(receipt_path),
                                                 std::move(*receipt))};
}

} // namespace refrain::mod
