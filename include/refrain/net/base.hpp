// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/resource_location.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::net {

/// Maximum packet body length encoded by the three-byte VarInt21 frame prefix.
inline constexpr std::size_t max_frame_length = 2'097'151;

/// Compressed frame ceiling enforced by the compression stage.
inline constexpr std::size_t max_compressed_length = 2'097'152;

/// Decompressed packet ceiling enforced by the compression stage.
inline constexpr std::size_t max_uncompressed_length = 8'388'608;

/// Vanilla character ceiling for a single string field (UTF-16 code units).
inline constexpr std::size_t max_string_characters = 32'767;

/// Byte ceiling for encoding a string field: four bytes per character is the
/// widest legal UTF-8 encoding.
inline constexpr std::size_t max_string_bytes = max_string_characters * 4;

enum class connection_state : std::uint8_t {
    handshake = 0,
    status = 1,
    login = 2,
    configuration = 3,
    play = 4,
};

enum class packet_flow : std::uint8_t {
    serverbound = 0,
    clientbound = 1,
};

[[nodiscard]] auto to_string(connection_state state) noexcept
    -> std::string_view;
[[nodiscard]] auto to_string(packet_flow flow) noexcept -> std::string_view;

/// A 128-bit identifier in wire order. The first eight bytes are the most
/// significant half on the wire; `bytes` keeps them in network order so the
/// textual form needs no byte swapping.
struct uuid {
    std::array<std::uint8_t, 16> bytes{};

    friend auto operator==(const uuid &, const uuid &) -> bool = default;

    /// Canonical `8-4-4-4-12` lowercase hex form.
    [[nodiscard]] auto to_string() const -> std::string;

    /// Parse the canonical form (hyphens required, lowercase or uppercase).
    [[nodiscard]] static auto parse(std::string_view text) -> result<uuid>;
};

/// Bounds-checked big-endian reader over one packet body. Every call validates
/// the remaining length before touching memory; a truncated packet yields
/// `error_code::parse_error` and leaves the reader usable.
class reader {
  public:
    explicit reader(std::span<const std::byte> data) noexcept : data_{data} {}

    [[nodiscard]] auto remaining() const noexcept -> std::size_t {
        return data_.size() - position_;
    }
    [[nodiscard]] auto position() const noexcept -> std::size_t {
        return position_;
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return remaining() == 0;
    }

    [[nodiscard]] auto read_u8() -> result<std::uint8_t>;
    [[nodiscard]] auto read_u16() -> result<std::uint16_t>;
    [[nodiscard]] auto read_i32() -> result<std::int32_t>;
    [[nodiscard]] auto read_i64() -> result<std::int64_t>;
    [[nodiscard]] auto read_f32() -> result<float>;
    [[nodiscard]] auto read_f64() -> result<double>;
    [[nodiscard]] auto read_bool() -> result<bool>;

    /// VarInt (32-bit, at most five bytes) and VarLong (64-bit, at most ten).
    [[nodiscard]] auto read_varint() -> result<std::uint32_t>;
    [[nodiscard]] auto read_varlong() -> result<std::uint64_t>;

    /// VarInt byte length + UTF-8 payload. `max_characters` is the UTF-16
    /// code-unit ceiling for the field; the payload is additionally bounded by
    /// four bytes per code unit, so an oversize string is rejected before it is
    /// allocated.
    [[nodiscard]] auto
    read_string(std::size_t max_characters = max_string_characters)
        -> result<std::string>;

    /// Resource location: VarInt byte length + `namespace:path`.
    [[nodiscard]] auto read_identifier() -> result<data::resource_location>;

    /// Two big-endian i64 halves.
    [[nodiscard]] auto read_uuid() -> result<uuid>;

    /// Borrowed view of the next `count` bytes.
    [[nodiscard]] auto read_bytes(std::size_t count)
        -> result<std::span<const std::byte>>;
    [[nodiscard]] auto skip(std::size_t count) -> status;

  private:
    [[nodiscard]] auto need(std::size_t count) -> status;

    std::span<const std::byte> data_{};
    std::size_t position_{0};
};

/// Appender for one packet body. Writes never fail for scalar values; string
/// and identifier writes enforce their field ceiling. The caller checks the
/// final body size against `max_frame_length`.
class writer {
  public:
    explicit writer(std::vector<std::byte> &out) noexcept : out_{&out} {}

    void write_u8(std::uint8_t value);
    void write_u16(std::uint16_t value);
    void write_i32(std::int32_t value);
    void write_i64(std::int64_t value);
    void write_f32(float value);
    void write_f64(double value);
    void write_bool(bool value);

    void write_varint(std::uint32_t value);
    void write_varlong(std::uint64_t value);

    [[nodiscard]] auto write_string(std::string_view text,
                                    std::size_t max_bytes = max_string_bytes)
        -> status;
    [[nodiscard]] auto write_identifier(const data::resource_location &value)
        -> status;
    [[nodiscard]] auto write_uuid(const uuid &value) -> status;
    void write_bytes(std::span<const std::byte> bytes);

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return out_->size();
    }

  private:
    std::vector<std::byte> *out_;
};

/// True when `text` is well-formed UTF-8 (no overlong forms, surrogates or
/// code points above U+10FFFF).
[[nodiscard]] auto is_valid_utf8(std::string_view text) noexcept -> bool;

} // namespace refrain::net
