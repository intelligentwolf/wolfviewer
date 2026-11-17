/**
 * @file wolfsunterrain.h
 * @brief WolfViewer: the terrain round the camera as height blocks, so the water can tell whether a hill or a mountain
 *        stands between a point on the water and the sun or moon.
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

#ifndef WOLF_SUN_TERRAIN_H
#define WOLF_SUN_TERRAIN_H

#include "llsingleton.h"
#include "llmath.h"
#include "v2math.h"
#include "v3dmath.h"

#include <vector>

// [T871 SUN BEHIND TERRAIN 2026-11-15] Ticket 871 (Jimmy Olsen, TROMSO): "sun is behind mountain, yet one sees sun reflection
// in the water" - on the Wolf sky and on EEP skies alike. The sun's (or moon's) glint on the water was hidden by NOTHING but
// the sun shadow map: class3/environment/waterF.glsl multiplies the glint by `shadow`, which is sampleDirectionalShadow() only
// under HAS_SUN_SHADOW (RenderShadowDetail >= 1; the Low to Mid presets have it 0, featuretable_linux.txt), and even then only
// as far as the shadow map reaches (pipeline.cpp generateSunShadow: far_clip at most 512 m; shadowUtil.glsl
// sampleDirectionalShadow returns 1, "lit beyond the far split"). Stock Firestorm's waterF.glsl:250-255/325 is the same.
// Seen on Xvfb on TROMSO with the sun behind the mountain: a bright glint path on the fjord with shadows off, none with them on.
//
// So the water asks the terrain itself, which the viewer has for every region it is connected to (LLSurface): from the
// water's own point, toward the light, how high does the ground rise (the terrain horizon), and how much of the light's
// disc is above it. These are the heights it marches over: BLOCKS camera-centred windows of N x N texels, finer near the
// camera, each texel the HIGHEST drawn terrain in its own square (the drawn terrain is triangles between grid points, so the
// highest grid point of every grid cell touching the square bounds it; a patch's own maximum where a texel is as big as a
// patch), held in global space so a recentre keeps what it had. One R32F texture, the blocks stacked in rows.
class WolfSunTerrain : public LLSingleton<WolfSunTerrain>
{
    LLSINGLETON(WolfSunTerrain);
    ~WolfSunTerrain();

public:
    static constexpr S32 BLOCKS = 4;
    static constexpr S32 N = 256;                     // texels an edge, every block (waterF.glsl WOLF_SUN_TERRAIN_N)
    static const F32 TEXEL_M[BLOCKS];                 // 1, 4, 16, 64 m: windows of 256, 1024, 4096 and 16384 m
    static constexpr F32 NO_TERRAIN = -10000.f;       // a texel no live region covers: lower than any light can be hidden by
    static constexpr F64 TERRAIN_INTERVAL_SECS = 1.0; // a terrain edit is re-rastered at most this often (as WolfWaterClearance)

    // The light's disc, angular radius in radians. Source: wolfenvironmentrender.cpp:181 WOLF_SUN_RADIUS = 0.004675
    // (three-geospatial AtmosphereParameters.ts:102 sunAngularRadius) and wolfenvironmentrender.cpp:173 WOLF_MOON_RADIUS
    // = 0.5181 * 0.5 deg (the Moon's mean angular diameter, 31.1 arcmin) - the discs the native sky draws.
    static constexpr F32 SUN_RADIUS_RAD = 0.004675f;
    static constexpr F32 MOON_RADIUS_RAD = 0.5181f * 0.5f * DEG_TO_RAD;

    /** Every frame from LLAppViewer::idle(): recentre, and re-raster at most one block a frame (finest first). */
    void idle();
    void reset();

    /** At least one block is uploaded. */
    bool isReady() const;
    /** Block b is uploaded (its texels and origin can be used). */
    bool blockReady(S32 b) const { return b >= 0 && b < BLOCKS && mUploaded[b]; }
    U32 texture() const { return mTex; }
    /** Block b's texel-grid SW corner in agent space this frame (the grid is held in global space). */
    LLVector2 blockOriginAgent(S32 b) const;
    /** The highest terrain any uploaded block holds (agent z = region z), NO_TERRAIN when none. */
    F32 highestZ() const;

private:
    void raster(S32 b);
    void upload(S32 b);
    U64 terrainStamp() const;

    LLVector3d mOrigin[BLOCKS];                       // global SW corner of each block's texel grid
    LLVector3d mPubOrigin[BLOCKS];                    // the corner of what the texture holds for block b (mUploaded)
    bool mHaveOrigin[BLOCKS] = { false, false, false, false };
    bool mValid[BLOCKS] = { false, false, false, false };      // rastered for this origin and the terrain stamp
    bool mUploaded[BLOCKS] = { false, false, false, false };   // the texture holds block b, at mPubOrigin[b]
    U64 mStamp[BLOCKS] = { 0, 0, 0, 0 };
    F64 mStampAt[BLOCKS] = { 0.0, 0.0, 0.0, 0.0 };
    F32 mMaxZ[BLOCKS] = { NO_TERRAIN, NO_TERRAIN, NO_TERRAIN, NO_TERRAIN };
    std::vector<F32> mBuf;
    U32 mTex = 0;
    bool mTexAllocated = false;
};

#endif // WOLF_SUN_TERRAIN_H
