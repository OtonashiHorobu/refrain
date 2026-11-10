// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::net {

struct registry_entry {
    std::string name{};
    std::uint32_t id{0};
};

struct registry_definition {
    std::string key{};
    std::vector<registry_entry> entries{};
};

struct tag_definition {
    std::string registry{};
    std::string tag{};
    std::vector<std::uint32_t> entries{};
};

/// The network registry catalog generated from the local game data. Entries
/// carry names and protocol ids only; their NBT payloads are resolved by the
/// client from the known `minecraft:core` pack during configuration.
class registry_catalog {
  public:
    [[nodiscard]] static auto load(std::string_view path)
        -> result<registry_catalog>;

    [[nodiscard]] auto registries() const noexcept
        -> const std::vector<registry_definition> & {
        return registries_;
    }
    [[nodiscard]] auto tags() const noexcept
        -> const std::vector<tag_definition> & {
        return tags_;
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return registries_.empty();
    }

  private:
    std::vector<registry_definition> registries_{};
    std::vector<tag_definition> tags_{};
};

} // namespace refrain::net
