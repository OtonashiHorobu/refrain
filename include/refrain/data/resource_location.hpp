// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::data {

// Minecraft resource location (`namespace:path`, e.g. `minecraft:block/stone`).
//
// Parsing follows vanilla rules: the namespace defaults to `minecraft` when the
// colon is omitted; namespace and path are lowercase ASCII `[a-z0-9_.-]` (the
// path additionally allows `/` separators); no empty/`.`/`..` segments.
// Resource locations are the identity of data-driven content (tags, recipes,
// loot tables, advancements, worldgen, registries) and are kept as an explicit
// value type instead of raw strings so validation happens at the parse
// boundary.

class resource_location {
  public:
    resource_location() = default;

    /// Construct from validated parts; precondition: both parts are valid
    /// (use parse()/make() for untrusted input).
    resource_location(std::string namespace_part, std::string path_part)
        : namespace_{std::move(namespace_part)}, path_{std::move(path_part)} {}

    /// Parse `[namespace:]path`; a missing namespace means `minecraft`.
    /// Fails with `invalid_argument` for empty/invalid namespaces, paths or
    /// extra separators.
    [[nodiscard]] static auto parse(std::string_view text)
        -> result<resource_location>;

    /// Validate + build from separate parts (untrusted input).
    [[nodiscard]] static auto make(std::string_view namespace_part,
                                   std::string_view path_part)
        -> result<resource_location>;

    [[nodiscard]] auto namespace_part() const noexcept -> std::string_view {
        return namespace_;
    }

    [[nodiscard]] auto path_part() const noexcept -> std::string_view {
        return path_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return namespace_.empty() && path_.empty();
    }

    /// Canonical `namespace:path` form (allocation).
    [[nodiscard]] auto to_string() const -> std::string {
        std::string text;
        text.reserve(namespace_.size() + path_.size() + 1U);
        text += namespace_;
        text.push_back(':');
        text += path_;
        return text;
    }

    friend auto operator==(const resource_location &, const resource_location &)
        -> bool = default;
    friend auto operator<=>(const resource_location &,
                            const resource_location &) = default;

    /// True when `text` is a valid namespace (lowercase, non-empty).
    [[nodiscard]] static auto is_valid_namespace(std::string_view text) noexcept
        -> bool;

    /// True when `text` is a valid path (lowercase, '/'-separated, non-empty
    /// segments, no `.`/`..`).
    [[nodiscard]] static auto is_valid_path(std::string_view text) noexcept
        -> bool;

  private:
    std::string namespace_{};
    std::string path_{};
};

inline auto
resource_location::is_valid_namespace(std::string_view text) noexcept -> bool {
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') ||
                           character == '_' || character == '-' ||
                           character == '.';
        if (!valid) {
            return false;
        }
    }
    return true;
}

inline auto resource_location::is_valid_path(std::string_view text) noexcept
    -> bool {
    if (text.empty() || text.front() == '/' || text.back() == '/') {
        return false;
    }
    std::size_t start{0};
    while (start <= text.size()) {
        const auto end{text.find('/', start)};
        const auto segment{text.substr(start, end == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : end - start)};
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        for (const char character : segment) {
            const bool valid = (character >= 'a' && character <= 'z') ||
                               (character >= '0' && character <= '9') ||
                               character == '_' || character == '-' ||
                               character == '.';
            if (!valid) {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

inline auto resource_location::parse(std::string_view text)
    -> result<resource_location> {
    const auto colon{text.find(':')};
    if (colon == std::string_view::npos) {
        if (!is_valid_path(text)) {
            return std::unexpected{make_error(error_code::invalid_argument,
                                              "resource location '" +
                                                  std::string{text} +
                                                  "' has an invalid path")};
        }
        return resource_location{"minecraft", std::string{text}};
    }
    if (text.find(':', colon + 1) != std::string_view::npos) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "resource location '" +
                                              std::string{text} +
                                              "' contains more than one ':'")};
    }
    const auto namespace_part{text.substr(0, colon)};
    const auto path_part{text.substr(colon + 1)};
    if (!is_valid_namespace(namespace_part)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "resource location '" +
                                              std::string{text} +
                                              "' has an invalid namespace")};
    }
    if (!is_valid_path(path_part)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "resource location '" +
                                              std::string{text} +
                                              "' has an invalid path")};
    }
    return resource_location{std::string{namespace_part},
                             std::string{path_part}};
}

inline auto resource_location::make(std::string_view namespace_part,
                                    std::string_view path_part)
    -> result<resource_location> {
    if (!is_valid_namespace(namespace_part)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "resource location namespace '" +
                                              std::string{namespace_part} +
                                              "' is invalid")};
    }
    if (!is_valid_path(path_part)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "resource location path '" +
                                              std::string{path_part} +
                                              "' is invalid")};
    }
    return resource_location{std::string{namespace_part},
                             std::string{path_part}};
}

} // namespace refrain::data

namespace std {

template <> struct hash<refrain::data::resource_location> {
    [[nodiscard]] auto
    operator()(const refrain::data::resource_location &location) const noexcept
        -> std::size_t {
        // Small, dependency-free combine over the canonical text; callers that
        // need high-quality hashing use the flat_map/ordered containers.
        std::size_t seed{0xcbf29ce484222325ULL};
        const auto feed = [&seed](std::string_view text) {
            for (const char character : text) {
                seed ^= static_cast<std::size_t>(
                    static_cast<unsigned char>(character));
                seed *= 0x100000001b3ULL;
            }
            seed ^= static_cast<std::size_t>(':');
            seed *= 0x100000001b3ULL;
        };
        feed(location.namespace_part());
        feed(location.path_part());
        return seed;
    }
};

} // namespace std
