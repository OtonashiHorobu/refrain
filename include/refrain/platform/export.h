// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Symbol visibility for shared-library entry points. Branching on the target
// OS belongs to the platform layer; every other header includes this one.

#ifndef REFRAIN_PLATFORM_EXPORT_H
#define REFRAIN_PLATFORM_EXPORT_H

#if defined(_WIN32)
#define REFRAIN_EXPORT __declspec(dllexport)
#else
#define REFRAIN_EXPORT __attribute__((visibility("default")))
#endif

#endif /* REFRAIN_PLATFORM_EXPORT_H */
