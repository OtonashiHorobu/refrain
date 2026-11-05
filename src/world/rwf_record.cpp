// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "rwf_internal.hpp"

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/platform/file.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::world::rwf {
namespace {

constexpr std::size_t record_file_overhead = record_file_header_bytes + 8;

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

auto read_record_file(std::string_view path, std::size_t max_bytes)
    -> result<std::vector<std::byte>> {
    auto bytes{core::io::read_file(
        path, {.max_bytes = record_file_header_bytes + max_record_bytes + 8,
               .allow_symlink = false})};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    if (bytes->size() < record_file_header_bytes) {
        return parse_failure(
            fmt::format("record file '{}' is {} bytes, shorter than the header",
                        path, bytes->size()));
    }
    const auto header{detail::decode_record_file_header(
        std::span<const std::byte>{*bytes}.subspan(0,
                                                   record_file_header_bytes))};
    if (!header) {
        return std::unexpected{header.error()};
    }
    if (header->uncompressed_size > max_bytes) {
        return parse_failure(
            fmt::format("record file '{}' declares {} payload bytes (limit {})",
                        path, header->uncompressed_size, max_bytes));
    }
    if (bytes->size() < record_file_header_bytes + header->stored_size) {
        return parse_failure(
            fmt::format("record file '{}' is truncated", path));
    }
    if (bytes->size() > record_file_header_bytes + header->stored_size + 7) {
        return parse_failure(fmt::format(
            "record file '{}' has {} trailing bytes", path,
            bytes->size() - record_file_header_bytes - header->stored_size));
    }
    const auto stored{std::span<const std::byte>{*bytes}.subspan(
        record_file_header_bytes, header->stored_size)};
    if (data::crc32c(stored) != detail::get_u32(*bytes, 12)) {
        return parse_failure(fmt::format("record file '{}' payload checksum "
                                         "mismatch",
                                         path));
    }
    return detail::decompress(header->codec, stored, header->uncompressed_size);
}

auto write_record_file(std::string_view path,
                       std::span<const std::byte> payload, compression codec,
                       int zstd_level, bool durable) -> status {
    if (payload.size() > max_record_bytes) {
        return failing(
            error_code::invalid_argument,
            fmt::format("record payload of {} bytes exceeds the {} byte limit",
                        payload.size(), max_record_bytes));
    }
    auto stored{detail::compress(codec, zstd_level, payload)};
    if (!stored) {
        return std::unexpected{stored.error()};
    }
    if (codec == compression::zstd && stored->size() >= payload.size()) {
        codec = compression::none;
        stored->assign(payload.begin(), payload.end());
    }
    auto header{detail::encode_record_file_header(
        static_cast<std::uint32_t>(stored->size()),
        static_cast<std::uint32_t>(payload.size()), codec)};
    detail::put_u32(header, 12, data::crc32c(*stored));
    detail::put_u32(header, 24, data::crc32c(header));
    std::vector<std::byte> file;
    file.reserve(record_file_overhead + stored->size());
    file.insert(file.end(), header.begin(), header.end());
    file.insert(file.end(), stored->begin(), stored->end());
    file.resize(static_cast<std::size_t>(
        detail::align_up(file.size(), std::uint64_t{8})));
    if (durable) {
        return platform::file::atomic_replace(path, file);
    }
    return core::io::write_file(path, file);
}

} // namespace refrain::world::rwf
