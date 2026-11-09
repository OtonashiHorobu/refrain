// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/core.hpp>
#include <refrain/contracts/host/platform.hpp>
#include <refrain/core/config.hpp>
#include <refrain/host/context.hpp>
#include <refrain/host/platform_context.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/platform/mmap.hpp>
#include <refrain/platform/random_access_file.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string_view>

namespace refrain::app {

// Concrete host runtime context used by `refrain-server`. It exposes the
// process-owned service registry and allocation/log/time primitives through
// the sealed `refrain:host.core` ABI. Config getters stay unsupported until
// the host config store lands (ADR-066 Phase 2); the methods are present so
// the ABI shape does not change when that happens.
class host_context final : public refrain::host::context,
                           public refrain::host::platform_context {
  public:
    /// `tick` may be empty; `tick_now()` then reports -1 (no tick loop).
    /// `config` may be null; config getters then return `unsupported`.
    explicit host_context(refrain::mod::host &owner,
                          std::function<std::int64_t()> tick = {},
                          const refrain::core::config::store *config = nullptr);

    host_context(const host_context &) = delete;
    auto operator=(const host_context &) -> host_context & = delete;
    host_context(host_context &&) = delete;
    auto operator=(host_context &&) -> host_context & = delete;
    ~host_context() override = default;

    void log(std::uint32_t level, std::string_view message) noexcept override;
    [[nodiscard]] auto now_ns() const noexcept -> std::int64_t override;
    [[nodiscard]] auto tick_now() const noexcept -> std::int64_t override;
    void
    version(refrain::host::core::host_version &out) const noexcept override;

    [[nodiscard]] auto config_i64(std::string_view ns, std::string_view key)
        -> refrain::result<std::int64_t> override;
    [[nodiscard]] auto config_bool(std::string_view ns, std::string_view key)
        -> refrain::result<bool> override;
    [[nodiscard]] auto config_string(std::string_view ns, std::string_view key)
        -> refrain::result<std::string_view> override;

    [[nodiscard]] auto query_service(std::string_view service,
                                     std::string_view instance)
        -> refrain::result<refrain::host::core::provider_ref> override;
    [[nodiscard]] auto
    register_provider(std::string_view service, std::string_view instance,
                      const refrain::host::core::provider_ref &ref)
        -> refrain::status override;

    [[nodiscard]] auto alloc(std::uint64_t size, std::uint64_t alignment)
        -> refrain::result<std::span<std::uint8_t>> override;
    void free(std::span<std::uint8_t> block) noexcept override;

    // `refrain::host.platform` implementation.
    [[nodiscard]] auto file_open(std::string_view path,
                                 refrain::host::platform::file_open_flags flags)
        -> refrain::result<refrain::host::platform::file> override;
    [[nodiscard]] auto file_close(refrain::host::platform::file handle)
        -> refrain::status override;
    [[nodiscard]] auto file_read(refrain::host::platform::file handle,
                                 std::uint64_t offset,
                                 std::span<std::uint8_t> buffer)
        -> refrain::result<std::uint64_t> override;
    [[nodiscard]] auto file_write(refrain::host::platform::file handle,
                                  std::uint64_t offset,
                                  std::span<const std::uint8_t> data)
        -> refrain::result<std::uint64_t> override;
    [[nodiscard]] auto file_size(refrain::host::platform::file handle)
        -> refrain::result<std::uint64_t> override;
    [[nodiscard]] auto file_sync(refrain::host::platform::file handle)
        -> refrain::status override;
    [[nodiscard]] auto file_exists(std::string_view path) -> bool override;

    [[nodiscard]] auto mmap_open(std::string_view path)
        -> refrain::result<refrain::host::platform::map> override;
    [[nodiscard]] auto mmap_data(refrain::host::platform::map handle)
        -> refrain::result<std::span<const std::uint8_t>> override;
    [[nodiscard]] auto mmap_close(refrain::host::platform::map handle)
        -> refrain::status override;

    void crash_annotation(std::string_view key,
                          std::string_view value) noexcept override;
    [[nodiscard]] auto temp_directory() noexcept -> std::string_view override;

  private:
    struct file_entry {
        refrain::platform::file::random_access_file file;
        bool read_only{false};
    };

    refrain::mod::host &owner_;
    std::function<std::int64_t()> tick_;
    const refrain::core::config::store *config_{nullptr};
    std::mutex platform_mutex_{};
    std::map<std::uint64_t, file_entry> files_{};
    std::map<std::uint64_t, refrain::platform::mapped_file> maps_{};
    std::uint64_t next_handle_{1};
};

} // namespace refrain::app
