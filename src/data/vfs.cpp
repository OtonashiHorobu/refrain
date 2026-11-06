// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/vfs.hpp>

#include <refrain/archive/zip.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/rfs.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

namespace fs = std::filesystem;

/// MSVC decodes `fs::path{std::string}` with the native narrow charset, not
/// UTF-8; go through u8string so non-ASCII pack paths resolve everywhere.
[[nodiscard]] auto path_from_utf8(std::string_view text) -> fs::path {
    std::u8string utf8;
    utf8.reserve(text.size());
    for (const char character : text) {
        utf8.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return fs::path{utf8};
}

[[nodiscard]] auto utf8_string(const fs::path &path) -> std::string {
    const auto utf8{path.u8string()};
    return std::string{utf8.begin(), utf8.end()};
}

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, message);
}

/// Exact binary search over a path-sorted entry vector.
[[nodiscard]] auto find_entry(std::span<const pack_file_info> entries,
                              std::string_view path) noexcept
    -> const pack_file_info * {
    const auto match{
        std::lower_bound(entries.begin(), entries.end(), path,
                         [](const pack_file_info &entry, std::string_view key) {
                             return std::string_view{entry.path} < key;
                         })};
    if (match == entries.end() || std::string_view{match->path} != path) {
        return nullptr;
    }
    return &*match;
}

/// Sort a pack's candidate entries deterministically and reject the shapes a
/// pack may never expose: unsafe paths, duplicates and absurd counts.
[[nodiscard]] auto finish_entries(std::vector<pack_file_info> candidates)
    -> result<std::vector<pack_file_info>> {
    if (candidates.size() > vfs_max_files) {
        return std::unexpected{fail(error_code::parse_error,
                                    "pack has more than " +
                                        std::to_string(vfs_max_files) +
                                        " entries (refused before indexing)")};
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const pack_file_info &lhs, const pack_file_info &rhs) {
                  return lhs.path < rhs.path;
              });
    for (std::size_t index{0}; index < candidates.size(); ++index) {
        if (!is_safe_relative_path(candidates[index].path)) {
            return std::unexpected{
                fail(error_code::parse_error,
                     "pack contains an unsafe entry path: '" +
                         candidates[index].path + "'")};
        }
        if (index > 0 &&
            !(candidates[index - 1U].path < candidates[index].path)) {
            return std::unexpected{
                fail(error_code::parse_error,
                     "pack contains duplicate entry paths: '" +
                         candidates[index].path + "'")};
        }
    }
    return candidates;
}

// ---------------------------------------------------------------------------
// pack implementations
// ---------------------------------------------------------------------------

/// Directory-tree pack (development builds, world/server datapacks).
///
/// The entry list is captured at open() time (the VFS snapshot only ever sees
/// that list); `read()` re-reads the host file, so an externally modified file
/// is served with its current bytes and a deleted file surfaces `io_error`.
class dir_pack final : public pack {
  public:
    dir_pack(std::string root, std::string label,
             std::vector<pack_file_info> entries)
        : root_{std::move(root)}, label_{std::move(label)},
          entries_{std::move(entries)} {}

    [[nodiscard]] auto format() const noexcept -> pack_format override {
        return pack_format::directory;
    }
    [[nodiscard]] auto label() const noexcept -> std::string_view override {
        return label_;
    }
    [[nodiscard]] auto file_count() const noexcept -> std::size_t override {
        return entries_.size();
    }
    [[nodiscard]] auto entries() const noexcept
        -> std::span<const pack_file_info> override {
        return entries_;
    }
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack_file_info * override {
        return find_entry(entries_, path);
    }
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>> override {
        if (find(path) == nullptr) {
            return std::unexpected{
                fail(error_code::not_found, "directory pack '" + label_ +
                                                "' has no file '" +
                                                std::string{path} + "'")};
        }
        return core::io::read_file(join_path(root_, path));
    }

  private:
    std::string root_;
    std::string label_;
    std::vector<pack_file_info> entries_;
};

