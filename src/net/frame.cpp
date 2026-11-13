// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/frame.hpp>

#include <refrain/net/primitives.hpp>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <fmt/format.h>

namespace refrain::net {
namespace {

/// Frames up to this size are latency-bound fan-out, not bulk transfer: their
/// absolute bandwidth is small and deflate spends most of the call building
/// the Huffman trees. They are compressed at the fastest level, which costs
/// roughly a third more bytes but several times less CPU. Larger frames, where
/// the ratio is what saves bandwidth, keep the default level.
constexpr std::size_t fast_compression_limit{16U * 1024U};

[[nodiscard]] auto frame_failure(std::string_view message) -> error {
    return make_error(error_code::parse_error, message);
}

/// Parse the three-byte VarInt21 prefix from `data`. Decoding is delegated to
/// `reader::read_varint` over a window capped at three bytes; this function
/// only maps that outcome onto the incremental reader's states. `need_more` is
/// set when the data ends in the middle of the prefix.
struct prefix_result {
    std::size_t bytes{0};
    std::uint32_t value{0};
    bool need_more{false};
    bool invalid{false};
    std::string_view message{};
};

[[nodiscard]] auto parse_prefix(std::span<const std::byte> data)
    -> prefix_result {
    constexpr std::size_t prefix_limit{3};
    const auto window{std::min(data.size(), prefix_limit)};
    reader in{data.first(window)};
    auto decoded{in.read_varint()};
    if (!decoded) {
        if (data.size() < prefix_limit) {
            return prefix_result{.bytes = data.size(), .need_more = true};
        }
        return prefix_result{.bytes = prefix_limit,
                             .invalid = true,
                             .message = "frame length wider than 21 bits"};
    }
    if (*decoded == 0) {
        return prefix_result{.bytes = in.position(),
                             .invalid = true,
                             .message = "frame length is zero"};
    }
    return prefix_result{.bytes = in.position(), .value = *decoded};
}

} // namespace

auto frame_encoder::encode(std::span<const std::byte> packet,
                           std::vector<std::byte> &out) const -> status {
    if (packet.size() > max_frame_length) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("packet body is {} bytes, frame limit is {}",
                                   packet.size(), max_frame_length))};
    }

    if (threshold_ < 0) {
        writer prefix{out};
        prefix.write_varint(static_cast<std::uint32_t>(packet.size()));
        out.insert(out.end(), packet.begin(), packet.end());
        return status_ok();
    }

    const bool compress{packet.size() >= static_cast<std::size_t>(threshold_)};
    if (!compress) {
        // Compression is enabled for the connection: every frame carries the
        // inner body-length VarInt, and an uncompressed body uses zero.
        const auto outer_size{1U + packet.size()};
        if (outer_size > max_frame_length) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           fmt::format("frame is {} bytes, frame limit is {}",
                                       outer_size, max_frame_length))};
        }
        writer prefix{out};
        prefix.write_varint(static_cast<std::uint32_t>(outer_size));
        prefix.write_varint(0);
        out.insert(out.end(), packet.begin(), packet.end());
        return status_ok();
    }

    // A compressed frame carries the body length as an inner VarInt. zlib's
    // `compress2` emits a complete zlib stream, which is what the vanilla
    // Deflater produces for one packet.
    uLongf compressed_size{compressBound(static_cast<uLong>(packet.size()))};
    std::vector<std::byte> compressed(compressed_size);
    const auto level{packet.size() <= fast_compression_limit
                         ? Z_BEST_SPEED
                         : Z_DEFAULT_COMPRESSION};
    const auto result{compress2(reinterpret_cast<Bytef *>(compressed.data()),
                                &compressed_size,
                                reinterpret_cast<const Bytef *>(packet.data()),
                                static_cast<uLong>(packet.size()), level)};
    if (result != Z_OK) {
        return std::unexpected{make_error(
            error_code::unknown,
            fmt::format("zlib compress failed with code {}", result))};
    }
    compressed.resize(compressed_size);

    const auto inner_size{
        varint_size(static_cast<std::uint32_t>(packet.size()))};
    const auto outer_size{inner_size + compressed.size()};
    if (outer_size > max_frame_length) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("compressed frame is {} bytes, frame limit is {}",
                        outer_size, max_frame_length))};
    }
    writer prefix{out};
    prefix.write_varint(static_cast<std::uint32_t>(outer_size));
    prefix.write_varint(static_cast<std::uint32_t>(packet.size()));
    out.insert(out.end(), compressed.begin(), compressed.end());
    return status_ok();
}

