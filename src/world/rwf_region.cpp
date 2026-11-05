// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "rwf_internal.hpp"

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/platform/random_access_file.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::world::rwf {
namespace {

using detail::align_up;

constexpr std::array<std::size_t, 2> slot_offsets{index_a_offset,
                                                  index_b_offset};

[[nodiscard]] auto corrupt(std::string message) -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
}

[[nodiscard]] auto failing(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

/// Storage paths must be regular files: a symlinked region would let a crafted
/// world redirect reads/writes outside its root. `open_options::no_follow`
/// covers the final component on POSIX too, but checking first yields a clear
/// `permission_denied` and also protects the Windows path.
[[nodiscard]] auto reject_symlink(std::string_view path) -> status {
    if (core::io::is_symlink(path)) {
        return failing(error_code::permission_denied,
                       fmt::format("refusing to follow symlink '{}'", path));
    }
    return status_ok();
}

[[nodiscard]] auto entry_end(const region_entry &entry) -> std::uint64_t {
    return entry.offset + align_up(entry.length, 8);
}

[[nodiscard]] auto read_bytes(platform::file::random_access_file &file,
                              std::uint64_t offset, std::size_t size)
    -> result<std::vector<std::byte>> {
    std::vector<std::byte> bytes(size);
    if (auto read{file.read_at(offset, bytes)}; !read) {
        return std::unexpected{read.error()};
    }
    return bytes;
}

/// Validate an index entry against the file size.
[[nodiscard]] auto entry_problem(const region_entry &entry,
                                 std::uint64_t file_size) -> std::string {
    if (entry.offset < data_offset) {
        return fmt::format("entry offset {} is inside the header",
                           entry.offset);
    }
    if (entry.offset % 8 != 0) {
        return fmt::format("entry offset {} is not 8-byte aligned",
                           entry.offset);
    }
    if (entry.length < record_header_bytes) {
        return fmt::format("entry length {} is shorter than a record header",
                           entry.length);
    }
    if (entry.length > max_record_bytes + record_header_bytes) {
        return fmt::format("entry length {} exceeds the record limit",
                           entry.length);
    }
    if (entry.uncompressed > max_record_bytes) {
        return fmt::format("entry declares {} payload bytes (limit {})",
                           entry.uncompressed, max_record_bytes);
    }
    // Subtraction form avoids `offset + length` wrapping for hostile indexes.
    if (entry.offset > file_size || entry.length > file_size - entry.offset) {
        return fmt::format("entry [{}, {}) exceeds the {} byte file",
                           entry.offset, entry.offset + entry.length,
                           file_size);
    }
    return {};
}

} // namespace

struct region_file::impl {
    platform::file::random_access_file file;
    region_options options;
    std::array<region_entry, index_slots> entries{};
    std::uint64_t generation{1};
    std::uint64_t data_tail{data_offset};
    std::uint64_t record_count{0};
    std::uint32_t active_slot{0};
    bool dirty{false};

    [[nodiscard]] auto write_empty_slot(std::size_t slot_index,
                                        std::uint64_t slot_generation)
        -> status {
        detail::index_slot slot{};
        slot.generation = slot_generation;
        const auto bytes{detail::encode_index_slot(slot)};
        return file.write_at(slot_offsets[slot_index], bytes);
    }

    [[nodiscard]] auto initialize_new() -> status {
        // Slot A is the first valid index; slot B stays at generation 0 so a
        // crash before the header write still leaves one readable slot.
        if (auto status{write_empty_slot(1, 0)}; !status) {
            return status;
        }
        if (auto status{write_empty_slot(0, 1)}; !status) {
            return status;
        }
        detail::file_header header{};
        header.x = options.x;
        header.z = options.z;
        header.active_slot = 0;
        header.generation = 1;
        header.data_tail = data_offset;
        header.record_count = 0;
        header.dictionary_id =
            options.dictionary != nullptr ? options.dictionary->id : 0;
        const auto bytes{detail::encode_file_header(header)};
        if (auto status{file.write_at(0, bytes)}; !status) {
            return status;
        }
        return file.sync();
    }

    [[nodiscard]] auto load_slot(std::size_t slot_index)
        -> result<detail::index_slot> {
        auto bytes{
            read_bytes(file, slot_offsets[slot_index], index_slot_bytes)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        return detail::decode_index_slot(*bytes);
    }

    [[nodiscard]] auto load() -> status {
        const auto size{file.size()};
        if (!size) {
            return std::unexpected{size.error()};
        }
        if (*size < data_offset) {
            return corrupt(fmt::format("region file has {} bytes, shorter than "
                                       "the {} byte header",
                                       *size, data_offset));
        }
        if (*size > max_region_file_bytes) {
            return corrupt(fmt::format("region file has {} bytes (limit {})",
                                       *size, max_region_file_bytes));
        }
        auto header_bytes{read_bytes(file, 0, 4096)};
        if (!header_bytes) {
            return std::unexpected{header_bytes.error()};
        }
        auto header{detail::decode_file_header(*header_bytes)};
        if (!header) {
            return std::unexpected{header.error()};
        }
        if (header->dictionary_id != 0 &&
            (options.dictionary == nullptr ||
             options.dictionary->id != header->dictionary_id)) {
            return corrupt(fmt::format(
                "region requires dictionary {:016x} but it was not supplied",
                header->dictionary_id));
        }
        if (header->x != options.x || header->z != options.z) {
            return corrupt(fmt::format("region file contains region "
                                       "({}, {}), expected ({}, {})",
                                       header->x, header->z, options.x,
                                       options.z));
        }
        auto slot_a{load_slot(0)};
        auto slot_b{load_slot(1)};
        const detail::index_slot *chosen{nullptr};
        std::uint32_t chosen_index{header->active_slot};
        if (slot_a && (!slot_b || slot_a->generation >= slot_b->generation)) {
            chosen = &*slot_a;
            chosen_index = 0;
        } else if (slot_b) {
            chosen = &*slot_b;
            chosen_index = 1;
        }
        if (chosen == nullptr) {
            return corrupt("region file has no readable index slot "
                           "(run world fsck --repair)");
        }
        for (std::size_t index{0}; index < index_slots; ++index) {
            const auto &entry{chosen->entries[index]};
            if (entry.empty()) {
                continue;
            }
            if (auto problem{entry_problem(entry, *size)}; !problem.empty()) {
                return corrupt(
                    fmt::format("region index entry {}: {}", index, problem));
            }
        }
        entries = chosen->entries;
        active_slot = chosen_index;
        generation = std::max(header->generation, chosen->generation);
        data_tail = std::max(header->data_tail, std::uint64_t{data_offset});
        record_count = 0;
        for (const auto &entry : entries) {
            if (entry.empty()) {
                continue;
            }
            ++record_count;
            data_tail = std::max(data_tail, entry_end(entry));
        }
        dirty = false;
        return status_ok();
    }

    [[nodiscard]] auto append_stored(std::uint32_t slot,
                                     std::span<const std::byte> frame,
                                     std::size_t uncompressed_size,
                                     region_entry metadata) -> status {
        std::vector<std::byte> padded{frame.begin(), frame.end()};
        padded.resize(static_cast<std::size_t>(align_up(padded.size(), 8)));
        const auto offset{data_tail};
        if (auto written{file.write_at(offset, padded)}; !written) {
            return written;
        }
        metadata.offset = offset;
        metadata.length = static_cast<std::uint32_t>(frame.size());
        metadata.crc32c = data::crc32c(frame);
        metadata.uncompressed = static_cast<std::uint32_t>(uncompressed_size);
        metadata.generation = generation + 1;
        entries[slot] = metadata;
        data_tail = offset + padded.size();
        ++record_count;
        dirty = true;
        return status_ok();
    }

    [[nodiscard]] auto commit() -> status {
        if (!dirty) {
            return status_ok();
        }
        // Publish the index only after the appended records are durable: one
        // fsync does not order data pages against the index-slot page, so a
        // crash during the slot write could otherwise leave a valid slot that
        // points at never-written bytes.
        if (auto synced{file.sync()}; !synced) {
            return synced;
        }
        detail::index_slot slot{};
        slot.generation = generation + 1;
        slot.entries = entries;
        const auto slot_index{active_slot == 0 ? 1U : 0U};
        const auto bytes{detail::encode_index_slot(slot)};
        if (auto written{file.write_at(slot_offsets[slot_index], bytes)};
            !written) {
            return written;
        }
        if (auto synced{file.sync()}; !synced) {
            return synced;
        }
        detail::file_header header{};
        header.x = options.x;
        header.z = options.z;
        header.active_slot = slot_index;
        header.generation = generation + 1;
        header.data_tail = data_tail;
        header.record_count = record_count;
        header.dictionary_id =
            options.dictionary != nullptr ? options.dictionary->id : 0;
        const auto header_bytes{detail::encode_file_header(header)};
        if (auto written{file.write_at(0, header_bytes)}; !written) {
            return written;
        }
        if (auto synced{file.sync()}; !synced) {
            return synced;
        }
        active_slot = slot_index;
        generation += 1;
        dirty = false;
        return status_ok();
    }

    [[nodiscard]] auto stats() -> result<region_stats> {
        const auto size{file.size()};
        if (!size) {
            return std::unexpected{size.error()};
        }
        region_stats out{};
        out.chunks = static_cast<std::size_t>(record_count);
        out.file_bytes = *size;
        out.data_tail = data_tail;
        out.generation = generation;
        for (const auto &entry : entries) {
            if (!entry.empty()) {
                out.live_bytes += align_up(entry.length, 8);
            }
        }
        const auto payload_area{
            data_tail > data_offset ? data_tail - data_offset : 0};
        out.garbage_bytes =
            payload_area > out.live_bytes ? payload_area - out.live_bytes : 0;
        return out;
    }
};

region_file::region_file() noexcept = default;
region_file::~region_file() {
    if (impl_ != nullptr && impl_->file.valid()) {
        static_cast<void>(impl_->commit());
    }
}
region_file::region_file(region_file &&other) noexcept = default;
auto region_file::operator=(region_file &&other) noexcept
    -> region_file & = default;
region_file::region_file(std::unique_ptr<impl> state)
    : impl_{std::move(state)} {}

auto region_file::create(std::string_view path, region_options options)
    -> result<region_file> {
    if (auto checked{reject_symlink(path)}; !checked) {
        return std::unexpected{checked.error()};
    }
    auto file{platform::file::random_access_file::open(
        path, platform::file::open_mode::read_write,
        {.create = true, .exclusive = true, .no_follow = true})};
    if (!file) {
        return std::unexpected{file.error()};
    }
    auto state{std::make_unique<impl>()};
    state->file = std::move(*file);
    state->options = options;
    if (auto status{state->initialize_new()}; !status) {
        return std::unexpected{status.error()};
    }
    if (auto synced{
            platform::file::sync_directory(core::io::parent_directory(path))};
        !synced) {
        return std::unexpected{synced.error()};
    }
    return region_file{std::move(state)};
}

auto region_file::load(std::string_view path, region_options options)
    -> result<region_file> {
    if (auto checked{reject_symlink(path)}; !checked) {
        return std::unexpected{checked.error()};
    }
    auto file{platform::file::random_access_file::open(
        path, platform::file::open_mode::read_write, {.no_follow = true})};
    if (!file && file.error().code == error_code::permission_denied) {
        // Read-only worlds (mounted media, chmod -w) must still be readable by
        // fsck/info/diff; writes will fail later with a clear error.
        file = platform::file::random_access_file::open(
            path, platform::file::open_mode::read_only, {.no_follow = true});
    }
    if (!file) {
        return std::unexpected{file.error()};
    }
    auto state{std::make_unique<impl>()};
    state->file = std::move(*file);
    state->options = options;
    if (auto status{state->load()}; !status) {
        return std::unexpected{status.error()};
    }
    return region_file{std::move(state)};
}

auto region_file::load_or_create(std::string_view path, region_options options)
    -> result<region_file> {
    auto opened{load(path, options)};
    if (opened) {
        return opened;
    }
    if (opened.error().code != error_code::not_found) {
        return opened;
    }
    return create(path, options);
}

auto region_file::valid() const noexcept -> bool {
    return impl_ != nullptr && impl_->file.valid();
}

auto region_file::path() const -> std::string_view {
    return impl_ != nullptr ? impl_->file.path() : std::string_view{};
}

auto region_file::slot_for(const chunk_pos &pos) -> std::uint32_t {
    const auto local_x{static_cast<std::uint32_t>(pos.x & (region_edge - 1))};
    const auto local_z{static_cast<std::uint32_t>(pos.z & (region_edge - 1))};
    return local_x + local_z * static_cast<std::uint32_t>(region_edge);
}

auto region_file::read(const chunk_pos &pos) -> result<std::vector<std::byte>> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "region handle is closed")};
    }
    const auto slot{slot_for(pos)};
    const auto &entry{impl_->entries[slot]};
    if (entry.empty()) {
        return std::unexpected{
            make_error(error_code::not_found,
                       fmt::format("chunk {} is not stored in region {}",
                                   format_chunk_pos(pos), path()))};
    }
    auto frame{read_bytes(impl_->file, entry.offset, entry.length)};
    if (!frame) {
        return std::unexpected{frame.error()};
    }
    if (data::crc32c(*frame) != entry.crc32c) {
        return corrupt(fmt::format("chunk {} record checksum mismatch",
                                   format_chunk_pos(pos)));
    }
    const auto header{detail::decode_frame_header(*frame)};
    if (!header) {
        return std::unexpected{header.error()};
    }
    if (!(header->pos == pos)) {
        return corrupt(fmt::format("region slot {} holds chunk {}, expected {}",
                                   slot, format_chunk_pos(header->pos),
                                   format_chunk_pos(pos)));
    }
    auto payload{detail::decode_frame(*frame, impl_->options.dictionary)};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    if (payload->size() != entry.uncompressed) {
        return corrupt(fmt::format(
            "chunk {} payload is {} bytes, index says {}",
            format_chunk_pos(pos), payload->size(), entry.uncompressed));
    }
    return payload;
}

