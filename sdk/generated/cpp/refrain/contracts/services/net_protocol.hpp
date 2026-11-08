// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 927eab4ffcbbd2c4

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_HPP

#include <refrain/contracts/services/net_protocol_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/contracts/core.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::net_protocol {

enum class protocol_edition : std::uint32_t { // enum 'protocol_edition'
    java = 0,
    bedrock = 1,
    refrain = 2,
    custom = 3,
};

enum class protocol_error : std::uint32_t { // enum 'protocol_error'
    frame_too_large = 1,
    malformed_packet = 2,
    unknown_packet = 3,
    unsupported_version = 4,
    compression_failed = 5,
};

enum class protocol_event : std::uint32_t { // enum 'protocol_event'
    status_request = 0,
    login_request = 1,
    login_acknowledged = 2,
    configuration_acknowledged = 3,
};

enum class connection_state : std::uint32_t { // enum 'connection_state'
    handshake = 0,
    status = 1,
    login = 2,
    configuration = 3,
    play = 4,
};

/// Provider identity and transport requirements. `transport_protocol` mirrors
/// the transport service values (tcp=0, tls_tcp=1, raknet=2, ...).
using protocol_descriptor = ::refrain_services_net_protocol_protocol_descriptor;
/// Zero-initialized `protocol_descriptor` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_protocol_descriptor() noexcept -> protocol_descriptor {
    protocol_descriptor value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using frame_chunk = ::refrain_services_net_protocol_frame_chunk;
/// Zero-initialized `frame_chunk` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_frame_chunk() noexcept -> frame_chunk {
    frame_chunk value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using decoded_packet = ::refrain_services_net_protocol_decoded_packet;
/// Zero-initialized `decoded_packet` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_decoded_packet() noexcept -> decoded_packet {
    decoded_packet value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using protocol_version = ::refrain_services_net_protocol_protocol_version;
/// Zero-initialized `protocol_version` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_protocol_version() noexcept -> protocol_version {
    protocol_version value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using frame_header = ::refrain_services_net_protocol_frame_header;
/// Zero-initialized `frame_header` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_frame_header() noexcept -> frame_header {
    frame_header value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Borrowed packet payload; valid for the duration of the contract call.
using packet_view = ::refrain_services_net_protocol_packet_view;
/// Zero-initialized `packet_view` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_packet_view() noexcept -> packet_view {
    packet_view value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using encoded_packet = ::refrain_services_net_protocol_encoded_packet;
/// Zero-initialized `encoded_packet` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_encoded_packet() noexcept -> encoded_packet {
    encoded_packet value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(protocol_descriptor) == 72);
static_assert(alignof(protocol_descriptor) == 8);
static_assert(offsetof(protocol_descriptor, struct_size) == 0);
static_assert(offsetof(protocol_descriptor, edition) == 4);
static_assert(offsetof(protocol_descriptor, family) == 8);
static_assert(offsetof(protocol_descriptor, canonical_version) == 24);
static_assert(offsetof(protocol_descriptor, accepted_versions) == 32);
static_assert(offsetof(protocol_descriptor, transport_protocol) == 48);
static_assert(offsetof(protocol_descriptor, requires_stream) == 52);
static_assert(offsetof(protocol_descriptor, capabilities) == 56);
static_assert(sizeof(frame_chunk) == 24);
static_assert(alignof(frame_chunk) == 8);
static_assert(offsetof(frame_chunk, struct_size) == 0);
static_assert(offsetof(frame_chunk, data) == 8);
static_assert(sizeof(decoded_packet) == 40);
static_assert(alignof(decoded_packet) == 8);
static_assert(offsetof(decoded_packet, struct_size) == 0);
static_assert(offsetof(decoded_packet, header) == 4);
static_assert(offsetof(decoded_packet, packet) == 16);
static_assert(sizeof(protocol_version) == 24);
static_assert(alignof(protocol_version) == 8);
static_assert(offsetof(protocol_version, struct_size) == 0);
static_assert(offsetof(protocol_version, protocol) == 4);
static_assert(offsetof(protocol_version, name) == 8);
static_assert(sizeof(frame_header) == 12);
static_assert(alignof(frame_header) == 4);
static_assert(offsetof(frame_header, struct_size) == 0);
static_assert(offsetof(frame_header, length) == 4);
static_assert(offsetof(frame_header, uncompressed_length) == 8);
static_assert(sizeof(packet_view) == 24);
static_assert(alignof(packet_view) == 8);
static_assert(offsetof(packet_view, struct_size) == 0);
static_assert(offsetof(packet_view, packet_id) == 4);
static_assert(offsetof(packet_view, payload) == 8);
static_assert(sizeof(encoded_packet) == 24);
static_assert(alignof(encoded_packet) == 8);
static_assert(offsetof(encoded_packet, struct_size) == 0);
static_assert(offsetof(encoded_packet, data) == 8);

