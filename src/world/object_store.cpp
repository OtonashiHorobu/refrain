// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/object_store.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/world/rwf.hpp>

#include "rwf_internal.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

constexpr std::string_view index_magic{"RFOS"};
constexpr std::uint16_t index_version{1};
constexpr std::uint32_t segment_target{32U * 1024U * 1024U};
constexpr std::size_t index_header_bytes{16};
constexpr std::uint32_t index_max_entries{1'000'000U};

struct entry {
    std::uint64_t segment{0};
    std::uint32_t offset{0};
    std::uint32_t stored{0};
    std::uint32_t uncompressed{0};
    std::uint32_t crc32c{0};
    object_media media{object_media::binary};
    rwf::compression codec{rwf::compression::none};
    bool large{false};
};

struct pending_object {
    std::vector<std::byte> bytes;
    object_media media{object_media::binary};
    bool large{false};
    std::uint64_t file_id{0};
    std::uint32_t file_size{0};
    std::uint32_t file_crc32c{0};
    std::uint32_t logical_size{0};
};

[[nodiscard]] auto digest_id(const data::digest32 &digest) noexcept
    -> std::uint64_t {
    std::uint64_t id{0};
    for (std::size_t index{0}; index < 8; ++index) {
        id |= static_cast<std::uint64_t>(
                  std::to_integer<unsigned int>(digest[index]))
              << (8U * index);
    }
    return id;
}

[[nodiscard]] auto magic_matches(std::span<const std::byte> bytes,
                                 std::size_t offset,
                                 std::string_view magic) noexcept -> bool {
    if (offset > bytes.size() || magic.size() > bytes.size() - offset) {
        return false;
    }
    for (std::size_t index{0}; index < magic.size(); ++index) {
        if (std::to_integer<unsigned char>(bytes[offset + index]) !=
            static_cast<unsigned char>(magic[index])) {
            return false;
        }
    }
    return true;
}

void append_u8(std::vector<std::byte> &out, std::uint8_t value) {
    const auto offset{out.size()};
    out.resize(offset + 1);
    rwf::detail::put_u8(out, offset, value);
}
void append_u16(std::vector<std::byte> &out, std::uint16_t value) {
    const auto offset{out.size()};
    out.resize(offset + 2);
    rwf::detail::put_u16(out, offset, value);
}
void append_u32(std::vector<std::byte> &out, std::uint32_t value) {
    const auto offset{out.size()};
    out.resize(offset + 4);
    rwf::detail::put_u32(out, offset, value);
}
void append_u64(std::vector<std::byte> &out, std::uint64_t value) {
    const auto offset{out.size()};
    out.resize(offset + 8);
    rwf::detail::put_u64(out, offset, value);
}
void append_text(std::vector<std::byte> &out, std::string_view text) {
    out.insert(out.end(), reinterpret_cast<const std::byte *>(text.data()),
               reinterpret_cast<const std::byte *>(text.data()) + text.size());
}

[[nodiscard]] auto parse_failure(std::string message)
    -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
}
[[nodiscard]] auto failing(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

} // namespace

struct object_store::impl {
    std::string root;
    mutable std::map<std::string, entry> committed;
    std::map<std::string, pending_object> pending;
    mutable bool dirty{false};
    mutable std::map<std::uint64_t, std::unique_ptr<data::segment_reader>>
        readers;

    [[nodiscard]] auto index_path() const -> std::string {
        return fmt::format("{}/index.rfw", root);
    }
    [[nodiscard]] auto segments_dir() const -> std::string {
        return fmt::format("{}/segments", root);
    }
    [[nodiscard]] auto large_dir() const -> std::string {
        return fmt::format("{}/large", root);
    }

    mutable bool loaded{false};