/// Zip/jar pack. libzip handles are not safe for concurrent reads, so reads are
/// serialized by a mutex: `pack::read` stays const and thread-safe, which is
/// what the post-freeze VFS contract requires.
class zip_pack final : public pack {
  public:
    zip_pack(archive::zip_reader reader, std::string label,
             std::vector<pack_file_info> entries)
        : reader_{std::move(reader)}, label_{std::move(label)},
          entries_{std::move(entries)} {}

    [[nodiscard]] auto format() const noexcept -> pack_format override {
        return pack_format::zip;
    }
    [[nodiscard]] auto label() const noexcept -> std::string_view override {
        return label_;
    }
    [[nodiscard]] auto file_count() const noexcept -> std::size_t override {
        return entries_.size();
    }
    [[nodiscard]] auto entries() const noexcept
        -> std::span<const pack_file_info> override {
        return entries_;
    }
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack_file_info * override {
        return find_entry(entries_, path);
    }
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>> override {
        if (find(path) == nullptr) {
            return std::unexpected{
                fail(error_code::not_found,
                     "zip pack '" + label_ + "' is not open")};
        }
        const std::lock_guard lock{mutex_};
        return reader_.read(path);
    }

  private:
    mutable archive::zip_reader reader_;
    std::string label_;
    std::vector<pack_file_info> entries_;
    mutable std::mutex mutex_;
};

/// RFS1 container pack (raw cook output). `rfs_reader` owns the file buffer;
/// reads copy the payload (or decompress zstd records).
class rfs_pack final : public pack {
  public:
    rfs_pack(rfs_reader reader, std::string label,
             std::vector<pack_file_info> entries)
        : reader_{std::move(reader)}, label_{std::move(label)},
          entries_{std::move(entries)} {}

    [[nodiscard]] auto format() const noexcept -> pack_format override {
        return pack_format::rfs;
    }
    [[nodiscard]] auto label() const noexcept -> std::string_view override {
        return label_;
    }
    [[nodiscard]] auto file_count() const noexcept -> std::size_t override {
        return entries_.size();
    }
    [[nodiscard]] auto entries() const noexcept
        -> std::span<const pack_file_info> override {
        return entries_;
    }
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack_file_info * override {
        return find_entry(entries_, path);
    }
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>> override {
        if (find(path) == nullptr) {
            return std::unexpected{
                fail(error_code::not_found, "RFS pack '" + label_ +
                                                "' has no file '" +
                                                std::string{path} + "'")};
        }
        return reader_.read(path);
    }

  private:
    rfs_reader reader_;
    std::string label_;
    std::vector<pack_file_info> entries_;
};

} // namespace

// ---------------------------------------------------------------------------
// pack
// ---------------------------------------------------------------------------

