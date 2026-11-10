// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/argument.hpp>
#include <refrain/command/source.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace refrain::command {

/// Recorded feedback is bounded so a runaway executor cannot grow the command
/// result without limit. Replies still reach the source sink after the bound.
inline constexpr std::size_t max_feedback_messages = 32;
inline constexpr std::size_t max_feedback_bytes = 8'192;
inline constexpr std::size_t max_failure_bytes = 4'096;

/// One parsed argument as seen by an executor.
struct argument_entry {
    std::string_view name{};
    argument_value value{};
    /// Byte range of the token in the original input.
    std::size_t start{0};
    std::size_t end{0};
};

/// Execution context handed to a command executor.
///
/// The context borrows the input and the parsed argument names from values that
/// stay alive for the duration of the call. It is not copyable and must not be
/// stored by an executor.
class context {
  public:
    context(const context &) = delete;
    context &operator=(const context &) = delete;

    [[nodiscard]] auto source() const noexcept -> command::source &;
    [[nodiscard]] auto input() const noexcept -> std::string_view;
    [[nodiscard]] auto arguments() const noexcept
        -> std::span<const argument_entry>;

    [[nodiscard]] auto has(std::string_view name) const noexcept -> bool;
    [[nodiscard]] auto find(std::string_view name) const noexcept
        -> const argument_entry *;
    [[nodiscard]] auto value(std::string_view name) const noexcept
        -> const argument_value *;

    /// Typed access to one variant alternative. Returns nullptr when the
    /// argument is absent or has another type.
    template <typename T>
    [[nodiscard]] auto get(std::string_view name) const noexcept -> const T * {
        static_assert(!std::is_reference_v<T>);
        const auto *entry = find(name);
        return entry == nullptr ? nullptr : std::get_if<T>(&entry->value);
    }

    /// Convenience accessors. They return the empty/zero value for a missing or
    /// mistyped argument; executors that need to distinguish absence use
    /// `has()` or `get<T>()`.
    [[nodiscard]] auto string(std::string_view name) const noexcept
        -> std::string_view;
    [[nodiscard]] auto integer(std::string_view name) const noexcept
        -> std::int64_t;
    [[nodiscard]] auto number(std::string_view name) const noexcept -> double;
    [[nodiscard]] auto boolean(std::string_view name) const noexcept -> bool;
    [[nodiscard]] auto location(std::string_view name) const noexcept
        -> const data::resource_location *;

    /// Send one feedback line through the source sink and record it in the
    /// execution result (within the feedback bounds).
    void reply(std::string_view message);

    /// Mark the execution as failed, remember the message and send it through
    /// the source sink.
    void fail(std::string_view message);

    [[nodiscard]] auto failed() const noexcept -> bool;
    [[nodiscard]] auto failure() const noexcept -> std::string_view;
    [[nodiscard]] auto feedback() const noexcept
        -> std::span<const std::string>;

  private:
    friend class dispatcher;

    context(refrain::command::source &src, std::string_view input);

    refrain::command::source *source_;
    std::string_view input_;
    std::vector<argument_entry> arguments_{};
    std::vector<std::string> feedback_{};
    std::string failure_{};
    std::size_t feedback_bytes_{0};
    bool failed_{false};
};

} // namespace refrain::command