    [[nodiscard]] auto load_once() const -> status {
        if (loaded) {
            return status_ok();
        }
        const auto path{index_path()};
        if (!core::io::exists(path)) {
            loaded = true;
            return status_ok();
        }
        auto bytes{rwf::read_record_file(path, 64ULL * 1024ULL * 1024ULL)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        auto decoded{decode_index(*bytes, committed)};
        if (!decoded) {
            return decoded;
        }
        loaded = true;
        return status_ok();
    }

    [[nodiscard]] static auto decode_index(std::span<const std::byte> bytes,
                                           std::map<std::string, entry> &out)
        -> status {
        if (bytes.size() < index_header_bytes) {
            return parse_failure("object index is shorter than its header");
        }
        if (!magic_matches(bytes, 0, index_magic)) {
            return parse_failure("object index has a bad RFOS magic");
        }
        if (rwf::detail::get_u16(bytes, 4) != index_version ||
            rwf::detail::get_u16(bytes, 6) != 0 ||
            rwf::detail::get_u32(bytes, 12) != 0) {
            return parse_failure("object index has an unsupported header");
        }
        const auto count{rwf::detail::get_u32(bytes, 8)};
        if (count > index_max_entries) {
            return parse_failure(
                fmt::format("object index claims {} entries", count));
        }
        std::size_t offset{index_header_bytes};
        out.clear();
        for (std::uint32_t index{0}; index < count; ++index) {
            if (offset + 2 > bytes.size()) {
                return parse_failure("object index entry is truncated");
            }
            const auto path_length{rwf::detail::get_u16(bytes, offset)};
            offset += 2;
            if (path_length == 0 || offset + path_length + 32 > bytes.size()) {
                return parse_failure("object index entry path is invalid");
            }
            const std::string path{
                reinterpret_cast<const char *>(bytes.data() + offset),
                path_length};
            offset += path_length;
            if (!object_store::valid_path(path)) {
                return parse_failure(
                    fmt::format("object index has unsafe path '{}'", path));
            }
            entry value{};
            const auto media{
                static_cast<object_media>(rwf::detail::get_u8(bytes, offset))};
            const auto codec{static_cast<rwf::compression>(
                rwf::detail::get_u8(bytes, offset + 1))};
            const auto flags{rwf::detail::get_u16(bytes, offset + 2)};
            offset += 4;
            if (flags > 1U ||
                static_cast<std::uint8_t>(media) >
                    static_cast<std::uint8_t>(object_media::text) ||
                static_cast<std::uint8_t>(codec) >
                    static_cast<std::uint8_t>(rwf::compression::zstd)) {
                return parse_failure("object index entry has invalid flags");
            }
            value.media = media;
            value.codec = codec;
            value.large = flags == 1U;
            value.stored = rwf::detail::get_u32(bytes, offset);
            value.uncompressed = rwf::detail::get_u32(bytes, offset + 4);
            value.crc32c = rwf::detail::get_u32(bytes, offset + 8);
            value.segment = rwf::detail::get_u64(bytes, offset + 12);
            value.offset = rwf::detail::get_u32(bytes, offset + 20);
            offset += 24;
            if (rwf::detail::get_u32(bytes, offset) != 0) {
                return parse_failure("object index entry reserved field set");
            }
            offset += 4;
            if (value.segment == 0 || value.stored > max_object_bytes ||
                value.uncompressed > max_object_bytes) {
                return parse_failure("object index entry is out of bounds");
            }
            if (!out.emplace(path, value).second) {
                return parse_failure(
                    fmt::format("object index has duplicate path '{}'", path));
            }
        }
        if (offset != bytes.size()) {
            return parse_failure("object index has trailing bytes");
        }
        return status_ok();
    }

    [[nodiscard]] static auto
    encode_index(const std::map<std::string, entry> &entries)
        -> std::vector<std::byte> {
        std::vector<std::byte> payload;
        payload.reserve(index_header_bytes + entries.size() * 48U);
        append_text(payload, index_magic);
        append_u16(payload, index_version);
        append_u16(payload, 0);
        append_u32(payload, static_cast<std::uint32_t>(entries.size()));
        append_u32(payload, 0);
        for (const auto &[path, value] : entries) {
            append_u16(payload, static_cast<std::uint16_t>(path.size()));
            append_text(payload, path);
            append_u8(payload, static_cast<std::uint8_t>(value.media));
            append_u8(payload, static_cast<std::uint8_t>(value.codec));
            append_u16(payload, value.large ? 1U : 0U);
            append_u32(payload, value.stored);
            append_u32(payload, value.uncompressed);
            append_u32(payload, value.crc32c);
            append_u64(payload, value.segment);
            append_u32(payload, value.offset);
            append_u32(payload, 0);
        }
        return payload;
    }

    [[nodiscard]] auto read_large(std::uint64_t id, std::uint32_t stored,
                                  std::uint32_t crc32c) const
        -> result<std::vector<std::byte>> {
        const auto path{fmt::format("{}/{:016x}.rfbl", large_dir(), id)};
        auto bytes{core::io::read_file(path, {.max_bytes = stored})};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (data::crc32c(*bytes) != crc32c) {
            return parse_failure("large object failed its CRC32C check");
        }
        return data::decode_packed_blob(*bytes);
    }

    [[nodiscard]] auto read_segment(std::uint64_t id) const
        -> result<data::segment_reader *> {
        if (const auto found{readers.find(id)}; found != readers.end()) {
            return found->second.get();
        }
        const auto path{fmt::format("{}/{:016x}.rfseg", segments_dir(), id)};
        auto reader{data::segment_reader::open(path)};
        if (!reader) {
            return std::unexpected{reader.error()};
        }
        auto owned{std::make_unique<data::segment_reader>(std::move(*reader))};
        auto *raw{owned.get()};
        readers.emplace(id, std::move(owned));
        return raw;
    }

    [[nodiscard]] auto commit_pending() -> status {
        if (!pending.empty()) {
            bool has_small{false};
            for (const auto &[path, object] : pending) {
                static_cast<void>(path);
                if (!object.large) {
                    has_small = true;
                }
            }
            std::unique_ptr<data::segment_writer> writer;
            if (has_small) {
                if (auto created{core::io::create_directories(segments_dir())};
                    !created) {
                    return created;
                }
                auto created{data::segment_writer::create(segments_dir(),
                                                          segment_target)};
                if (!created) {
                    return std::unexpected{created.error()};
                }
                writer =
                    std::make_unique<data::segment_writer>(std::move(*created));
            }
            std::map<std::string, entry> fresh;
            std::size_t small_appended{0};
            for (const auto &[path, object] : pending) {
                if (object.large) {
                    entry value{};
                    value.large = true;
                    value.segment = object.file_id;
                    value.stored = object.file_size;
                    value.uncompressed = object.logical_size;
                    value.crc32c = object.file_crc32c;
                    value.media = object.media;
                    value.codec = rwf::compression::none;
                    committed[path] = value;
                    continue;
                }
                auto stored{rwf::detail::compress(rwf::compression::zstd, 3,
                                                  object.bytes)};
                if (!stored) {
                    return std::unexpected{stored.error()};
                }
                auto codec{rwf::compression::zstd};
                if (stored->size() >= object.bytes.size()) {
                    codec = rwf::compression::none;
                    *stored = object.bytes;
                }
                auto offset{writer->append(*stored)};
                if (!offset) {
                    return std::unexpected{offset.error()};
                }
                ++small_appended;
                entry value{};
                value.offset = *offset;
                value.stored = static_cast<std::uint32_t>(stored->size());
                value.uncompressed =
                    static_cast<std::uint32_t>(object.bytes.size());
                value.crc32c = data::crc32c(*stored);
                value.media = object.media;
                value.codec = codec;
                fresh.emplace(path, value);
            }
            if (writer && small_appended != 0) {
                auto header{writer->seal()};
                if (!header) {
                    return std::unexpected{header.error()};
                }
                for (auto &[path, value] : fresh) {
                    value.segment = header->id;
                    committed[path] = value;
                }
            } else {
                writer.reset();
            }
            pending.clear();
            dirty = true;
        }
        if (!dirty) {
            return status_ok();
        }
        auto payload{encode_index(committed)};
        if (auto written{rwf::write_record_file(
                index_path(), payload, rwf::compression::zstd, 3, true)};
            !written) {
            return written;
        }
        dirty = false;
        return status_ok();
    }
};

object_store::object_store(std::string root) : impl_{std::make_unique<impl>()} {
    impl_->root = std::move(root);
    while (!impl_->root.empty() && impl_->root.back() == '/') {
        impl_->root.pop_back();
    }
}

object_store::~object_store() = default;
object_store::object_store(object_store &&other) noexcept = default;
auto object_store::operator=(object_store &&other) noexcept
    -> object_store & = default;

auto object_store::root() const -> const std::string & { return impl_->root; }

auto object_store::valid_path(std::string_view path) -> bool {
    if (path.empty() || path.size() > 512 || path.front() == '/' ||
        path.back() == '/') {
        return false;
    }
    std::size_t components{0};
    std::size_t start{0};
    for (std::size_t index{0}; index <= path.size(); ++index) {
        if (index == path.size() || path[index] == '/') {
            const auto component{path.substr(start, index - start)};
            if (component.empty() || component == "." || component == "..") {
                return false;
            }
            ++components;
            if (components > 32) {
                return false;
            }
            start = index + 1;
            continue;
        }
        const auto character{path[index]};
        const auto allowed{(character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') ||
                           character == '_' || character == '-' ||
                           character == '.' || character == '/' ||
                           character == '%'};
        if (!allowed) {
            return false;
        }
    }
    return true;
}

auto object_store::write(std::string_view path,
                         std::span<const std::byte> payload, object_media media,
                         bool durable) -> status {
    if (!valid_path(path)) {
        return failing(error_code::invalid_argument,
                       fmt::format("invalid object path '{}'", path));
    }
    if (static_cast<std::uint8_t>(media) >
        static_cast<std::uint8_t>(object_media::text)) {
        return failing(error_code::invalid_argument, "invalid object media");
    }
    if (auto loaded{impl_->load_once()}; !loaded) {
        return loaded;
    }
    if (payload.size() > max_object_bytes) {
        return failing(error_code::invalid_argument,
                       fmt::format("object '{}' is {} bytes (limit {})", path,
                                   payload.size(), max_object_bytes));
    }
    pending_object pending{};
    pending.media = media;
    if (payload.size() > rwf::max_record_bytes) {
        auto packed{data::encode_packed_blob(payload)};
        if (!packed) {
            return std::unexpected{packed.error()};
        }
        if (packed->size() > max_object_bytes) {
            return failing(error_code::invalid_argument,
                           fmt::format("packed object '{}' exceeds the {} byte "
                                       "limit",
                                       path, max_object_bytes));
        }
        if (auto created{core::io::create_directories(impl_->large_dir())};
            !created) {
            return created;
        }
        const auto id{digest_id(data::blake3(*packed))};
        const auto file{fmt::format("{}/{:016x}.rfbl", impl_->large_dir(), id)};
        if (auto written{core::io::atomic_write_file(file, *packed)};
            !written) {
            return written;
        }
        pending.large = true;
        pending.file_id = id;
        pending.file_size = static_cast<std::uint32_t>(packed->size());
        pending.file_crc32c = data::crc32c(*packed);
        pending.logical_size = static_cast<std::uint32_t>(payload.size());
    } else {
        pending.bytes.assign(payload.begin(), payload.end());
    }
    impl_->pending[std::string{path}] = std::move(pending);
    if (!durable) {
        return status_ok();
    }
    return impl_->commit_pending();
}

auto object_store::read(std::string_view path, std::size_t max_bytes)
    -> result<std::vector<std::byte>> {
    if (!valid_path(path)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("invalid object path '{}'", path))};
    }
    if (auto loaded{impl_->load_once()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    if (const auto found{impl_->pending.find(std::string{path})};
        found != impl_->pending.end()) {
        if (found->second.large) {
            if (found->second.logical_size > max_bytes) {
                return std::unexpected{make_error(
                    error_code::invalid_argument,
                    fmt::format("object '{}' exceeds the read limit", path))};
            }
            return impl_->read_large(found->second.file_id,
                                     found->second.file_size,
                                     found->second.file_crc32c);
        }
        if (found->second.bytes.size() > max_bytes) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("object '{}' exceeds the read limit", path))};
        }
        return found->second.bytes;
    }
    const auto found{impl_->committed.find(std::string{path})};
    if (found == impl_->committed.end()) {
        return std::unexpected{
            make_error(error_code::not_found,
                       fmt::format("object '{}' does not exist", path))};
    }
    const auto &value{found->second};
    if (value.uncompressed > max_bytes) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("object '{}' exceeds the read limit", path))};
    }
    if (value.large) {
        return impl_->read_large(value.segment, value.stored, value.crc32c);
    }
    auto reader{impl_->read_segment(value.segment)};
    if (!reader) {
        return std::unexpected{reader.error()};
    }
    auto stored{(*reader)->read_at(value.offset, value.stored)};
    if (!stored) {
        return std::unexpected{stored.error()};
    }
    if (data::crc32c(*stored) != value.crc32c) {
        return parse_failure(
            fmt::format("object '{}' failed its CRC32C check", path));
    }
    auto payload{
        rwf::detail::decompress(value.codec, *stored, value.uncompressed)};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    if (payload->size() != value.uncompressed) {
        return parse_failure(
            fmt::format("object '{}' decoded to {} bytes, expected {}", path,
                        payload->size(), value.uncompressed));
    }
    return payload;
}

