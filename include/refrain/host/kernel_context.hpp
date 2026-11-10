// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/kernel.hpp>
#include <refrain/core/error.hpp>

#include <cstdint>

namespace refrain::host {

// Abstract host kernel context behind the sealed `refrain:host.kernel`
// service. Job callbacks run on host worker threads; event callbacks run on
// the publishing thread.
class kernel_context {
  public:
    kernel_context() = default;
    kernel_context(const kernel_context &) = delete;
    auto operator=(const kernel_context &) -> kernel_context & = delete;
    kernel_context(kernel_context &&) = delete;
    auto operator=(kernel_context &&) -> kernel_context & = delete;
    virtual ~kernel_context() = default;

    [[nodiscard]] virtual auto
    job_submit(std::uint32_t priority,
               refrain::host::kernel::job_callback callback, void *userdata)
        -> result<refrain::host::kernel::job> = 0;
    [[nodiscard]] virtual auto job_cancel(refrain::host::kernel::job handle)
        -> status = 0;
    [[nodiscard]] virtual auto job_wait(refrain::host::kernel::job handle)
        -> status = 0;

    [[nodiscard]] virtual auto
    event_subscribe(std::uint64_t event_type,
                    refrain::host::kernel::event_callback callback,
                    void *userdata)
        -> result<refrain::host::kernel::subscription> = 0;
    [[nodiscard]] virtual auto
    event_unsubscribe(refrain::host::kernel::subscription handle) -> status = 0;
    virtual void event_publish(std::uint64_t event_type, void *payload,
                               std::uint64_t size) = 0;
};

} // namespace refrain::host