/// Optional tab-list packet encoder. A protocol provider that exposes this
/// extension owns the version-specific text-component encoding.
/// Provider-implemented extension extending 'refrain:net.protocol'.
/// A host binds the reference once through `provider::extension<T>()`;
/// method calls below dispatch through the bound vtable.
class tab_codec final {
  public:
    using vtable = ::refrain_services_net_protocol_tab_codec_vtable;
    static constexpr std::string_view extension_id =
        "refrain:net.protocol/tab_codec";
    static constexpr std::string_view extends_service =
        "refrain:net.protocol";
    static constexpr std::string_view capability =
        "tab-codec";
    static constexpr std::uint32_t abi_major = 1;
    static constexpr std::uint32_t abi_minor = 0;
    static constexpr std::uint32_t abi_patch = 0;
    /// Packed minimum version requested from a provider:
    /// (major << 16) | minor.
    static constexpr std::uint32_t abi_version =
        (1u << 16u) | 0u;

    tab_codec(const vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    [[nodiscard]] auto table() const noexcept
        -> const vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    [[nodiscard]] auto encode_tab(std::string_view header, std::string_view footer, ::refrain::services::net_protocol::encoded_packet &packet) const -> std::expected<void, ::refrain::services::net_protocol::protocol_error> {
        ::refrain::contracts::init_struct(packet);
        if (const auto code = table_->encode_tab(self_, ::refrain::contracts::to_borrowed(header), ::refrain::contracts::to_borrowed(footer), &packet); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return {};
    }

  private:
    const vtable *table_;
    void *self_;
};

static_assert(offsetof(::refrain_services_net_protocol_tab_codec_vtable, struct_size) == 0);

inline constexpr std::string_view service_id = "refrain:net.protocol";
inline constexpr std::uint32_t abi_major = 2;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::net;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::instanced;
inline constexpr std::string_view capabilities = "frame-compression,registry-sync";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:net.protocol"};
inline constexpr std::string_view extension_ids[] = {
"refrain:net.protocol/tab_codec"};

namespace detail {
[[nodiscard]] inline auto extension_failure(::refrain_status status,
                                           std::string_view id)
    -> ::refrain::error {
    switch (status) {
    case REFRAIN_EXTENSION_NOT_FOUND:
        return ::refrain::make_error(
            ::refrain::error_code::not_found,
            std::string{"extension '"} + std::string{id} + "' is not provided");
    case REFRAIN_EXTENSION_VERSION_MISMATCH:
        return ::refrain::make_error(
            ::refrain::error_code::version_mismatch,
            std::string{"extension '"} + std::string{id} +
                "' does not provide a compatible version");
    case REFRAIN_EXTENSION_INVALID:
    default:
        return ::refrain::make_error(
            ::refrain::error_code::invalid_argument,
            std::string{"extension '"} + std::string{id} +
                "' returned a malformed reference");
    }
}
} // namespace detail

/// One extension a provider offers. Build it with make_extension_binding().
struct extension_binding {
    std::string_view id{};
    std::uint32_t abi_major{0};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
    const void *vtable{nullptr};
    void *self{nullptr};
    std::uint32_t vtable_size{0};
};

/// Stable-id registry over a provider's extension bindings. The bindings and
/// the referenced vtables are owned by the base provider; extensions have no
/// independent lifetime.
class extension_registry final {
  public:
    constexpr explicit extension_registry(
        std::span<const extension_binding> entries) noexcept
        : entries_{entries} {}

    [[nodiscard]] auto query(::refrain_str id, std::uint32_t min_version,
                             ::refrain_extension_ref &out) const noexcept
        -> ::refrain_status {
        if (out.struct_size != REFRAIN_EXTENSION_REF_SIZE) {
            return REFRAIN_EXTENSION_INVALID;
        }
        const auto requested_major{min_version >> 16u};
        const auto requested_minor{min_version & 0xFFFFu};
        const auto wanted{::refrain::contracts::as_string_view(id)};
        for (const auto &entry : entries_) {
            if (entry.id != wanted) {
                continue;
            }
            if (entry.vtable == nullptr || entry.vtable_size == 0) {
                return REFRAIN_EXTENSION_INVALID;
            }
            if (entry.abi_major != requested_major ||
                entry.abi_minor < requested_minor) {
                return REFRAIN_EXTENSION_VERSION_MISMATCH;
            }
            out.abi_major = entry.abi_major;
            out.abi_minor = entry.abi_minor;
            out.abi_patch = entry.abi_patch;
            out.id = ::refrain::contracts::to_borrowed(entry.id);
            out.vtable = entry.vtable;
            out.self = entry.self;
            return REFRAIN_STATUS_OK;
        }
        return REFRAIN_EXTENSION_NOT_FOUND;
    }

  private:
    std::span<const extension_binding> entries_;
};

/// Build one registration entry for `Extension`. A vtable whose struct_size is
/// below the generated size is stored as null and fails closed at query time.
template <typename Extension>
[[nodiscard]] constexpr auto make_extension_binding(
    const typename Extension::vtable &table, void *self) noexcept
    -> extension_binding {
    constexpr auto expected_size{
        static_cast<std::uint32_t>(sizeof(typename Extension::vtable))};
    const auto valid{table.struct_size >= expected_size};
    return extension_binding{
        .id = Extension::extension_id,
        .abi_major = Extension::abi_major,
        .abi_minor = Extension::abi_minor,
        .abi_patch = Extension::abi_patch,
        .vtable = valid ? static_cast<const void *>(&table) : nullptr,
        .self = self,
        .vtable_size = expected_size,
    };
}

/// Vtable adapter: assign `&query_extension<Provider>` to the service's
/// `query_extension` slot. `Provider` must expose a public
/// `extension_registry extensions;` member holding the bindings above.
template <typename Provider>
[[nodiscard]] auto query_extension(void *self, ::refrain_str id,
                                   std::uint32_t min_version,
                                   ::refrain_extension_ref *out) noexcept
    -> ::refrain_status {
    if (self == nullptr || out == nullptr) {
        return REFRAIN_EXTENSION_INVALID;
    }
    return static_cast<const Provider *>(self)->extensions.query(
        id, min_version, *out);
}

/// Type-safe wrapper around `refrain_services_net_protocol_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_net_protocol_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_net_protocol_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    auto descriptor(::refrain::services::net_protocol::protocol_descriptor &value) const -> void {
        ::refrain::contracts::init_struct(value);
        table_->descriptor(self_, &value);
    }

