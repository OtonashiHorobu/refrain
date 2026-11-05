// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstddef>
#include <string_view>

namespace refrain::builtin {

// Embedded built-in content image (RFSI v1): one deterministic
// image holding every internal mod's manifest + data/assets/scripts, generated
// at build time by `refrain-tools builtin image --emit-cpp`. Implemented by the
// generated translation unit (build/<...>/generated/builtin_image.cpp); the
// image's bytes are immutable for the lifetime of the process.
//
// The image is *data only*: internal mod code is statically linked and
// registered through kernel/static_mod.hpp. The digest is the image file
// BLAKE3 (BLAKE3 over the header-excluded bytes; stable identity for
// diagnostics/mods.lock).

[[nodiscard]] auto image_data() noexcept -> const std::byte *;
[[nodiscard]] auto image_size() noexcept -> std::size_t;
/// Lowercase hex BLAKE3 of the embedded image (`builtin_image::digest()`).
[[nodiscard]] auto image_digest() noexcept -> std::string_view;

} // namespace refrain::builtin
