// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Process-wide allocation backend.
//
// When REFRAIN_WITH_MIMALLOC and REFRAIN_MIMALLOC_OVERRIDE_NEW are enabled this
// translation unit installs mimalloc as the global operator new/delete pair
// through the vendor-supported hook from <mimalloc-new-delete.h>. Global
// replacement operators must be defined in exactly one translation unit, which
// is why this lives in its own file.

#if REFRAIN_WITH_MIMALLOC && REFRAIN_MIMALLOC_OVERRIDE_NEW
#include <mimalloc-new-delete.h>
#endif
