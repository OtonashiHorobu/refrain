// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/content_segment.hpp>

#include <refrain/core/bytes.hpp>
#include <refrain/core/hex.hpp>
#include <refrain/core/io.hpp>
#include <refrain/platform/mmap.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

using core::bytes::align_up;
using core::bytes::read_u16;
using core::bytes::read_u32;
using core::bytes::read_u64;
using core::hex::hex_u64;

// ---------------------------------------------------------------------------
// RFSG v1 layout (all integers little-endian).
// ---------------------------------------------------------------------------

constexpr std::uint16_t segment_format_version{1};
constexpr std::size_t segment_magic_bytes{4};
constexpr std::string_view segment_magic{"RFSG"};
constexpr std::size_t segment_id_offset{8};
constexpr std::size_t segment_blob_count_offset{16};
constexpr std::size_t segment_reserved32_offset{20};
constexpr std::size_t segment_data_bytes_offset{24};
constexpr std::size_t segment_body_hash_offset{32};
constexpr std::size_t segment_crc32c_offset{64};
constexpr std::size_t segment_crc32c_bytes{64};
constexpr std::size_t segment_reserved_tail_offset{68};
constexpr std::size_t segment_reserved_tail_bytes{12};
constexpr std::size_t segment_body_offset{content_segment_header_bytes};

/// Segments at or above this size are mapped read-only instead of streamed
/// (the 32 MiB segment target means this is the normal case). Smaller
/// segments keep the ifstream path.
constexpr std::uint64_t segment_mmap_threshold_bytes{1024U * 1024U};

[[nodiscard]] auto invalid_argument_error(std::string message) -> error {
    return make_error(error_code::invalid_argument, std::move(message));
}

[[nodiscard]] auto parse_error(std::string message) -> error {
    return make_error(error_code::parse_error, std::move(message));
}

[[nodiscard]] auto version_error(std::string message) -> error {
    return make_error(error_code::version_mismatch, std::move(message));
}

[[nodiscard]] auto io_error(std::string message) -> error {
    return make_error(error_code::io_error, std::move(message));
}

