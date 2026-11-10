// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::net {

/// Maximum number of bytes buffered while waiting for a complete frame. One
/// maximum frame plus its three-byte prefix; a larger buffer means the peer
/// declared a frame we will reject anyway.
inline constexpr std::size_t max_frame_buffer = max_frame_length + 3;

/// Outbound frame writer for one connection.
///
/// The compression threshold is set from `login_compression` and stays for the
/// lifetime of the connection. `-1` disables compression (handshake/status and
/// the login packets before the switch).
class frame_encoder {
  public:
    frame_encoder() = default;
    explicit frame_encoder(int threshold) noexcept : threshold_{threshold} {}

    /// `-1` disables compression; `0` compresses every body; positive values
    /// compress bodies at or above the threshold.
    void set_threshold(int threshold) noexcept { threshold_ = threshold; }
    [[nodiscard]] auto threshold() const noexcept -> int { return threshold_; }

    /// Append one complete frame (VarInt21 length prefix + optional
    /// compression envelope + body) to `out`. `packet` is the packet body
    /// (packet id + fields) without any framing.
    [[nodiscard]] auto encode(std::span<const std::byte> packet,
                              std::vector<std::byte> &out) const -> status;

  private:
    int threshold_{-1};
};

/// Incremental inbound frame reader for one connection. Bytes are pushed as
/// they arrive from the socket; `next` returns true and fills `out` for each
/// complete packet body. A malformed or oversized frame makes the decoder
/// failed; every later call returns the same error.
class frame_decoder {
  public:
    frame_decoder() = default;
    explicit frame_decoder(int threshold) noexcept : threshold_{threshold} {}

    void set_threshold(int threshold) noexcept { threshold_ = threshold; }
    [[nodiscard]] auto threshold() const noexcept -> int { return threshold_; }

    [[nodiscard]] auto push(std::span<const std::byte> bytes) -> status;

    /// Decode one body. Returns true when `out` was filled, false when more
    /// bytes are needed. A protocol error is returned once and latched.
    [[nodiscard]] auto next(std::vector<std::byte> &out) -> result<bool>;

    [[nodiscard]] auto buffered() const noexcept -> std::size_t {
        return buffer_.size() - offset_;
    }
    [[nodiscard]] auto failed() const noexcept -> bool {
        return !failure_.empty();
    }

  private:
    [[nodiscard]] auto fail(std::string_view message) -> result<bool>;
    void compact();

    std::vector<std::byte> buffer_{};
    std::size_t offset_{0};
    int threshold_{-1};
    std::string failure_{};
};

} // namespace refrain::net