auto pack::read_text(std::string_view path) const -> result<std::string> {
    auto bytes{read(path)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    if (bytes->empty()) {
        return std::string{};
    }
    return std::string{reinterpret_cast<const char *>(bytes->data()), // NOLINT
                       bytes->size()};
}

// ---------------------------------------------------------------------------
// factories
// ---------------------------------------------------------------------------

auto open_dir_pack(std::string_view root, std::string_view label)
    -> result<std::unique_ptr<pack>> {
    if (root.empty()) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "directory pack root must not be empty")};
    }
    // Pack paths are UTF-8 by contract; validating before any filesystem call
    // also avoids a throwing path conversion on Windows hosts, where an
    // invalid UTF-8 name cannot be represented at all.
    if (!is_valid_utf8(root)) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "directory pack root is not valid UTF-8")};
    }
    if (!core::io::exists(root)) {
        return std::unexpected{fail(error_code::not_found,
                                    "directory pack root does not exist: '" +
                                        std::string{root} + "'")};
    }
    if (!core::io::is_directory(root)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "directory pack root is not a directory: '" +
                     std::string{root} + "'")};
    }

    const fs::path root_path{path_from_utf8(root)};
    std::error_code iterate_error;
    // The recursive walk is std::filesystem (portable, no OS calls of our own);
    // the actual file bytes are read through core::io in dir_pack::read().
    fs::recursive_directory_iterator iterator{
        root_path, fs::directory_options::skip_permission_denied,
        iterate_error};
    if (iterate_error) {
        return std::unexpected{
            fail(error_code::io_error, "failed to enumerate directory pack '" +
                                           std::string{root} +
                                           "': " + iterate_error.message())};
    }

    std::vector<pack_file_info> candidates;
    const fs::recursive_directory_iterator end{};
    while (iterator != end) {
        std::error_code status_error;
        const fs::file_status status{iterator->symlink_status(status_error)};
        if (status_error) {
            return std::unexpected{fail(
                error_code::io_error, "failed to stat directory pack entry '" +
                                          utf8_string(iterator->path()) +
                                          "': " + status_error.message())};
        }
        if (fs::is_symlink(status)) {
            // Symlinks could point outside the pack root; skipping them keeps
            // the snapshot self-contained and deterministic.
            iterator.disable_recursion_pending();
        } else if (fs::is_regular_file(status)) {
            std::error_code size_error;
            const auto size{iterator->file_size(size_error)};
            if (size_error) {
                return std::unexpected{
                    fail(error_code::io_error,
                         "failed to stat directory pack file '" +
                             utf8_string(iterator->path()) +
                             "': " + size_error.message())};
            }
            const auto relative{iterator->path().lexically_relative(root_path)};
            const auto relative_utf8{relative.generic_u8string()};
            candidates.push_back(pack_file_info{
                .path = std::string{relative_utf8.begin(), relative_utf8.end()},
                .size = static_cast<std::uint64_t>(size)});
            if (candidates.size() > vfs_max_files) {
                return std::unexpected{fail(error_code::parse_error,
                                            "directory pack has more than " +
                                                std::to_string(vfs_max_files) +
                                                " files")};
            }
        }

        iterator.increment(iterate_error);
        if (iterate_error) {
            return std::unexpected{fail(error_code::io_error,
                                        "failed to enumerate directory pack '" +
                                            std::string{root} +
                                            "': " + iterate_error.message())};
        }
    }

    auto entries{finish_entries(std::move(candidates))};
    if (!entries) {
        return std::unexpected{std::move(entries.error())};
    }
    return std::unique_ptr<pack>{std::make_unique<dir_pack>(
        std::string{root},
        label.empty() ? std::string{root} : std::string{label},
        std::move(*entries))};
}

auto open_zip_pack(std::string_view path, std::string_view label)
    -> result<std::unique_ptr<pack>> {
    if (path.empty()) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "zip pack path must not be empty")};
    }
    auto reader{archive::zip_reader::open(path)};
    if (!reader) {
        return std::unexpected{std::move(reader.error())};
    }

    std::vector<pack_file_info> candidates;
    candidates.reserve(reader->entries().size());
    for (const auto &entry : reader->entries()) {
        if (entry.directory) {
            continue;
        }
        // Zip content can be extracted to a host filesystem; apply the stricter
        // portable-name rules instead of the looser virtual-path rules.
        if (!archive::safe_relative_path(entry.name)) {
            return std::unexpected{
                fail(error_code::parse_error,
                     "zip pack contains an unsafe entry name: '" + entry.name +
                         "'")};
        }
        candidates.push_back(
            pack_file_info{.path = entry.name, .size = entry.size});
        if (candidates.size() > vfs_max_files) {
            return std::unexpected{fail(error_code::parse_error,
                                        "zip pack has more than " +
                                            std::to_string(vfs_max_files) +
                                            " entries")};
        }
    }

    auto entries{finish_entries(std::move(candidates))};
    if (!entries) {
        return std::unexpected{std::move(entries.error())};
    }
    return std::unique_ptr<pack>{std::make_unique<zip_pack>(
        std::move(*reader),
        label.empty() ? std::string{path} : std::string{label},
        std::move(*entries))};
}