/// UTF-8 -> std::filesystem::path through u8string, matching core::io (the
/// narrow `path` constructor would use the ANSI code page on Windows).
[[nodiscard]] auto to_path(std::string_view utf8_path)
    -> std::filesystem::path {
    std::u8string text;
    text.reserve(utf8_path.size());
    for (const char character : utf8_path) {
        text.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path{text};
}

[[nodiscard]] auto join_path(std::string_view base, std::string_view child)
    -> std::string {
    std::string joined{base};
    if (joined.empty() || joined.back() != '/') {
        joined.push_back('/');
    }
    joined.append(child);
    return joined;
}

constexpr auto write_u16 = core::bytes::write_le<std::uint16_t>;

constexpr auto write_u32 = core::bytes::write_le<std::uint32_t>;

constexpr auto write_u64 = core::bytes::write_le<std::uint64_t>;

[[nodiscard]] auto bytes_equal(std::span<const std::byte> bytes,
                               std::size_t offset,
                               std::string_view expected) noexcept -> bool {
    if (offset > bytes.size() || expected.size() > bytes.size() - offset) {
        return false;
    }
    for (std::size_t index{0}; index < expected.size(); ++index) {
        if (std::to_integer<char>(bytes[offset + index]) != expected[index]) {
            return false;
        }
    }
    return true;
}

/// First 8 body-hash bytes interpreted little-endian; the header stores both
/// the hash and its derived id, so the parser can cross-check them without the
/// body.
[[nodiscard]] auto segment_id_from_hash(const digest32 &hash) noexcept
    -> std::uint64_t {
    std::uint64_t id{0};
    for (std::size_t index{0}; index < 8U; ++index) {
        id |= static_cast<std::uint64_t>(
                  std::to_integer<unsigned int>(hash[index]))
              << (8U * index);
    }
    return id;
}

/// Segment file name: 16 lowercase hex digits plus the file extension.
[[nodiscard]] auto segment_file_name(std::uint64_t id) -> std::string {
    std::string name{hex_u64(id)};
    name.append(content_segment_extension);
    return name;
}

[[nodiscard]] auto segment_body(std::span<const std::byte> bytes) noexcept
    -> std::span<const std::byte> {
    return bytes.subspan(segment_body_offset);
}

/// Parse the fixed 80-byte header prefix. The caller is responsible for the
/// `bytes.size() >= 80` precondition (checked here as well) and for the
/// `data_bytes == bytes.size() - 80` relation.
[[nodiscard]] auto parse_header_prefix(std::span<const std::byte> bytes)
    -> result<segment_header> {
    if (bytes.size() < content_segment_header_bytes) {
        return std::unexpected{
            parse_error("segment is truncated before the 80-byte header")};
    }
    if (!bytes_equal(bytes, 0, segment_magic)) {
        return std::unexpected{parse_error("segment has a bad RFSG magic")};
    }
    if (read_u16(bytes, 4) != segment_format_version) {
        return std::unexpected{version_error(
            "segment has an unsupported RFSG format version (expected " +
            std::to_string(segment_format_version) + ")")};
    }
    if (read_u16(bytes, 6) != 0U) {
        return std::unexpected{
            parse_error("segment has non-zero header flags")};
    }
    if (read_u32(bytes, segment_reserved32_offset) != 0U) {
        return std::unexpected{
            parse_error("segment has non-zero reserved header bytes")};
    }
    for (std::size_t index{0}; index < segment_reserved_tail_bytes; ++index) {
        if (bytes[segment_reserved_tail_offset + index] != std::byte{0}) {
            return std::unexpected{
                parse_error("segment has non-zero reserved header bytes")};
        }
    }
    if (read_u32(bytes, segment_crc32c_offset) !=
        crc32c(bytes.first(segment_crc32c_bytes))) {
        return std::unexpected{
            parse_error("segment header failed the CRC32C check")};
    }

    segment_header header;
    header.id = read_u64(bytes, segment_id_offset);
    header.blob_count = read_u32(bytes, segment_blob_count_offset);
    header.data_bytes = read_u64(bytes, segment_data_bytes_offset);
    std::copy_n(bytes.begin() +
                    static_cast<std::ptrdiff_t>(segment_body_hash_offset),
                header.body_hash.size(), header.body_hash.begin());

    if (header.blob_count == 0U) {
        return std::unexpected{
            parse_error("segment header declares no blobs (empty segments are "
                        "never published)")};
    }
    if (header.id != segment_id_from_hash(header.body_hash)) {
        return std::unexpected{
            parse_error("segment id does not match the body hash prefix")};
    }
    if (header.data_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{
            parse_error("segment body exceeds the 32-bit offset range")};
    }
    return header;
}

[[nodiscard]] auto write_all(std::ostream &out,
                             std::span<const std::byte> bytes) -> bool {
    if (bytes.empty()) {
        return static_cast<bool>(out);
    }
    out.write(reinterpret_cast<const char *>(bytes.data()), // NOLINT
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

/// Remove the staging file, ignoring failures: an orphaned `.part` is inert
/// (never read as a segment) and the destructor must not fail. The `noexcept`
/// is kept for the destructor call, but `to_path()` allocates: a hard OOM is
/// contained rather than terminating.
void remove_part(const std::string &path) noexcept {
    try {
        std::error_code error;
        std::filesystem::remove(to_path(path), error);
    } catch (...) {
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Header / verification.
// ---------------------------------------------------------------------------

auto read_segment_header(std::span<const std::byte> bytes)
    -> result<segment_header> {
    auto header{parse_header_prefix(bytes)};
    if (!header) {
        return std::unexpected{std::move(header.error())};
    }
    if (header->data_bytes !=
        static_cast<std::uint64_t>(bytes.size() - segment_body_offset)) {
        return std::unexpected{
            parse_error("segment data_bytes does not match its body length")};
    }
    return header;
}

auto verify_segment(std::span<const std::byte> bytes,
                    std::span<const segment_blob_span> blobs) -> status {
    auto header{read_segment_header(bytes)};
    if (!header) {
        return std::unexpected{std::move(header.error())};
    }
    const auto body{segment_body(bytes)};
    if (blake3(body) != header->body_hash) {
        return std::unexpected{
            parse_error("segment body failed the BLAKE3 check")};
    }
    if (blobs.empty()) {
        return status_ok();
    }
    if (static_cast<std::uint64_t>(blobs.size()) !=
        static_cast<std::uint64_t>(header->blob_count)) {
        return std::unexpected{
            parse_error("segment blob-span count does not match the header")};
    }
    std::uint64_t previous_end{0};
    bool first{true};
    for (const auto &blob : blobs) {
        if (blob.offset % content_segment_alignment != 0U) {
            return std::unexpected{
                parse_error("segment blob is not 8-byte aligned")};
        }
        if (blob.offset > header->data_bytes ||
            static_cast<std::uint64_t>(blob.size) >
                header->data_bytes - static_cast<std::uint64_t>(blob.offset)) {
            return std::unexpected{
                parse_error("segment blob is outside the body")};
        }
        if (!first && static_cast<std::uint64_t>(blob.offset) < previous_end) {
            return std::unexpected{
                parse_error("segment blobs overlap or are out of order")};
        }
        previous_end = static_cast<std::uint64_t>(blob.offset) +
                       static_cast<std::uint64_t>(blob.size);
        first = false;
    }
    return status_ok();
}

auto segment_path(std::string_view store_root, std::uint64_t id)
    -> std::string {
    std::string base{store_root};
    while (base.size() > 1U && base.back() == '/') {
        base.pop_back();
    }
    const auto relative{
        join_path(content_segment_directory, segment_file_name(id))};
    if (base.empty()) {
        return relative;
    }
    return join_path(base, relative);
}

// ---------------------------------------------------------------------------
// Writer.
// ---------------------------------------------------------------------------

struct segment_writer::impl {
    std::string segments_dir{};
    std::string part_path{};
    std::uint32_t target_bytes{0};
    std::uint32_t blob_count{0};
    std::uint64_t data_bytes{0};
    std::ofstream out{};
    bool sealed{false};
    bool failed{false};
};

segment_writer::segment_writer() noexcept = default;

segment_writer::segment_writer(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

segment_writer::segment_writer(segment_writer &&other) noexcept = default;

auto segment_writer::operator=(segment_writer &&other) noexcept
    -> segment_writer & = default;

segment_writer::~segment_writer() {
    if (impl_ && !impl_->sealed) {
        if (impl_->out.is_open()) {
            impl_->out.close();
        }
        remove_part(impl_->part_path);
    }
}

auto segment_writer::create(std::string_view segments_dir,
                            std::uint32_t target_bytes)
    -> result<segment_writer> {
    if (segments_dir.empty()) {
        return std::unexpected{invalid_argument_error(
            "segment writer needs a non-empty segments directory")};
    }
    if (target_bytes == 0U) {
        return std::unexpected{invalid_argument_error(
            "segment writer needs a non-zero target size")};
    }
    const std::string directory{segments_dir};
    if (auto created{core::io::create_directories(directory)}; !created) {
        return std::unexpected{std::move(created.error())};
    }

    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp{static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    const auto index{sequence.fetch_add(1, std::memory_order_relaxed)};
    const auto part_path{join_path(directory, ".tmp-" + std::to_string(stamp) +
                                                  "-" + std::to_string(index) +
                                                  ".part")};

    auto state{std::make_unique<impl>()};
    state->segments_dir = directory;
    state->part_path = part_path;
    state->target_bytes = target_bytes;
    state->out.open(to_path(part_path),
                    std::ios::binary | std::ios::out | std::ios::trunc);
    if (!state->out) {
        return std::unexpected{io_error(
            "failed to create segment staging file '" + part_path + "'")};
    }
    // Placeholder header; the real fields are written by seal().
    const std::array<std::byte, content_segment_header_bytes> placeholder{};
    if (!write_all(state->out, placeholder)) {
        state->out.close();
        remove_part(part_path);
        return std::unexpected{io_error(
            "failed to write the segment staging header '" + part_path + "'")};
    }
    state->out.flush();
    if (!state->out) {
        state->out.close();
        remove_part(part_path);
        return std::unexpected{io_error(
            "failed to flush the segment staging header '" + part_path + "'")};
    }
    return segment_writer{std::move(state)};
}

auto segment_writer::valid() const noexcept -> bool {
    return impl_ != nullptr && !impl_->sealed && !impl_->failed;
}

auto segment_writer::blob_count() const noexcept -> std::uint32_t {
    return impl_ ? impl_->blob_count : 0U;
}

auto segment_writer::data_bytes() const noexcept -> std::uint64_t {
    return impl_ ? impl_->data_bytes : 0U;
}

auto segment_writer::target_bytes() const noexcept -> std::uint32_t {
    return impl_ ? impl_->target_bytes : 0U;
}

auto segment_writer::path() const noexcept -> std::string_view {
    return impl_ ? std::string_view{impl_->part_path} : std::string_view{};
}

auto segment_writer::append(std::span<const std::byte> bytes)
    -> result<std::uint32_t> {
    if (!impl_) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "segment writer is not initialized")};
    }
    if (impl_->sealed) {
        return std::unexpected{
            invalid_argument_error("segment writer is already sealed")};
    }
    if (impl_->failed) {
        return std::unexpected{
            io_error("segment writer failed earlier and cannot append")};
    }

    const auto offset{static_cast<std::uint64_t>(
        align_up(impl_->data_bytes, content_segment_alignment))};
    if (offset > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(bytes.size()) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max()) -
                offset) {
        impl_->failed = true;
        return std::unexpected{invalid_argument_error(
            "segment body exceeds the 32-bit offset range")};
    }

    constexpr std::array<std::byte, content_segment_alignment - 1U> zeros{};
    const auto padding{offset - impl_->data_bytes};
    if (!write_all(impl_->out, std::span<const std::byte>{zeros}.first(
                                   static_cast<std::size_t>(padding))) ||
        !write_all(impl_->out, bytes)) {
        impl_->failed = true;
        return std::unexpected{
            io_error("failed to append to the segment staging file '" +
                     impl_->part_path + "'")};
    }
    impl_->out.flush();
    if (!impl_->out) {
        impl_->failed = true;
        return std::unexpected{
            io_error("failed to flush the segment staging file '" +
                     impl_->part_path + "'")};
    }
    impl_->data_bytes = offset + static_cast<std::uint64_t>(bytes.size());
    ++impl_->blob_count;
    return static_cast<std::uint32_t>(offset);
}

auto segment_writer::seal() -> result<segment_header> {
    if (!impl_) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "segment writer is not initialized")};
    }
    if (impl_->sealed) {
        return std::unexpected{
            invalid_argument_error("segment writer is already sealed")};
    }
    if (impl_->failed) {
        return std::unexpected{
            io_error("segment writer failed earlier and cannot be sealed")};
    }
    if (impl_->blob_count == 0U) {
        impl_->failed = true;
        if (impl_->out.is_open()) {
            impl_->out.close();
        }
        remove_part(impl_->part_path);
        return std::unexpected{invalid_argument_error(
            "cannot seal a segment with no appended blobs")};
    }

    impl_->out.flush();
    impl_->out.close();
    if (!impl_->out) {
        impl_->failed = true;
        remove_part(impl_->part_path);
        return std::unexpected{
            io_error("failed to flush the segment staging file '" +
                     impl_->part_path + "'")};
    }

    auto staging{core::io::read_file(impl_->part_path)};
    if (!staging) {
        impl_->failed = true;
        remove_part(impl_->part_path);
        return std::unexpected{std::move(staging.error())};
    }
    if (staging->size() != content_segment_header_bytes +
                               static_cast<std::size_t>(impl_->data_bytes)) {
        impl_->failed = true;
        remove_part(impl_->part_path);
        return std::unexpected{io_error("segment staging file '" +
                                        impl_->part_path +
                                        "' has an unexpected size")};
    }

    segment_header header;
    header.blob_count = impl_->blob_count;
    header.data_bytes = impl_->data_bytes;
    header.body_hash = blake3(segment_body(*staging));
    header.id = segment_id_from_hash(header.body_hash);

    auto file{std::move(*staging)};
    const auto view{std::span<std::byte>{file}};
    for (std::size_t index{0}; index < segment_magic_bytes; ++index) {
        view[index] = static_cast<std::byte>(segment_magic[index]);
    }
    write_u16(view, 4, segment_format_version);
    write_u16(view, 6, 0);
    write_u64(view, segment_id_offset, header.id);
    write_u32(view, segment_blob_count_offset, header.blob_count);
    write_u32(view, segment_reserved32_offset, 0);
    write_u64(view, segment_data_bytes_offset, header.data_bytes);
    std::copy(header.body_hash.begin(), header.body_hash.end(),
              view.begin() +
                  static_cast<std::ptrdiff_t>(segment_body_hash_offset));
    write_u32(
        view, segment_crc32c_offset,
        crc32c(std::span<const std::byte>{view}.first(segment_crc32c_bytes)));

    const auto final_path{
        join_path(impl_->segments_dir, segment_file_name(header.id))};
    if (core::io::exists(final_path)) {
        // Content-addressed: the same body was sealed before. Reuse it only if
        // the recorded header agrees; otherwise the 64-bit id collided with a
        // different segment (or the store is damaged) and we fail closed.
        auto existing{core::io::read_file(final_path)};
        if (!existing) {
            impl_->failed = true;
            remove_part(impl_->part_path);
            return std::unexpected{std::move(existing.error())};
        }
        auto existing_header{read_segment_header(*existing)};
        if (!existing_header) {
            impl_->failed = true;
            remove_part(impl_->part_path);
            return std::unexpected{parse_error(
                "existing segment '" + final_path +
                "' is invalid: " + existing_header.error().message)};
        }
        if (existing_header->blob_count != header.blob_count ||
            existing_header->data_bytes != header.data_bytes ||
            existing_header->body_hash != header.body_hash) {
            impl_->failed = true;
            remove_part(impl_->part_path);
            return std::unexpected{
                parse_error("segment id collision: '" + final_path +
                            "' records a different blob count/length/body")};
        }
        impl_->sealed = true;
        remove_part(impl_->part_path);
        return header;
    }

    if (auto published{core::io::atomic_write_file(final_path, file)};
        !published) {
        impl_->failed = true;
        remove_part(impl_->part_path);
        return std::unexpected{std::move(published.error())};
    }
    impl_->sealed = true;
    remove_part(impl_->part_path);
    return header;
}