    /// Protocol number implemented by this provider (777 for vanilla 26.3).
    [[nodiscard]] auto protocol_id() const -> std::uint32_t {
        return static_cast<std::uint32_t>(table_->protocol_id(self_));
    }

    [[nodiscard]] auto accepts_version(std::uint32_t protocol) const -> bool {
        return (table_->accepts_version(self_, protocol) != 0);
    }

    /// Create/close the framing, compression and cipher state for one
    /// connection. `next_packet`/`send_packet` operate on this state.
    auto create_connection(std::uint64_t connection) const -> void {
        table_->create_connection(self_, connection);
    }

    auto close_connection(std::uint64_t connection) const -> void {
        table_->close_connection(self_, connection);
    }

    [[nodiscard]] auto set_compression(std::uint64_t connection, std::int32_t threshold) const -> std::expected<void, ::refrain::services::net_protocol::protocol_error> {
        if (const auto code = table_->set_compression(self_, connection, threshold); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto set_cipher(std::uint64_t connection, std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv) const -> std::expected<void, ::refrain::services::net_protocol::protocol_error> {
        if (const auto code = table_->set_cipher(self_, connection, ::refrain::contracts::to_borrowed(key), ::refrain::contracts::to_borrowed(iv)); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return {};
    }

    /// Feed inbound transport bytes. They are decrypted and framed lazily by
    /// next_packet.
    [[nodiscard]] auto feed(std::uint64_t connection, std::span<const std::uint8_t> data) const -> std::expected<void, ::refrain::services::net_protocol::protocol_error> {
        if (const auto code = table_->feed(self_, connection, ::refrain::contracts::to_borrowed(data)); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return {};
    }

    /// Decode one packet from a connection's inbound buffer. Returns false
    /// when more transport bytes are needed.
    [[nodiscard]] auto next_packet(std::uint64_t connection, ::refrain::services::net_protocol::decoded_packet &packet) const -> std::expected<bool, ::refrain::services::net_protocol::protocol_error> {
        ::refrain::contracts::init_struct(packet);
        ::uint8_t value{};
        if (const auto code = table_->next_packet(self_, connection, &packet, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return (value != 0);
    }

    /// Encode and enqueue one packet on the connection.
    [[nodiscard]] auto send_packet(std::uint64_t connection, const ::refrain::services::net_protocol::packet_view &packet) const -> std::expected<void, ::refrain::services::net_protocol::protocol_error> {
        const auto abi_packet = ::refrain::contracts::with_struct_size(packet);
        if (const auto code = table_->send_packet(self_, connection, &abi_packet); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_protocol::protocol_error>(code));
        }
        return {};
    }

    /// Pop one framed/encrypted outbound chunk. `data` is borrowed until the
    /// next take_outbound call on that connection.
    [[nodiscard]] auto take_outbound(std::uint64_t connection, ::refrain::services::net_protocol::frame_chunk &chunk) const -> bool {
        ::refrain::contracts::init_struct(chunk);
        return (table_->take_outbound(self_, connection, &chunk) != 0);
    }

    /// Report a state transition observed by the session shell. The provider
    /// keeps the state for `state_of`; it does not drive transitions.
    auto notify_state(std::uint64_t connection, ::refrain::services::net_protocol::connection_state state) const -> void {
        table_->notify_state(self_, connection, static_cast<refrain_services_net_protocol_connection_state>(state));
    }

    /// Advance the connection state machine for one protocol event. The
    /// provider returns the next state and records it for `state_of`.
    [[nodiscard]] auto transition(std::uint64_t connection, ::refrain::services::net_protocol::protocol_event event) const -> ::refrain::services::net_protocol::connection_state {
        return static_cast<::refrain::services::net_protocol::connection_state>(table_->transition(self_, connection, static_cast<refrain_services_net_protocol_protocol_event>(event)));
    }

    [[nodiscard]] auto state_of(::refrain::core::connection conn) const -> ::refrain::services::net_protocol::connection_state {
        return static_cast<::refrain::services::net_protocol::connection_state>(table_->state_of(self_, conn.raw()));
    }
    using service_vtable = ::refrain_services_net_protocol_vtable;

    /// Bind one extension declared by this service. The stable id, version
    /// and struct sizes are validated once here; the returned wrapper calls
    /// the extension vtable directly.
    template <typename Extension>
    [[nodiscard]] auto extension() const
        -> std::expected<Extension, ::refrain::error> {
        const auto slot_end{static_cast<std::uint32_t>(
            offsetof(service_vtable, query_extension) +
            sizeof(table_->query_extension))};
        if (table_->struct_size < slot_end ||
            table_->query_extension == nullptr) {
            return std::unexpected(::refrain::make_error(
                ::refrain::error_code::unsupported,
                "service provider does not expose extension queries"));
        }
        ::refrain_extension_ref ref{};
        ref.struct_size = REFRAIN_EXTENSION_REF_SIZE;
        const auto status{table_->query_extension(
            self_, ::refrain::contracts::to_borrowed(Extension::extension_id),
            Extension::abi_version, &ref)};
        if (status != REFRAIN_STATUS_OK) {
            return std::unexpected(
                detail::extension_failure(status, Extension::extension_id));
        }
        if (ref.struct_size != REFRAIN_EXTENSION_REF_SIZE ||
            ref.vtable == nullptr ||
            ::refrain::contracts::as_string_view(ref.id) !=
                Extension::extension_id ||
            ref.abi_major != Extension::abi_major ||
            ref.abi_minor < Extension::abi_minor) {
            return std::unexpected(::refrain::make_error(
                ::refrain::error_code::invalid_argument,
                std::string{"extension '"} +
                    std::string{Extension::extension_id} +
                    "' returned a malformed reference"));
        }
        const auto *extension_table{static_cast<
            const typename Extension::vtable *>(ref.vtable)};
        if (extension_table->struct_size <
            static_cast<std::uint32_t>(
                sizeof(typename Extension::vtable))) {
            return std::unexpected(::refrain::make_error(
                ::refrain::error_code::invalid_argument,
                std::string{"extension '"} +
                    std::string{Extension::extension_id} +
                    "' has an undersized vtable"));
        }
        return Extension{*extension_table, ref.self};
    }

  private:
    const ::refrain_services_net_protocol_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::net_protocol

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_HPP