auto object_store::erase(std::string_view path) -> status {
    if (!valid_path(path)) {
        return failing(error_code::invalid_argument,
                       fmt::format("invalid object path '{}'", path));
    }
    if (auto loaded{impl_->load_once()}; !loaded) {
        return loaded;
    }
    bool found{impl_->pending.erase(std::string{path}) != 0};
    found = impl_->committed.erase(std::string{path}) != 0 || found;
    if (!found) {
        return failing(error_code::not_found,
                       fmt::format("object '{}' does not exist", path));
    }
    impl_->dirty = true;
    return impl_->commit_pending();
}

auto object_store::exists(std::string_view path) const -> bool {
    if (!valid_path(path)) {
        return false;
    }
    if (auto loaded{impl_->load_once()}; !loaded) {
        return false;
    }
    return impl_->pending.contains(std::string{path}) ||
           impl_->committed.contains(std::string{path});
}

auto object_store::list(std::string_view prefix) const
    -> result<std::vector<std::string>> {
    auto prefix_check{prefix};
    if (!prefix_check.empty() && prefix_check.back() == '/') {
        prefix_check.remove_suffix(1);
    }
    if (!prefix_check.empty() && !valid_path(prefix_check)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("invalid object prefix '{}'", prefix))};
    }
    if (auto loaded{impl_->load_once()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    std::set<std::string> paths;
    const auto include = [&](std::string_view path) {
        if (prefix.empty() || path.starts_with(prefix)) {
            paths.insert(std::string{path});
        }
    };
    for (const auto &[path, value] : impl_->committed) {
        static_cast<void>(value);
        include(path);
    }
    for (const auto &[path, object] : impl_->pending) {
        static_cast<void>(object);
        include(path);
    }
    return std::vector<std::string>{paths.begin(), paths.end()};
}

auto object_store::commit() -> status {
    if (auto loaded{impl_->load_once()}; !loaded) {
        return loaded;
    }
    return impl_->commit_pending();
}

auto object_store::compact() -> result<object_store_stats> {
    if (auto committed{commit()}; !committed) {
        return std::unexpected{committed.error()};
    }
    std::map<std::string, entry> next;
    std::set<std::uint64_t> keep;
    std::set<std::uint64_t> keep_large;
    std::uint64_t live_bytes{0};
    std::uint64_t old_bytes{0};
    const auto directory{impl_->segments_dir()};
    if (core::io::is_directory(directory)) {
        auto names{core::io::list_directory(directory)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            if (!name.ends_with(".rfseg")) {
                continue;
            }
            const auto size{
                core::io::file_size(fmt::format("{}/{}", directory, name))};
            if (!size) {
                return std::unexpected{size.error()};
            }
            old_bytes += *size;
        }
    }
    if (!impl_->committed.empty()) {
        if (auto created{core::io::create_directories(directory)}; !created) {
            return std::unexpected{created.error()};
        }
        auto writer{data::segment_writer::create(directory, segment_target)};
        if (!writer) {
            return std::unexpected{writer.error()};
        }
        std::size_t small_appended{0};
        for (const auto &[path, value] : impl_->committed) {
            if (value.large) {
                next.emplace(path, value);
                keep_large.insert(value.segment);
                live_bytes += value.stored;
                continue;
            }
            auto reader{impl_->read_segment(value.segment)};
            if (!reader) {
                return std::unexpected{reader.error()};
            }
            auto stored{(*reader)->read_at(value.offset, value.stored)};
            if (!stored) {
                return std::unexpected{stored.error()};
            }
            if (data::crc32c(*stored) != value.crc32c) {
                return parse_failure(
                    fmt::format("object '{}' failed its CRC32C check", path));
            }
            auto offset{writer->append(*stored)};
            if (!offset) {
                return std::unexpected{offset.error()};
            }
            ++small_appended;
            entry copy = value;
            copy.segment = 0;
            copy.offset = *offset;
            next.emplace(path, copy);
            live_bytes += value.stored;
        }
        if (small_appended != 0) {
            auto header{writer->seal()};
            if (!header) {
                return std::unexpected{header.error()};
            }
            for (auto &[path, value] : next) {
                static_cast<void>(path);
                if (!value.large) {
                    value.segment = header->id;
                }
            }
            keep.insert(header->id);
        }
    }
    auto payload{impl::encode_index(next)};
    if (auto written{rwf::write_record_file(impl_->index_path(), payload,
                                            rwf::compression::zstd, 3, true)};
        !written) {
        return std::unexpected{written.error()};
    }
    impl_->committed = std::move(next);
    impl_->readers.clear();
    impl_->dirty = false;
    if (core::io::is_directory(directory)) {
        auto names{core::io::list_directory(directory)};
        if (names) {
            for (const auto &name : *names) {
                if (!name.ends_with(".rfseg")) {
                    continue;
                }
                std::uint64_t id{0};
                const auto stem{name.substr(0, name.size() - 6)};
                const auto parsed{std::from_chars(
                    stem.data(), stem.data() + stem.size(), id, 16)};
                if (parsed.ec == std::errc{} &&
                    parsed.ptr == stem.data() + stem.size() &&
                    keep.contains(id)) {
                    continue;
                }
                std::error_code error;
                std::filesystem::remove(std::filesystem::path{fmt::format(
                                            "{}/{}", directory, name)},
                                        error);
            }
        }
    }
    const auto large_directory{impl_->large_dir()};
    if (core::io::is_directory(large_directory)) {
        auto names{core::io::list_directory(large_directory)};
        if (names) {
            for (const auto &name : *names) {
                if (!name.ends_with(".rfbl")) {
                    continue;
                }
                std::uint64_t id{0};
                const auto stem{name.substr(0, name.size() - 5)};
                const auto parsed{std::from_chars(
                    stem.data(), stem.data() + stem.size(), id, 16)};
                if (parsed.ec == std::errc{} &&
                    parsed.ptr == stem.data() + stem.size() &&
                    keep_large.contains(id)) {
                    continue;
                }
                std::error_code error;
                std::filesystem::remove(std::filesystem::path{fmt::format(
                                            "{}/{}", large_directory, name)},
                                        error);
            }
        }
    }
    std::size_t segments{0};
    if (core::io::is_directory(directory)) {
        auto names{core::io::list_directory(directory)};
        if (names) {
            for (const auto &name : *names) {
                if (name.ends_with(".rfseg")) {
                    ++segments;
                }
            }
        }
    }
    object_store_stats stats{};
    stats.objects = impl_->committed.size();
    stats.segments = segments;
    stats.live_bytes = live_bytes;
    stats.garbage_bytes = old_bytes > live_bytes ? old_bytes - live_bytes : 0;
    return stats;
}

// ---------------------------------------------------------------------------
// blob_store: kind/key view over object_store.
// ---------------------------------------------------------------------------

rwf::blob_store::blob_store(std::string root) : store_{std::move(root)} {}
rwf::blob_store::~blob_store() = default;
rwf::blob_store::blob_store(rwf::blob_store &&other) noexcept = default;
auto rwf::blob_store::operator=(rwf::blob_store &&other) noexcept
    -> rwf::blob_store & = default;

auto rwf::blob_store::root() const -> const std::string & {
    return store_.root();
}

auto rwf::blob_store::valid_key(std::string_view key) -> bool {
    if (key.empty() || key.size() > 96 || key.front() == '.') {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char value) {
        return (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '_' || value == '-' ||
               value == '.';
    });
}

auto rwf::blob_store::valid_kind(std::string_view kind) -> bool {
    return kind.size() <= 64 && valid_key(kind);
}

auto rwf::blob_store::write(std::string_view kind, std::string_view key,
                            std::span<const std::byte> payload, bool durable)
    -> status {
    if (!valid_kind(kind) || !valid_key(key)) {
        return failing(error_code::invalid_argument,
                       fmt::format("invalid blob key '{}/{}'", kind, key));
    }
    return store_.write(fmt::format("{}/{}", kind, key), payload,
                        object_media::binary, durable);
}

auto rwf::blob_store::read(std::string_view kind, std::string_view key,
                           std::size_t max_bytes)
    -> result<std::vector<std::byte>> {
    if (!valid_kind(kind) || !valid_key(key)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("invalid blob key '{}/{}'", kind, key))};
    }
    return store_.read(fmt::format("{}/{}", kind, key), max_bytes);
}

