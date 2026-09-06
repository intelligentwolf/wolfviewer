/**
 * @file wolfwakefield.h
 * @brief WolfViewer: world-locked boat wash / wake field for the region water.
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

#ifndef WOLF_WAKEFIELD_H
#define WOLF_WAKEFIELD_H

#include "llsingleton.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llpointer.h"
#include <map>

class LLViewerObject;

// Source: wolfstorm/js/world/wake_field.js WakeField — the same technique and numbers.
// A WORLD-LOCKED field over the agent's region: moving hulls stamp into an off-screen
// RGBA8 texture as they move (prop wash astern, the Kelvin V at asin(1/3), the bow churn),
// a decay+diffuse pass ages it, and the water shader samples it for foam and displacement,
// so the wake belongs to the surface rather than floating over it. Firestorm has no
// viewer-side wake at all (a wake in SL is whatever the boat's own scripts emit).
class WolfWakeField : public LLSingleton<WolfWakeField>
{
    LLSINGLETON(WolfWakeField);
    ~WolfWakeField();

public:
    static constexpr S32 RES = 512;
    static constexpr F32 FULL_SPEED = 9.0f;      // m/s
    static constexpr F32 MIN_SPEED = 0.45f;      // m/s
    static constexpr F32 FOAM_HALF_LIFE = 7.0f;
    static constexpr F32 CREST_HALF_LIFE = 2.2f;
    static constexpr S32 MAX_BOATS = 6;
    static constexpr S32 MAX_STAMPS_PER_FRAME = 48;
    static constexpr F32 MAX_STRENGTH = 0.25f;

    /** Once per frame from the water pool: decay, then stamp every boat's path. */
    void update(F32 dt);
    bool isReady() const { return mReady && mAllocated; }
    LLRenderTarget* texture() { return mAllocated ? &mRT[mCur] : nullptr; }
    F32 getRegionSizeX() const { return mRegionSizeX; }
    F32 getRegionSizeY() const { return mRegionSizeY; }
    /** Reset to clean water (region change, kill switch). */
    void clear();
    void release();

private:
    struct BoatState { F32 mX = 0.f; F32 mY = 0.f; };

    bool allocate();
    static bool isBoat(LLViewerObject* obj, F32 water_z);
    S32 stampBoat(LLViewerObject* obj, S32 budget);

    LLRenderTarget mRT[2];
    S32 mCur = 0;
    bool mAllocated = false;
    bool mFailed = false;
    bool mReady = false;
    LLPointer<LLVertexBuffer> mQuadVB;
    std::map<U32, BoatState> mBoats;
    U64 mRegionHandle = 0;
    F32 mRegionSizeX = 256.f;
    F32 mRegionSizeY = 256.f;
    F32 mIdleSecs = 0.f;
};

#endif // WOLF_WAKEFIELD_H
