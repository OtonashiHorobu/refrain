// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Emitter entry points. Both emitters are pure functions: (model, schema hash)
// -> in-memory files with '/'-separated paths relative to the output root.

#pragma once

#include <schema/emitter_common.hpp>
#include <schema/sema.hpp>

#include <string_view>
#include <vector>

namespace refrain::schema {

[[nodiscard]] auto emit_ridl(const model &program, std::string_view schema_hash)
    -> std::vector<emit::generated_file>;

[[nodiscard]] auto emit_rsc(const model &program, std::string_view schema_hash)
    -> std::vector<emit::generated_file>;

/// One `custom_traits<T>` index over every generated C++ struct/record (RSC +
/// RIDL), emitted as part of the C++ output set. It is the only generated file
/// that may include `<refrain/reflect.hpp>`; the per-module headers stay free
/// of `REFL_AUTO`/`REFR_REFLECT` and of backend includes. The body is compiled
/// out when `REFRAIN_USE_REFLECTION` is disabled.
[[nodiscard]] auto emit_reflection(const model &program,
                                   std::string_view schema_hash)
    -> emit::generated_file;

} // namespace refrain::schema