auto open_rfs_pack(std::string_view path, std::string_view label)
    -> result<std::unique_ptr<pack>> {
    auto reader{rfs_reader::open_file(path)};
    if (!reader) {
        return std::unexpected{std::move(reader.error())};
    }
    std::vector<pack_file_info> candidates{};
    candidates.reserve(reader->file_count());
    for (const auto &entry : reader->entries()) {
        if (!is_safe_relative_path(entry.key)) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                "RFS pack '" + std::string{path} +
                    "' contains an unsafe record key '" + entry.key + "'")};
        }
        candidates.push_back(
            pack_file_info{.path = entry.key, .size = entry.data_len});
    }
    auto entries{finish_entries(std::move(candidates))};
    if (!entries) {
        return std::unexpected{std::move(entries.error())};
    }
    return std::make_unique<rfs_pack>(std::move(*reader),
                                      label.empty() ? std::string{path}
                                                    : std::string{label},
                                      std::move(*entries));
}

// ---------------------------------------------------------------------------
// vfs
// ---------------------------------------------------------------------------

struct vfs::impl {
    /// Mounted packs in priority order (lowest first, highest last).
    std::vector<std::unique_ptr<pack>> packs{};
    std::vector<mounted_pack_info> mounted{};
    /// Merged, path-sorted view. `owners[i]` is the index into `packs` that
    /// provides `files[i]` (the highest-priority pack containing the path).
    std::vector<pack_file_info> files{};
    std::vector<std::uint32_t> owners{};
    /// Sum of the file counts of all mounted packs before overrides: the VFS
    /// bound is expressed over this (a hostile stack cannot hide work behind
    /// heavy cross-pack duplication).
    std::size_t mounted_files{0};
    bool frozen{false};
};

vfs::vfs() : impl_{std::make_shared<impl>()} {}

auto vfs::mount(std::unique_ptr<pack> source) -> status {
    if (!source) {
        return std::unexpected{
            fail(error_code::invalid_argument, "cannot mount a null pack")};
    }
    if (!impl_) {
        impl_ = std::make_shared<impl>();
    }
    if (impl_->frozen) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "vfs is frozen: mounting is only allowed before freeze()")};
    }

    const auto entries{source->entries()};
    if (source->file_count() != entries.size()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "pack '" + std::string{source->label()} +
                     "' reports a file count that disagrees with its entries")};
    }
    if (entries.empty()) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "cannot mount an empty pack: '" +
                                        std::string{source->label()} +
                                        "' has no files")};
    }
    for (std::size_t index{0}; index < entries.size(); ++index) {
        if (!is_safe_relative_path(entries[index].path)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "pack '" + std::string{source->label()} +
                         "' exposes an unsafe entry path: '" +
                         entries[index].path + "'")};
        }
        if (index > 0 && !(entries[index - 1U].path < entries[index].path)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "pack '" + std::string{source->label()} +
                         "' entries are not strictly sorted by path")};
        }
    }
    if (entries.size() > vfs_max_files ||
        impl_->mounted_files > vfs_max_files - entries.size()) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "vfs file limit (" +
                                        std::to_string(vfs_max_files) +
                                        ") would be exceeded by mounting '" +
                                        std::string{source->label()} + "'")};
    }

    // Merge in priority order: the candidate (later mount) wins on equal paths,
    // existing lower-priority entries are kept otherwise.
    const auto new_index{static_cast<std::uint32_t>(impl_->packs.size())};
    std::vector<pack_file_info> merged;
    std::vector<std::uint32_t> merged_owners;
    merged.reserve(impl_->files.size() + entries.size());
    merged_owners.reserve(impl_->files.size() + entries.size());
    std::size_t existing_position{0};
    std::size_t candidate_position{0};
    while (existing_position < impl_->files.size() &&
           candidate_position < entries.size()) {
        const auto &existing{impl_->files[existing_position]};
        const auto &candidate{entries[candidate_position]};
        if (existing.path < candidate.path) {
            merged.push_back(existing);
            merged_owners.push_back(impl_->owners[existing_position]);
            ++existing_position;
        } else if (candidate.path < existing.path) {
            merged.push_back(candidate);
            merged_owners.push_back(new_index);
            ++candidate_position;
        } else {
            // Cross-pack duplicate: highest priority (the new pack) wins.
            merged.push_back(candidate);
            merged_owners.push_back(new_index);
            ++existing_position;
            ++candidate_position;
        }
    }
    for (; existing_position < impl_->files.size(); ++existing_position) {
        merged.push_back(impl_->files[existing_position]);
        merged_owners.push_back(impl_->owners[existing_position]);
    }
    for (; candidate_position < entries.size(); ++candidate_position) {
        merged.push_back(entries[candidate_position]);
        merged_owners.push_back(new_index);
    }

    impl_->files = std::move(merged);
    impl_->owners = std::move(merged_owners);
    impl_->mounted_files += entries.size();
    impl_->mounted.push_back(mounted_pack_info{
        .label = std::string{source->label()},
        .format = source->format(),
        .file_count = entries.size(),
    });
    impl_->packs.push_back(std::move(source));
    return status_ok();
}

