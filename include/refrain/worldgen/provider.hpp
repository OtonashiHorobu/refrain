// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The value providers shared by carver configuration, feature placement and
// structures: integer, float and height ranges. Decoding accepts the inline
// constant forms the data pack uses and rejects unknown provider types.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <memory>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen::provider {

class float_provider;

/// The generation bounds height anchors resolve against.
struct generation_context {
    jint min_y{-64};
    jint height{384};
    jint sea_level{63};
    jlong world_seed{0};
};

[[nodiscard]] auto make_constant_float(float value)
    -> std::unique_ptr<float_provider>;

class int_provider {
  public:
    int_provider() = default;
    int_provider(const int_provider &) = delete;
    auto operator=(const int_provider &) -> int_provider & = delete;
    int_provider(int_provider &&) = delete;
    auto operator=(int_provider &&) -> int_provider & = delete;
    virtual ~int_provider() = default;

    [[nodiscard]] virtual auto sample(worldgen_random &random) const
        -> jint = 0;

    /// Decodes an integer provider; an inline integer is a constant.
    [[nodiscard]] static auto decode(const data::json::value &value)
        -> result<std::unique_ptr<int_provider>>;
};

class float_provider {
  public:
    float_provider() = default;
    float_provider(const float_provider &) = delete;
    auto operator=(const float_provider &) -> float_provider & = delete;
    float_provider(float_provider &&) = delete;
    auto operator=(float_provider &&) -> float_provider & = delete;
    virtual ~float_provider() = default;

    [[nodiscard]] virtual auto sample(worldgen_random &random) const
        -> float = 0;

    [[nodiscard]] static auto decode(const data::json::value &value)
        -> result<std::unique_ptr<float_provider>>;
};

class height_provider {
  public:
    height_provider() = default;
    height_provider(const height_provider &) = delete;
    auto operator=(const height_provider &) -> height_provider & = delete;
    height_provider(height_provider &&) = delete;
    auto operator=(height_provider &&) -> height_provider & = delete;
    virtual ~height_provider() = default;

    [[nodiscard]] virtual auto sample(worldgen_random &random,
                                      const generation_context &context) const
        -> jint = 0;

    [[nodiscard]] static auto decode(const data::json::value &value)
        -> result<std::unique_ptr<height_provider>>;
};

} // namespace refrain::worldgen::provider