auto rwf::blob_store::erase(std::string_view kind, std::string_view key)
    -> status {
    if (!valid_kind(kind) || !valid_key(key)) {
        return failing(error_code::invalid_argument,
                       fmt::format("invalid blob key '{}/{}'", kind, key));
    }
    return store_.erase(fmt::format("{}/{}", kind, key));
}

auto rwf::blob_store::exists(std::string_view kind, std::string_view key) const
    -> bool {
    if (!valid_kind(kind) || !valid_key(key)) {
        return false;
    }
    return store_.exists(fmt::format("{}/{}", kind, key));
}

auto rwf::blob_store::list(std::string_view kind) const
    -> result<std::vector<std::string>> {
    if (!valid_kind(kind)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("invalid blob kind '{}'", kind))};
    }
    auto paths{store_.list(fmt::format("{}/", kind))};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    std::vector<std::string> keys;
    const auto prefix{fmt::format("{}/", kind)};
    for (auto &path : *paths) {
        if (!path.starts_with(prefix)) {
            continue;
        }
        auto key{path.substr(prefix.size())};
        if (key.find('/') == std::string::npos && valid_key(key)) {
            keys.push_back(std::move(key));
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

auto rwf::blob_store::commit() -> status { return store_.commit(); }

auto rwf::blob_store::compact() -> result<object_store_stats> {
    return store_.compact();
}

} // namespace refrain::world