auto vfs::mount(const pack_source &source) -> status {
    if (!impl_) {
        impl_ = std::make_shared<impl>();
    }
    if (impl_->frozen) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "vfs is frozen: mounting is only allowed before freeze()")};
    }
    const auto label{source.label.empty() ? source.path : source.label};
    switch (source.format) {
    case pack_format::directory: {
        auto opened{open_dir_pack(source.path, label)};
        if (!opened) {
            return std::unexpected{std::move(opened.error())};
        }
        return mount(std::move(*opened));
    }
    case pack_format::zip: {
        auto opened{open_zip_pack(source.path, label)};
        if (!opened) {
            return std::unexpected{std::move(opened.error())};
        }
        return mount(std::move(*opened));
    }
    case pack_format::rfs: {
        auto opened{open_rfs_pack(source.path, label)};
        if (!opened) {
            return std::unexpected{std::move(opened.error())};
        }
        return mount(std::move(*opened));
    }
    case pack_format::memory:
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "memory packs carry caller-owned bytes: use "
                 "data::open_builtin_image_pack() + mount(unique_ptr)")};
    case pack_format::content:
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "content packs need a store and a composition: use "
                 "open_content_pack() + mount(unique_ptr)")};
    }
    return std::unexpected{
        fail(error_code::invalid_argument, "unknown pack format")};
}

auto vfs::pack_count() const noexcept -> std::size_t {
    return impl_ ? impl_->packs.size() : 0U;
}

auto vfs::file_count() const noexcept -> std::size_t {
    return impl_ ? impl_->files.size() : 0U;
}

auto vfs::mounted() const noexcept -> std::span<const mounted_pack_info> {
    if (!impl_) {
        return {};
    }
    return impl_->mounted;
}

auto vfs::packs() const -> std::vector<const pack *> {
    if (!impl_) {
        return {};
    }
    std::vector<const pack *> layers{};
    layers.reserve(impl_->packs.size());
    for (const auto &layer : impl_->packs) {
        layers.push_back(layer.get());
    }
    return layers;
}

auto vfs::find(std::string_view path) const noexcept -> const pack * {
    if (!impl_ || impl_->files.empty()) {
        return nullptr;
    }
    const auto match{
        std::lower_bound(impl_->files.begin(), impl_->files.end(), path,
                         [](const pack_file_info &entry, std::string_view key) {
                             return std::string_view{entry.path} < key;
                         })};
    if (match == impl_->files.end() || std::string_view{match->path} != path) {
        return nullptr;
    }
    const auto index{static_cast<std::size_t>(match - impl_->files.begin())};
    const auto owner{impl_->owners[index]};
    if (owner >= impl_->packs.size()) {
        return nullptr;
    }
    return impl_->packs[owner].get();
}