// ---------------------------------------------------------------------------
// Reader.
// ---------------------------------------------------------------------------

struct segment_reader::impl {
    std::string path{};
    segment_header header{};
    std::uint64_t file_size{0};
    platform::mapped_file mapping{};
    mutable std::ifstream in{};
};

segment_reader::segment_reader() noexcept = default;

segment_reader::segment_reader(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

segment_reader::segment_reader(segment_reader &&other) noexcept = default;

auto segment_reader::operator=(segment_reader &&other) noexcept
    -> segment_reader & = default;

segment_reader::~segment_reader() = default;

auto segment_reader::open(std::string_view path) -> result<segment_reader> {
    if (path.empty()) {
        return std::unexpected{
            invalid_argument_error("segment reader needs a non-empty path")};
    }
    const std::string location{path};
    auto size{core::io::file_size(location)};
    if (!size) {
        return std::unexpected{std::move(size.error())};
    }
    if (*size < content_segment_header_bytes) {
        return std::unexpected{
            parse_error("segment '" + location +
                        "' is truncated before the 80-byte header")};
    }

    auto state{std::make_unique<impl>()};
    state->path = location;
    state->file_size = *size;

    std::array<std::byte, content_segment_header_bytes> prefix{};
    if (*size >= segment_mmap_threshold_bytes) {
        auto mapped{platform::mapped_file::open(location)};
        if (mapped) {
            if (mapped->size() < content_segment_header_bytes) {
                return std::unexpected{
                    parse_error("segment '" + location +
                                "' is truncated before the 80-byte header")};
            }
            state->mapping = std::move(*mapped);
            state->file_size = state->mapping.size();
            std::copy_n(state->mapping.data().begin(),
                        content_segment_header_bytes, prefix.begin());
        }
        // Mapping failure falls back to the ifstream path below.
    }
    if (!state->mapping.valid()) {
        state->in.open(to_path(location), std::ios::binary | std::ios::in);
        if (!state->in) {
            return std::unexpected{
                io_error("failed to open segment '" + location + "'")};
        }
        state->in.read(reinterpret_cast<char *>(prefix.data()), // NOLINT
                       static_cast<std::streamsize>(prefix.size()));
        if (state->in.gcount() != static_cast<std::streamsize>(prefix.size())) {
            return std::unexpected{io_error(
                "failed to read the segment header of '" + location + "'")};
        }
    }
    auto header{parse_header_prefix(prefix)};
    if (!header) {
        return std::unexpected{std::move(header.error())};
    }
    if (header->data_bytes != state->file_size - content_segment_header_bytes) {
        return std::unexpected{
            parse_error("segment '" + location +
                        "': data_bytes does not match the file size")};
    }
    state->header = *header;
    return segment_reader{std::move(state)};
}

auto segment_reader::valid() const noexcept -> bool { return impl_ != nullptr; }

auto segment_reader::mapped() const noexcept -> bool {
    return impl_ != nullptr && impl_->mapping.valid();
}

auto segment_reader::header() const noexcept -> const segment_header & {
    static const segment_header empty{};
    return impl_ ? impl_->header : empty;
}

auto segment_reader::path() const noexcept -> std::string_view {
    return impl_ ? std::string_view{impl_->path} : std::string_view{};
}

auto segment_reader::read_at(std::uint32_t offset, std::size_t size) const
    -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "segment reader is not initialized")};
    }
    const auto offset64{static_cast<std::uint64_t>(offset)};
    if (offset64 > impl_->header.data_bytes ||
        static_cast<std::uint64_t>(size) >
            impl_->header.data_bytes - offset64) {
        return std::unexpected{invalid_argument_error(
            "segment read_at is outside the body (offset " +
            std::to_string(offset) + ", length " + std::to_string(size) +
            ", body size " + std::to_string(impl_->header.data_bytes) + ")")};
    }
    std::vector<std::byte> bytes(size);
    if (size == 0U) {
        return bytes;
    }
    if (impl_->mapping.valid()) {
        const auto body{
            impl_->mapping.data().subspan(content_segment_header_bytes)};
        std::copy_n(body.begin() + static_cast<std::ptrdiff_t>(offset64), size,
                    bytes.begin());
        return bytes;
    }
    impl_->in.clear();
    impl_->in.seekg(
        static_cast<std::streamoff>(content_segment_header_bytes + offset64));
    if (!impl_->in) {
        return std::unexpected{
            io_error("failed to seek in segment '" + impl_->path + "'")};
    }
    impl_->in.read(reinterpret_cast<char *>(bytes.data()), // NOLINT
                   static_cast<std::streamsize>(bytes.size()));
    if (impl_->in.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::unexpected{
            io_error("failed to read from segment '" + impl_->path + "'")};
    }
    return bytes;
}

auto segment_reader::verify(std::span<const segment_blob_span> blobs) const
    -> status {
    if (!impl_) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "segment reader is not initialized")};
    }
    if (impl_->mapping.valid()) {
        auto verified{verify_segment(impl_->mapping.data(), blobs)};
        if (!verified) {
            verified.error().message =
                "segment '" + impl_->path + "': " + verified.error().message;
            return verified;
        }
        return status_ok();
    }
    auto bytes{core::io::read_file(impl_->path)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    auto verified{verify_segment(*bytes, blobs)};
    if (!verified) {
        verified.error().message =
            "segment '" + impl_->path + "': " + verified.error().message;
        return verified;
    }
    return status_ok();
}

} // namespace refrain::data
