// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/context.hpp>

#include <algorithm>
#include <utility>

namespace refrain::command {

context::context(refrain::command::source &src, std::string_view input)
    : source_{&src}, input_{input} {}

auto context::source() const noexcept -> command::source & { return *source_; }

auto context::input() const noexcept -> std::string_view { return input_; }

auto context::arguments() const noexcept -> std::span<const argument_entry> {
    return arguments_;
}

auto context::has(std::string_view name) const noexcept -> bool {
    return find(name) != nullptr;
}

auto context::find(std::string_view name) const noexcept
    -> const argument_entry * {
    const auto match =
        std::ranges::find_if(arguments_, [name](const argument_entry &entry) {
            return entry.name == name;
        });
    return match == arguments_.end() ? nullptr : &*match;
}

auto context::value(std::string_view name) const noexcept
    -> const argument_value * {
    const auto *entry = find(name);
    return entry == nullptr ? nullptr : &entry->value;
}

auto context::string(std::string_view name) const noexcept -> std::string_view {
    const auto *entry = find(name);
    if (entry == nullptr) {
        return {};
    }
    if (const auto *borrowed = std::get_if<std::string_view>(&entry->value)) {
        return *borrowed;
    }
    if (const auto *owned = std::get_if<std::string>(&entry->value)) {
        return *owned;
    }
    return {};
}

auto context::integer(std::string_view name) const noexcept -> std::int64_t {
    const auto *entry = find(name);
    if (entry == nullptr) {
        return 0;
    }
    if (const auto *value = std::get_if<std::int64_t>(&entry->value)) {
        return *value;
    }
    return 0;
}

auto context::number(std::string_view name) const noexcept -> double {
    const auto *entry = find(name);
    if (entry == nullptr) {
        return 0.0;
    }
    if (const auto *value = std::get_if<double>(&entry->value)) {
        return *value;
    }
    if (const auto *integer_value = std::get_if<std::int64_t>(&entry->value)) {
        return static_cast<double>(*integer_value);
    }
    return 0.0;
}

auto context::boolean(std::string_view name) const noexcept -> bool {
    const auto *value = get<bool>(name);
    return value != nullptr && *value;
}

auto context::location(std::string_view name) const noexcept
    -> const data::resource_location * {
    return get<data::resource_location>(name);
}

void context::reply(std::string_view message) {
    if (feedback_.size() < max_feedback_messages &&
        message.size() <= max_feedback_bytes - feedback_bytes_) {
        feedback_.emplace_back(message);
        feedback_bytes_ += message.size();
    }
    source_->reply(message);
}

void context::fail(std::string_view message) {
    failed_ = true;
    failure_.assign(message.substr(0, max_failure_bytes));
    source_->reply(message);
}

auto context::failed() const noexcept -> bool { return failed_; }

auto context::failure() const noexcept -> std::string_view { return failure_; }

auto context::feedback() const noexcept -> std::span<const std::string> {
    return feedback_;
}

} // namespace refrain::command