auto vfs::exists(std::string_view path) const noexcept -> bool {
    return find(path) != nullptr;
}

auto vfs::size(std::string_view path) const -> result<std::uint64_t> {
    if (!impl_) {
        return std::unexpected{
            fail(error_code::not_initialized, "vfs has no mounted packs")};
    }
    const auto match{
        std::lower_bound(impl_->files.begin(), impl_->files.end(), path,
                         [](const pack_file_info &entry, std::string_view key) {
                             return std::string_view{entry.path} < key;
                         })};
    if (match == impl_->files.end() || std::string_view{match->path} != path) {
        return std::unexpected{
            fail(error_code::not_found,
                 "vfs has no file '" + std::string{path} + "'")};
    }
    return match->size;
}

auto vfs::read(std::string_view path) const -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected{
            fail(error_code::not_initialized, "vfs has no mounted packs")};
    }
    const auto *winner{find(path)};
    if (winner == nullptr) {
        return std::unexpected{
            fail(error_code::not_found,
                 "vfs has no file '" + std::string{path} + "'")};
    }
    return winner->read(path);
}

auto vfs::read_text(std::string_view path) const -> result<std::string> {
    auto bytes{read(path)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    if (bytes->empty()) {
        return std::string{};
    }
    return std::string{reinterpret_cast<const char *>(bytes->data()), // NOLINT
                       bytes->size()};
}

auto vfs::list() const noexcept -> const std::vector<pack_file_info> & {
    static const std::vector<pack_file_info> empty{};
    return impl_ ? impl_->files : empty;
}

auto vfs::list_prefix(std::string_view prefix) const
    -> std::vector<pack_file_info> {
    std::vector<pack_file_info> result;
    if (!impl_) {
        return result;
    }
    auto iterator{
        std::lower_bound(impl_->files.begin(), impl_->files.end(), prefix,
                         [](const pack_file_info &entry, std::string_view key) {
                             return std::string_view{entry.path} < key;
                         })};
    for (; iterator != impl_->files.end() &&
           std::string_view{iterator->path}.starts_with(prefix);
         ++iterator) {
        result.push_back(*iterator);
    }
    return result;
}

auto vfs::list_directory(std::string_view directory) const
    -> std::vector<std::string> {
    if (!impl_) {
        return {};
    }
    // Accept "data/" as "data"; the empty string lists the stack root.
    std::string_view normalized{directory};
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.remove_suffix(1);
    }
    std::string prefix;
    if (!normalized.empty()) {
        if (!is_safe_relative_path(normalized)) {
            return {};
        }
        prefix.assign(normalized);
        prefix.push_back('/');
    }

    std::vector<std::string> children;
    auto iterator{std::lower_bound(
        impl_->files.begin(), impl_->files.end(), std::string_view{prefix},
        [](const pack_file_info &entry, std::string_view key) {
            return std::string_view{entry.path} < key;
        })};
    for (; iterator != impl_->files.end() &&
           std::string_view{iterator->path}.starts_with(prefix);
         ++iterator) {
        auto remainder{std::string_view{iterator->path}};
        remainder.remove_prefix(prefix.size());
        const auto slash{remainder.find('/')};
        if (slash == std::string_view::npos) {
            children.emplace_back(remainder);
        } else {
            std::string child{remainder.substr(0, slash)};
            child.push_back('/');
            children.push_back(std::move(child));
        }
    }
    // The merged view only yields each direct child once, and iterating a
    // sorted path list already yields them grouped; normalize defensively.
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()),
                   children.end());
    return children;
}

void vfs::freeze() {
    if (!impl_) {
        impl_ = std::make_shared<impl>();
    }
    impl_->frozen = true;
}

auto vfs::is_frozen() const noexcept -> bool {
    return impl_ != nullptr && impl_->frozen;
}

} // namespace refrain::data
