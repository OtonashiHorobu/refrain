// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 4d4424e4f6b78480

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_DATA_PACKS_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_DATA_PACKS_HPP

#include <refrain/contracts/services/data_packs_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::data_packs {

enum class pack_kind : std::uint32_t { // enum 'pack_kind'
    data = 0,
    resource = 1,
    mixed = 2,
};

/// Pack errors. Zero is success, so entries start at 1.
enum class pack_error : std::uint32_t { // enum 'pack_error'
    not_found = 1,
    invalid_format = 2,
    version_mismatch = 3,
    io_error = 4,
    already_mounted = 5,
};

namespace detail {
struct pack_tag;
} // namespace detail

/// Opaque handle for one mounted pack.
using pack = ::refrain::contracts::handle<detail::pack_tag>;

/// Where a pack comes from; `path` is a UTF-8 filesystem path or virtual
/// location understood by the provider.
using pack_source = ::refrain_services_data_packs_pack_source;
/// Zero-initialized `pack_source` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_pack_source() noexcept -> pack_source {
    pack_source value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using pack_info = ::refrain_services_data_packs_pack_info;
/// Zero-initialized `pack_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_pack_info() noexcept -> pack_info {
    pack_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(pack_source) == 32);
static_assert(alignof(pack_source) == 8);
static_assert(offsetof(pack_source, struct_size) == 0);
static_assert(offsetof(pack_source, path) == 8);
static_assert(offsetof(pack_source, kind) == 24);
static_assert(offsetof(pack_source, pack_format) == 28);
static_assert(sizeof(pack_info) == 64);
static_assert(alignof(pack_info) == 8);
static_assert(offsetof(pack_info, struct_size) == 0);
static_assert(offsetof(pack_info, pack) == 8);
static_assert(offsetof(pack_info, id) == 16);
static_assert(offsetof(pack_info, version) == 32);
static_assert(offsetof(pack_info, pack_format) == 48);
static_assert(offsetof(pack_info, file_count) == 56);

inline constexpr std::string_view service_id = "refrain:data.packs";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::both;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::main;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "jar,datapack,resourcepack";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:data.packs"};

/// Type-safe wrapper around `refrain_services_data_packs_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_data_packs_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_data_packs_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Mount a pack and return its metadata. Only callable while the registry
    /// is still open (on_load phase).
    [[nodiscard]] auto mount(const ::refrain::services::data_packs::pack_source &source) const -> std::expected<::refrain::services::data_packs::pack_info, ::refrain::services::data_packs::pack_error> {
        const auto abi_source = ::refrain::contracts::with_struct_size(source);
        ::refrain_services_data_packs_pack_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->mount(self_, &abi_source, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::data_packs::pack_error>(code));
        }
        return value;
    }

    [[nodiscard]] auto unmount(::refrain::services::data_packs::pack handle) const -> std::expected<void, ::refrain::services::data_packs::pack_error> {
        if (const auto code = table_->unmount(self_, handle.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::data_packs::pack_error>(code));
        }
        return {};
    }

    /// Number of mounted packs.
    [[nodiscard]] auto mounted_count() const -> std::uint32_t {
        return static_cast<std::uint32_t>(table_->mounted_count(self_));
    }

    /// Borrowed resource bytes; valid until the next registry mutation.
    [[nodiscard]] auto read_resource(::refrain::services::data_packs::pack handle, std::string_view name) const -> std::expected<std::span<const std::uint8_t>, ::refrain::services::data_packs::pack_error> {
        ::refrain_bytes value{};
        if (const auto code = table_->read_resource(self_, handle.raw(), ::refrain::contracts::to_borrowed(name), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::data_packs::pack_error>(code));
        }
        return ::refrain::contracts::as_bytes(value);
    }

  private:
    const ::refrain_services_data_packs_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::data_packs

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_DATA_PACKS_HPP
