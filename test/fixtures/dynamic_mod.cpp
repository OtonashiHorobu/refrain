// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Minimal first-party mod built as a shared module: the same registration
// macro the static mods use, compiled with REFR_MOD_DYNAMIC_BUILD so it exports
// the module descriptor instead of running a static initializer. The factory
// does nothing: the loader contract under test is the exported descriptor and
// the host-side registration, not provider behaviour.

#include <refrain/mod/static_mod.hpp>

namespace {

auto make_test_dynamic_mod(refrain::mod::host &) -> refrain::status {
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.test.dynamic", "1.0.0", 1, make_test_dynamic_mod);
