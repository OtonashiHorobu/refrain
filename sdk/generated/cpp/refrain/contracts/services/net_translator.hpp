// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: e168f1954a0feabb

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_TRANSLATOR_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_TRANSLATOR_HPP

#include <refrain/contracts/services/net_translator_abi.h>
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

namespace refrain::services::net_translator {

enum class translation_error : std::uint32_t { // enum 'translation_error'
    unsupported = 1,
    malformed_packet = 2,
    internal = 3,
};

using translator_descriptor = ::refrain_services_net_translator_translator_descriptor;
/// Zero-initialized `translator_descriptor` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_translator_descriptor() noexcept -> translator_descriptor {
    translator_descriptor value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using translation_request = ::refrain_services_net_translator_translation_request;
/// Zero-initialized `translation_request` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_translation_request() noexcept -> translation_request {
    translation_request value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using translation_result = ::refrain_services_net_translator_translation_result;
/// Zero-initialized `translation_result` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_translation_result() noexcept -> translation_result {
    translation_result value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(translator_descriptor) == 72);
static_assert(alignof(translator_descriptor) == 8);
static_assert(offsetof(translator_descriptor, struct_size) == 0);
static_assert(offsetof(translator_descriptor, name) == 8);
static_assert(offsetof(translator_descriptor, source_family) == 24);
static_assert(offsetof(translator_descriptor, target_family) == 40);
static_assert(offsetof(translator_descriptor, capabilities) == 56);
static_assert(sizeof(translation_request) == 48);
static_assert(alignof(translation_request) == 8);
static_assert(offsetof(translation_request, struct_size) == 0);
static_assert(offsetof(translation_request, connection) == 8);
static_assert(offsetof(translation_request, source_protocol) == 16);
static_assert(offsetof(translation_request, target_protocol) == 20);
static_assert(offsetof(translation_request, packet_id) == 24);
static_assert(offsetof(translation_request, payload) == 32);
static_assert(sizeof(translation_result) == 24);
static_assert(alignof(translation_result) == 8);
static_assert(offsetof(translation_result, struct_size) == 0);
static_assert(offsetof(translation_result, packet_id) == 4);
static_assert(offsetof(translation_result, payload) == 8);

inline constexpr std::string_view service_id = "refrain:net.translator";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::worker;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "packet-rewrite";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:net.translator"};

/// Type-safe wrapper around `refrain_services_net_translator_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_net_translator_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_net_translator_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    auto descriptor(::refrain::services::net_translator::translator_descriptor &value) const -> void {
        ::refrain::contracts::init_struct(value);
        table_->descriptor(self_, &value);
    }

    [[nodiscard]] auto translate(const ::refrain::services::net_translator::translation_request &request, ::refrain::services::net_translator::translation_result &result) const -> std::expected<void, ::refrain::services::net_translator::translation_error> {
        const auto abi_request = ::refrain::contracts::with_struct_size(request);
        ::refrain::contracts::init_struct(result);
        if (const auto code = table_->translate(self_, &abi_request, &result); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::net_translator::translation_error>(code));
        }
        return {};
    }

  private:
    const ::refrain_services_net_translator_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::net_translator

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_NET_TRANSLATOR_HPP
