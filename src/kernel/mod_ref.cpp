// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/kernel/mod_ref.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <fmt/format.h>

namespace refrain::kernel {

namespace {

[[nodiscard]] auto is_ascii_alnum(char value) noexcept -> bool {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

[[nodiscard]] auto is_mod_id_char(char value) noexcept -> bool {
    return is_ascii_alnum(value) || value == '.' || value == '_' ||
           value == '-';
}

[[nodiscard]] auto is_name_char(char value) noexcept -> bool {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == '-';
}

[[nodiscard]] auto trim(std::string_view text) noexcept -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

/// Split `id@version`, validating the id. Returns the raw version text which
/// callers then validate as an exact SemVer or as a range.
[[nodiscard]] auto split_reference(std::string_view text)
    -> result<std::pair<std::string, std::string>> {
    const auto at{text.find('@')};
    if (at == std::string_view::npos) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod reference '{}' has no '@version'", text))};
    }
    if (text.find('@', at + 1) != std::string_view::npos) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod reference '{}' contains more than one '@'",
                             text))};
    }
    auto id{std::string{text.substr(0, at)}};
    auto version{std::string{text.substr(at + 1)}};
    if (!is_valid_mod_id(id)) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("mod reference '{}' has an invalid mod id", text))};
    }
    if (version.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod reference '{}' has an empty version", text))};
    }
    return std::pair{std::move(id), std::move(version)};
}

} // namespace

auto mod_ref::text() const -> std::string { return id + '@' + version; }

auto mod_ref::parsed() const -> result<core::semver> {
    return core::semver::parse(version);
}

auto parse_mod_ref(std::string_view text) -> result<mod_ref> {
    auto split{split_reference(text)};
    if (!split) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod reference '{}' is malformed: {}", text,
                             split.error().message))};
    }
    auto [id, version] = std::move(*split);
    auto parsed{core::semver::parse(version)};
    if (!parsed) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("mod reference '{}' has an invalid version (expected "
                        "SemVer major.minor.patch): {}",
                        text, parsed.error().message))};
    }
    return mod_ref{.id = std::move(id), .version = std::move(version)};
}

auto mod_requirement::text() const -> std::string { return id + '@' + range; }

auto parse_mod_requirement(std::string_view text) -> result<mod_requirement> {
    const auto at{text.find('@')};
    if (at == std::string_view::npos) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod requirement '{}' has no '@range'", text))};
    }
    if (text.find('@', at + 1) != std::string_view::npos) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod requirement '{}' contains more than one '@'",
                             text))};
    }
    const auto id{trim(text.substr(0, at))};
    const auto range_text{trim(text.substr(at + 1))};
    if (!is_valid_mod_id(id)) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("mod requirement '{}' has an invalid mod id", text))};
    }
    if (range_text.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod requirement '{}' has an empty version range",
                             text))};
    }
    const auto range{core::parse_range(range_text)};
    if (!range) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("mod requirement '{}' has an invalid version range: {}",
                        text, range.error().message))};
    }
    return mod_requirement{.id = std::string{id},
                           .range = std::string{range_text}};
}

auto is_valid_mod_id(std::string_view id) noexcept -> bool {
    if (id.empty() || id.size() > max_mod_id_length) {
        return false;
    }
    if (!is_ascii_alnum(id.front())) {
        return false;
    }
    return std::all_of(id.begin() + 1, id.end(), is_mod_id_char);
}

auto is_valid_service_id(std::string_view id) noexcept -> bool {
    const auto colon{id.find(':')};
    if (colon == std::string_view::npos || colon == 0 ||
        colon + 1 >= id.size()) {
        return false;
    }
    if (id.find(':', colon + 1) != std::string_view::npos) {
        return false;
    }
    const auto namespace_part{id.substr(0, colon)};
    const auto name_part{id.substr(colon + 1)};
    if (namespace_part.front() == '.' || namespace_part.back() == '.' ||
        name_part.front() == '.' || name_part.back() == '.') {
        return false;
    }
    if (namespace_part.find("..") != std::string_view::npos ||
        name_part.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(namespace_part.begin(), namespace_part.end(),
                       is_name_char) &&
           std::all_of(name_part.begin(), name_part.end(), is_name_char);
}

} // namespace refrain::kernel