auto region_file::write(const chunk_pos &pos, chunk_status state,
                        std::span<const std::byte> payload) -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "region handle is closed");
    }
    return write(pos, state, payload, impl_->options.zstd_level);
}

auto region_file::write(const chunk_pos &pos, chunk_status state,
                        std::span<const std::byte> payload, int zstd_level)
    -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "region handle is closed");
    }
    if (!is_valid_zstd_level(zstd_level)) {
        return failing(error_code::invalid_argument,
                       fmt::format("zstd level {} is outside [{}, {}]",
                                   zstd_level, min_zstd_level, max_zstd_level));
    }
    if (payload.size() > max_record_bytes) {
        return failing(
            error_code::invalid_argument,
            fmt::format(
                "chunk {} payload of {} bytes exceeds the {} byte limit",
                format_chunk_pos(pos), payload.size(), max_record_bytes));
    }
    const auto region_x{pos.x >> 5};
    const auto region_z{pos.z >> 5};
    if (region_x != impl_->options.x || region_z != impl_->options.z) {
        return failing(
            error_code::invalid_argument,
            fmt::format("chunk {} does not belong to region ({}, {})",
                        format_chunk_pos(pos), impl_->options.x,
                        impl_->options.z));
    }
    auto frame{detail::build_frame(pos, state, impl_->options.codec, zstd_level,
                                   payload, impl_->options.dictionary)};
    if (!frame) {
        return std::unexpected{frame.error()};
    }
    // The entry records the codec/build_frame actually chose (raw when zstd
    // did not shrink this level's output); decompression only needs that byte,
    // never the level used to produce it.
    region_entry metadata{};
    metadata.codec = static_cast<compression>(detail::get_u8(*frame, 17));
    metadata.flags = 0;
    return impl_->append_stored(slot_for(pos), *frame, payload.size(),
                                metadata);
}

