// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Game data versions understood by this build.
//
// A world records the data version its records were written with. The storage
// model, the chunk model and the world generator all interpret the fields of
// one version; a different version is either upgraded before the store opens
// (see `refrain/world/migration.hpp`) or refused, never read as if it were the
// current one.

#pragma once

#include <cstdint>

namespace refrain::world {

/// Game data version this build writes and interprets (Minecraft 26.3).
inline constexpr std::int32_t target_data_version = 5023;

} // namespace refrain::world