auto frame_decoder::push(std::span<const std::byte> bytes) -> status {
    if (!failure_.empty()) {
        return std::unexpected{frame_failure(failure_)};
    }
    if (bytes.empty()) {
        return status_ok();
    }
    if (buffer_.size() - offset_ + bytes.size() > max_frame_buffer) {
        return std::unexpected{frame_failure(
            fmt::format("frame buffer exceeds {} bytes", max_frame_buffer))};
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return status_ok();
}

auto frame_decoder::fail(std::string_view message) -> result<bool> {
    failure_ = std::string{message};
    return std::unexpected{frame_failure(message)};
}

void frame_decoder::compact() {
    if (offset_ == 0) {
        return;
    }
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
        return;
    }
    if (offset_ >= 4096 && offset_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
}

auto frame_decoder::next(std::vector<std::byte> &out) -> result<bool> {
    if (!failure_.empty()) {
        return std::unexpected{frame_failure(failure_)};
    }
    const auto available{std::span<const std::byte>{buffer_}.subspan(offset_)};
    const auto prefix{parse_prefix(available)};
    if (prefix.need_more) {
        return false;
    }
    if (prefix.invalid) {
        return fail(prefix.message);
    }
    const auto frame_length{static_cast<std::size_t>(prefix.value)};
    if (frame_length > max_frame_length) {
        return fail(fmt::format("frame length {} exceeds the 21-bit limit",
                                frame_length));
    }
    if (available.size() < prefix.bytes + frame_length) {
        return false;
    }
    const auto body{available.subspan(prefix.bytes, frame_length)};
    if (threshold_ < 0) {
        out.assign(body.begin(), body.end());
        offset_ += prefix.bytes + frame_length;
        compact();
        return true;
    }

    reader inner{body};
    auto declared{inner.read_varint()};
    if (!declared) {
        return fail("compressed frame has no body length");
    }
    const auto declared_size{static_cast<std::size_t>(*declared)};
    if (declared_size == 0) {
        auto remainder{inner.read_bytes(inner.remaining())};
        if (!remainder) {
            return fail("compressed frame body is truncated");
        }
        out.assign(remainder->begin(), remainder->end());
        offset_ += prefix.bytes + frame_length;
        compact();
        return true;
    }
    if (declared_size < static_cast<std::size_t>(threshold_)) {
        return fail(
            fmt::format("compressed body length {} is below the threshold {}",
                        declared_size, threshold_));
    }
    if (declared_size > max_uncompressed_length) {
        return fail(fmt::format("decompressed body length {} exceeds {}",
                                declared_size, max_uncompressed_length));
    }
    const auto compressed{inner.read_bytes(inner.remaining())};
    if (!compressed) {
        return fail("compressed frame body is truncated");
    }
    if (compressed->size() > max_compressed_length) {
        return fail(fmt::format("compressed body is {} bytes, limit is {}",
                                compressed->size(), max_compressed_length));
    }
    out.resize(declared_size);
    uLongf output_size{static_cast<uLongf>(declared_size)};
    const auto result{
        uncompress(reinterpret_cast<Bytef *>(out.data()), &output_size,
                   reinterpret_cast<const Bytef *>(compressed->data()),
                   static_cast<uLong>(compressed->size()))};
    if (result != Z_OK) {
        return fail(fmt::format("zlib uncompress failed with code {}", result));
    }
    if (output_size != declared_size) {
        return fail(fmt::format("decompressed body is {} bytes, declared {}",
                                output_size, declared_size));
    }
    offset_ += prefix.bytes + frame_length;
    compact();
    return true;
}

} // namespace refrain::net
