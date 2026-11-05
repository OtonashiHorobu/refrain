// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/registry.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

namespace {

/// Domain-separation tag of canonical fingerprint encoding v1. Changing the
/// encoding requires bumping this tag (and the documented version above).
constexpr std::string_view fingerprint_domain{
    "refrain.registry.fingerprint.v1"};

/// Append `value` as 8 little-endian bytes.
void append_u64(std::string &out, std::uint64_t value) {
    for (std::size_t byte{0}; byte < 8U; ++byte) {
        out.push_back(static_cast<char>(value & 0xFFU));
        value >>= 8U;
    }
}

} // namespace

auto registry_id::to_string() const -> std::string {
    if (!valid()) {
        return "invalid";
    }
    return std::to_string(value_);
}

namespace detail {

auto fingerprint_of(std::string_view type_name,
                    std::span<const resource_location> locations) -> digest32 {
    // Canonical order is the ascending canonical `namespace:path` text, not the
    // registration order: equal location sets always fingerprint identically.
    std::vector<std::string> texts;
    texts.reserve(locations.size());
    for (const auto &location : locations) {
        texts.push_back(location.to_string());
    }
    std::sort(texts.begin(), texts.end());

    std::string encoded;
    encoded.reserve(fingerprint_domain.size() + 1U + 16U + type_name.size() +
                    texts.size() * (8U + 24U));
    encoded.append(fingerprint_domain);
    encoded.push_back('\0');
    append_u64(encoded, static_cast<std::uint64_t>(type_name.size()));
    encoded.append(type_name);
    append_u64(encoded, static_cast<std::uint64_t>(texts.size()));
    for (const auto &text : texts) {
        append_u64(encoded, static_cast<std::uint64_t>(text.size()));
        encoded.append(text);
    }

    const auto bytes{
        std::as_bytes(std::span<const char>{encoded.data(), encoded.size()})};
    return blake3(bytes);
}

void append_dump_prefix(std::string &out, registry_id id,
                        const resource_location &location) {
    out.append(id.to_string());
    out.push_back('\t');
    out.append(location.namespace_part());
    out.push_back(':');
    out.append(location.path_part());
}

} // namespace detail

} // namespace refrain::data
