// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Minimal external native mod used by the host loader test. It registers a
// fake provider for `refrain:data.packs`; the host only validates the vtable
// ABI header during registration, so the slots after it are not exercised.

#include <refrain/mod/entry.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {

struct fake_vtable {
    std::uint32_t struct_size{16};
    std::uint32_t abi_major{1};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
};

fake_vtable g_vtable{};
int g_self{0};

auto to_str(const char *text) -> refrain_str {
    std::size_t length{0};
    while (text[length] != '\0') {
        ++length;
    }
    return refrain_str{reinterpret_cast<const std::uint8_t *>(text),
                       static_cast<std::uint64_t>(length)};
}

auto on_load(const refrain_mod_context *context, void **user)
    -> refrain_status {
    if (context == nullptr || context->host_core == nullptr ||
        context->host_core_self == nullptr) {
        return 1;
    }
    refrain_host_core_provider_ref ref{};
    ref.struct_size =
        static_cast<std::uint32_t>(sizeof(refrain_host_core_provider_ref));
    ref.vtable = &g_vtable;
    ref.self = &g_self;
    ref.abi_major = 1;
    const auto service{to_str("refrain:data.packs")};
    const refrain_str instance{};
    const auto status{context->host_core->register_provider(
        context->host_core_self, service, instance, &ref)};
    if (status != REFRAIN_STATUS_OK) {
        return status;
    }
    *user = &g_self;
    return REFRAIN_STATUS_OK;
}

std::atomic<std::uint32_t> g_unload_count{0};
std::atomic<std::uint32_t> g_last_unload_reason{0};

void on_unload(void *user) { static_cast<void>(user); }

auto on_unload_ex(void *user, std::uint32_t reason) -> refrain_status {
    static_cast<void>(user);
    g_unload_count.fetch_add(1, std::memory_order_relaxed);
    g_last_unload_reason.store(reason, std::memory_order_relaxed);
    // The lifecycle test flips this to prove a failing on_unload quarantines
    // the instance instead of being called again.
    if (const char *fail{std::getenv("REFRAIN_TEST_EXTERNAL_MOD_FAIL_UNLOAD")};
        fail != nullptr && fail[0] == '1') {
        return 1;
    }
    return REFRAIN_STATUS_OK;
}

const refrain_mod_desc g_desc{
    .struct_size = static_cast<std::uint32_t>(sizeof(refrain_mod_desc)),
    .abi_major = REFRAIN_MOD_ABI_MAJOR,
    .on_load = &on_load,
    .on_unload = &on_unload,
    .on_unload_ex = &on_unload_ex,
};

} // namespace

/// Observation points for the host lifecycle test: the loaded module and the
/// test binary are separate link units, so the counters cross the boundary
/// through exported functions.
extern "C" {
REFRAIN_EXPORT auto refrain_test_external_unload_count() -> std::uint32_t {
    return g_unload_count.load(std::memory_order_relaxed);
}
REFRAIN_EXPORT auto refrain_test_external_last_unload_reason()
    -> std::uint32_t {
    return g_last_unload_reason.load(std::memory_order_relaxed);
}
}

REFRAIN_MOD_MAIN(refrain_mod_main) {
    static_cast<void>(init);
    return &g_desc;
}