auto region_file::contains(const chunk_pos &pos) const -> bool {
    return valid() && !impl_->entries[slot_for(pos)].empty();
}

auto region_file::commit() -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "region handle is closed");
    }
    return impl_->commit();
}

auto region_file::stats() -> result<region_stats> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "region handle is closed")};
    }
    return impl_->stats();
}

auto region_file::entries() const -> std::span<const region_entry> {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->entries;
}

auto region_file::close() -> status {
    if (impl_ == nullptr) {
        return status_ok();
    }
    auto committed{impl_->commit()};
    impl_->file = platform::file::random_access_file{};
    impl_.reset();
    return committed;
}

auto region_file::compact() -> result<region_stats> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "region handle is closed")};
    }
    const std::string target{fmt::format("{}.compact.tmp", path())};
    std::error_code ignored;
    std::filesystem::remove(std::filesystem::path{target}, ignored);
    auto compacted{create(target, impl_->options)};
    if (!compacted) {
        return std::unexpected{compacted.error()};
    }
    for (std::size_t slot{0}; slot < index_slots; ++slot) {
        const auto &entry{impl_->entries[slot]};
        if (entry.empty()) {
            continue;
        }
        auto frame{read_bytes(impl_->file, entry.offset, entry.length)};
        if (!frame) {
            return std::unexpected{frame.error()};
        }
        region_entry metadata{};
        metadata.codec = entry.codec;
        metadata.flags = entry.flags;
        // `append_stored` recomputes offset/length/crc; `entry.uncompressed`
        // was validated by `load()`/`entry_problem` before compaction.
        if (auto status{compacted->impl_->append_stored(
                static_cast<std::uint32_t>(slot), *frame, entry.uncompressed,
                metadata)};
            !status) {
            return std::unexpected{status.error()};
        }
    }
    if (auto status{compacted->impl_->commit()}; !status) {
        return std::unexpected{status.error()};
    }
    auto compacted_stats{compacted->impl_->stats()};
    if (!compacted_stats) {
        return std::unexpected{compacted_stats.error()};
    }
    static_cast<void>(compacted->close());
    // Windows cannot replace an open file: release the source handle first.
    const auto source{std::string{path()}};
    const auto options{impl_->options};
    impl_->file = platform::file::random_access_file{};
    impl_.reset();
    if (auto renamed{platform::file::rename_replace(target, source)};
        !renamed) {
        return std::unexpected{renamed.error()};
    }
    if (auto synced{
            platform::file::sync_directory(core::io::parent_directory(source))};
        !synced) {
        return std::unexpected{synced.error()};
    }
    auto reopened{load(source, options)};
    if (!reopened) {
        return std::unexpected{reopened.error()};
    }
    *this = std::move(*reopened);
    return compacted_stats;
}

