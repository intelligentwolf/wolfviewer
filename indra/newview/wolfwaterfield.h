/**
 * @file wolfwaterfield.h
 * @brief WolfViewer: per-region water depth + open-water exposure fields for the water shader.
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

#ifndef WOLF_WATERFIELD_H
#define WOLF_WATERFIELD_H

#include "llsingleton.h"
#include <map>
#include <vector>

#include "llmath.h"

class LLViewerRegion;

// Source: wolfstorm/js/world/terrain/terrain_manager.js _bakeWaterDepthTexture() and
// _bakeSwellExposure() — the same two fields, the same layout, the same numbers.
//
// The water shader had no idea where the land was: its only depth came from the
// refraction buffer, per fragment, which cannot reach the VERTEX stage and knows nothing
// beyond the screen. These two textures give it what WolfStorm's water has had since
// August: the terrain height under every point of the region (DEPTH field, RGBA32F:
// R = height, GB = unit direction toward the beach scaled by a slope/fetch confidence,
// A = box-smoothed height for a stable wave phase), and how far each point is from any
// land over a 3x-region span, neighbours included (EXPOSURE field, R32F, smoothstep 6..150 m).
// From them: the swell calms toward every shore and dies in rivers, breakers rise and
// steepen as they shoal, the swash line pulses up the beach, the shallows tint.
class WolfWaterField : public LLSingleton<WolfWaterField>
{
    LLSINGLETON(WolfWaterField);
    ~WolfWaterField();

public:
    struct Field
    {
        U32 mDepthTex = 0;      // GL texture, RES x RES RGBA32F, region space 0..width
        U32 mExpoTex = 0;       // GL texture, ERES x ERES R32F over the 3x span
        F32 mSizeX = 256.f;
        F32 mSizeY = 256.f;
        F32 mWaterLevel = 20.f;
        F32 mExpoX0 = -256.f;   // region-space origin and size of the exposure span
        F32 mExpoY0 = -256.f;
        F32 mExpoSX = 768.f;
        F32 mExpoSY = 768.f;
        U64 mStamp = 0;
        F64 mBakedAt = 0.0;
        bool mReady = false;
        // CPU copies of the two bakes, for the boat rocker's wave sampler (wolfboatrock.cpp
        // mirrors the vertex stage on the CPU and needs the same fields). RES*RES*4 and
        // ERES*ERES floats — 1.3 MB per region.
        std::vector<F32> mDepth;    // RGBA per texel, row-major, y outer
        std::vector<F32> mExpo;     // R per texel
    };

    static constexpr S32 RES = 256;
    static constexpr S32 ERES = 256;
    static constexpr F32 CHECK_INTERVAL_SECS = 2.f;
    static constexpr F32 REBAKE_SECS = 20.f;     // neighbours stream in over a minute

    /** Every frame from LLAppViewer::idle(); bakes at most one region per check. */
    void idle();
    /** Drop every field (shutdown, GL reset). */
    void reset();
    /** The field for a region, or nullptr before its first bake. */
    const Field* get(const LLViewerRegion* regionp) const;

    /**
     * Open-water exposure at region-relative (rx, ry) metres: the nearest texel of the
     * exposure bake, 1.0 (open sea) outside its span.
     * Source: wolfstorm terrain_manager.js _swellExposureAt().
     */
    static F32 exposureAt(const Field& f, F32 rx, F32 ry);
    /**
     * The depth bake's nearest texel at region-relative (rx, ry): out[0] height, out[1..2]
     * beachward unit direction x confidence, out[3] box-smoothed height. False outside the
     * region. Source: terrain_manager.js _waveSampleCPU() depth-texture read.
     */
    static bool depthAt(const Field& f, F32 rx, F32 ry, F32 out[4]);

private:
    void bake(LLViewerRegion* regionp, Field& f);
    static U64 terrainStamp(LLViewerRegion* regionp);
    /** Terrain height at region-relative (px, py) metres, answering from this region or
     *  any live neighbour; false = no region covers the point (open water). */
    static bool heightAt(LLViewerRegion* regionp, F32 px, F32 py, F32& out);
    static void upload(U32& tex, S32 w, S32 h, U32 internal_format, U32 format, const F32* data);
    static void releaseField(Field& f);

    std::map<U64, Field> mFields;
    F64 mNextCheck = 0.0;
    // Scratch, reused across bakes.
    std::vector<F32> mH, mTmp, mSm, mDist, mDepthData, mExpoData;
};

#endif // WOLF_WATERFIELD_H
