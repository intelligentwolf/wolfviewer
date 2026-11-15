/**
 * @file wolfprefetch.h
 * @brief WolfViewer: a cache prefetch hint for loops that walk thousands of objects through pointers.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_PREFETCH_H
#define WOLF_PREFETCH_H

// [W107 PIPELINE 2026-11-14] wolf_prefetch(p) asks the processor to start loading the cache line that holds *p. It reads
// nothing and changes nothing: a hint only, and a wrong or null address is harmless (the instruction never faults -
// Source: GCC manual "__builtin_prefetch": "data prefetch does not generate faults if addr is invalid"; Intel SDM
// PREFETCHh: "does not cause any exceptions"). Used a few elements ahead in loops whose cost is the wait for memory
// (LLPipeline::calcNearbyLights and the local light loop of renderDeferredLighting: 6,430 lights at Paddington Station,
// each reached through light -> drawable -> object -> light parameters).
#if defined(__GNUC__) || defined(__clang__)
// GCC and Clang, every processor (Linux x86-64, macOS x86-64 and arm64)
#define wolf_prefetch(p) __builtin_prefetch((const void*)(p))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
// Source: llcommon/llmemory.h:373 uses the same intrinsic under MSVC
#include <xmmintrin.h>
#define wolf_prefetch(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#else
#define wolf_prefetch(p) ((void)0)
#endif

#endif // WOLF_PREFETCH_H