namespace {

struct scanned_record {
    std::uint32_t slot{0};
    std::uint64_t offset{0};
    region_entry entry{};
};

} // namespace

auto check_region(std::string_view path, region_options options, bool repair)
    -> result<region_check> {
    if (auto checked{reject_symlink(path)}; !checked) {
        return std::unexpected{checked.error()};
    }
    auto file{platform::file::random_access_file::open(
        path,
        repair ? platform::file::open_mode::read_write
               : platform::file::open_mode::read_only,
        {.no_follow = true})};
    if (!file) {
        return std::unexpected{file.error()};
    }
    const auto size{file->size()};
    if (!size) {
        return std::unexpected{size.error()};
    }
    if (*size > max_region_file_bytes) {
        return corrupt(fmt::format("region file has {} bytes (limit {})", *size,
                                   max_region_file_bytes));
    }
    region_check report{};
    report.file_bytes = *size;
    detail::file_header header{};
    if (*size >= 4096) {
        auto header_bytes{read_bytes(*file, 0, 4096)};
        if (!header_bytes) {
            return std::unexpected{header_bytes.error()};
        }
        auto decoded{detail::decode_file_header(*header_bytes)};
        if (decoded) {
            header = *decoded;
            report.header_valid = true;
        } else {
            report.problems.push_back(
                fmt::format("header: {}", decoded.error().message));
        }
    } else {
        report.problems.push_back(fmt::format(
            "file has {} bytes, shorter than the 4096 byte header", *size));
    }

    std::array<detail::index_slot, 2> slots{};
    std::array<bool, 2> slot_valid{false, false};
    for (std::size_t index{0}; index < 2 && *size >= data_offset; ++index) {
        auto bytes{read_bytes(*file, slot_offsets[index], index_slot_bytes)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        auto decoded{detail::decode_index_slot(*bytes)};
        if (decoded) {
            slots[index] = *decoded;
            slot_valid[index] = true;
        } else {
            report.problems.push_back(fmt::format("index slot {}: {}", index,
                                                  decoded.error().message));
        }
    }
    report.index_valid = slot_valid[0] || slot_valid[1];

    // Scan the append-only data area. Records are 8-byte aligned; a damaged
    // area resynchronises 8 bytes at a time so one bad chunk does not hide the
    // rest of the region. The scan reads a window at a time instead of one
    // 32-byte pread per candidate, so a large garbage tail cannot turn fsck
    // into millions of syscalls.
    constexpr std::size_t scan_window_bytes{256U * 1024U};
    std::vector<scanned_record> scanned;
    std::vector<std::uint64_t> broken_offsets;
    std::uint64_t scan_end{data_offset};
    std::uint64_t live{0};
    auto offset{std::uint64_t{data_offset}};
    std::vector<std::byte> window;
    std::uint64_t window_offset{0};
    std::size_t window_size{0};
    const auto ensure_window = [&](std::uint64_t at) -> status {
        if (at >= window_offset &&
            at + record_header_bytes <= window_offset + window_size) {
            return status_ok();
        }
        window_offset = at;
        const auto remaining{*size - at};
        window_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, scan_window_bytes));
        window.resize(window_size);
        return file->read_at(window_offset, window);
    };
    while (offset + record_header_bytes <= *size) {
        if (auto read{ensure_window(offset)}; !read) {
            return std::unexpected{read.error()};
        }
        const auto relative{static_cast<std::size_t>(offset - window_offset)};
        if (detail::get_u32(window, relative) != 0x48434652U) { // "RFCH"
            bool found{false};
            for (auto candidate{relative + 8}; candidate + 4 <= window_size;
                 candidate += 8) {
                if (detail::get_u32(window, candidate) == 0x48434652U) {
                    offset = window_offset + candidate;
                    found = true;
                    break;
                }
            }
            if (!found) {
                auto advance{window_offset + (window_size / 8) * 8};
                if (advance <= offset) {
                    advance = offset + 8;
                }
                offset = advance;
            }
            continue;
        }
        const auto stored{detail::get_u32(window, relative + 20)};
        if (stored > max_record_bytes) {
            broken_offsets.push_back(offset);
            offset += 8;
            continue;
        }
        const auto total{record_header_bytes + stored};
        if (offset + total > *size) {
            broken_offsets.push_back(offset);
            offset += 8;
            continue;
        }
        auto frame{read_bytes(*file, offset, total)};
        if (!frame) {
            return std::unexpected{frame.error()};
        }
        auto frame_header{detail::decode_frame_header(*frame)};
        if (!frame_header) {
            broken_offsets.push_back(offset);
            offset += 8;
            continue;
        }
        if (detail::frame_crc32c(*frame) != frame_header->record_crc32c) {
            broken_offsets.push_back(offset);
            offset += 8;
            continue;
        }
        const auto region_x{frame_header->pos.x >> 5};
        const auto region_z{frame_header->pos.z >> 5};
        if (region_x != options.x || region_z != options.z) {
            broken_offsets.push_back(offset);
            offset += 8;
            continue;
        }
        scanned_record record{};
        record.slot = region_file::slot_for(frame_header->pos);
        record.offset = offset;
        record.entry.offset = offset;
        record.entry.length = static_cast<std::uint32_t>(total);
        record.entry.crc32c = data::crc32c(*frame);
        record.entry.uncompressed = frame_header->uncompressed_size;
        record.entry.codec = frame_header->codec;
        record.entry.generation = header.generation;
        scanned.push_back(record);
        scan_end = align_up(offset + total, 8);
        live += align_up(total, 8);
        offset = scan_end;
    }
    report.chunks = scanned.size();
    report.live_bytes = live;
    const auto tail{std::max(header.data_tail, scan_end)};
    const auto payload_area{tail > data_offset ? tail - data_offset : 0};
    report.garbage_bytes = payload_area > live ? payload_area - live : 0;
    // Fragments that are not referenced by a valid index are garbage left
    // behind by an earlier failed write or a previous repair; only an index
    // entry that points at an unreadable record is corruption the user must
    // act on. Reporting every fragment would make a repaired world fail fsck
    // forever (the damaged bytes stay in the append-only area).
    std::set<std::uint64_t> referenced;
    const detail::index_slot *best_slot{nullptr};
    if (slot_valid[0] &&
        (!slot_valid[1] || slots[0].generation >= slots[1].generation)) {
        best_slot = &slots[0];
    } else if (slot_valid[1]) {
        best_slot = &slots[1];
    }
    if (best_slot != nullptr) {
        for (std::size_t index{0}; index < index_slots; ++index) {
            const auto &entry{best_slot->entries[index]};
            if (entry.empty()) {
                continue;
            }
            referenced.insert(entry.offset);
            if (auto problem{entry_problem(entry, *size)}; !problem.empty()) {
                report.problems.push_back(
                    fmt::format("index entry {}: {}", index, problem));
            }
        }
    }
    for (const auto broken : broken_offsets) {
        if (best_slot == nullptr || referenced.contains(broken)) {
            report.problems.push_back(
                fmt::format("record at offset {} is unreadable", broken));
        }
    }

    if (!repair || report.problems.empty()) {
        // A healthy region is left byte-for-byte untouched (no generation
        // bump, no gratuitous rewrite). Only a region with a damaged header,
        // index or referenced record is rebuilt.
        return report;
    }

    // Rebuild the index from the data area: newest record per slot wins.
    std::array<region_entry, index_slots> rebuilt{};
    std::array<bool, index_slots> present{};
    for (const auto &record : scanned) {
        if (present[record.slot] &&
            rebuilt[record.slot].offset > record.entry.offset) {
            continue;
        }
        rebuilt[record.slot] = record.entry;
        present[record.slot] = true;
    }
    std::uint64_t generation{std::max(header.generation, std::uint64_t{1}) + 1};
    for (std::size_t index{0}; index < 2; ++index) {
        if (slot_valid[index]) {
            generation = std::max(generation, slots[index].generation + 1);
        }
    }
    detail::index_slot slot{};
    slot.generation = generation;
    slot.entries = rebuilt;
    std::uint64_t rebuilt_count{0};
    for (std::size_t index{0}; index < index_slots; ++index) {
        if (!rebuilt[index].empty()) {
            ++rebuilt_count;
        }
    }
    const auto bytes{detail::encode_index_slot(slot)};
    for (const auto target : slot_offsets) {
        if (auto written{file->write_at(target, bytes)}; !written) {
            return std::unexpected{written.error()};
        }
    }
    if (auto synced{file->sync()}; !synced) {
        return std::unexpected{synced.error()};
    }
    detail::file_header repaired{};
    repaired.x = options.x;
    repaired.z = options.z;
    repaired.active_slot = 0;
    repaired.generation = generation;
    repaired.data_tail = tail;
    repaired.record_count = rebuilt_count;
    repaired.dictionary_id =
        options.dictionary != nullptr ? options.dictionary->id : 0;
    const auto header_bytes{detail::encode_file_header(repaired)};
    if (auto written{file->write_at(0, header_bytes)}; !written) {
        return std::unexpected{written.error()};
    }
    if (auto synced{file->sync()}; !synced) {
        return std::unexpected{synced.error()};
    }
    report.header_valid = true;
    report.index_valid = true;
    report.repaired = true;
    return report;
}

} // namespace refrain::world::rwf
