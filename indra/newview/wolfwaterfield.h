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
#include <set>
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
        U32 mDepthTex = 0;      // GL texture, RES x RES RGBA32F over the FIELD (mX0, mY0, mSizeX, mSizeY)
        U32 mExpoTex = 0;       // GL texture, ERES x ERES RGBA32F over the 3x span: R exposure, G distance to land, B distance from the open sea
        // <WolfViewer 2026-09-20> The depth field's region-space origin and size: (0, 0) and
        // the region up to WINDOW_M an edge, else a WINDOW_M window that follows the camera
        // (fieldWindow). Shader uniform depthOrigin / depthRegionSize.
        F32 mX0 = 0.f;
        F32 mY0 = 0.f;
        F32 mSizeX = 256.f;
        F32 mSizeY = 256.f;
        F32 mWaterLevel = 20.f;
        F32 mExpoX0 = -256.f;   // region-space origin and size of the exposure span
        F32 mExpoY0 = -256.f;
        F32 mExpoSX = 768.f;
        F32 mExpoSY = 768.f;
        // <WolfViewer 2026-09-21> The deep-water surf wavenumber the A-channel optical path
        // was integrated with. The shader multiplies the path by THIS, not by whatever the
        // sliders say right now, so phase and path can never disagree while a rebake is due.
        // 0 = this field has no surf path (no region record, or surf switched off).
        F32 mSurfK0 = 0.f;
        U64 mStamp = 0;
        // <WolfViewer 2026-09-20> the newest terrain stamp seen and when it first appeared: a
        // terrain change re-bakes only once the stamp has held for STAMP_SETTLE_SECS.
        U64 mPendingStamp = 0;
        F64 mPendingSince = 0.0;
        F64 mBakedAt = 0.0;
        bool mReady = false;
        // CPU copies of the two bakes, for the boat rocker's wave sampler (wolfboatrock.cpp
        // mirrors the vertex stage on the CPU and needs the same fields). RES*RES*4 and
        // ERES*ERES floats — 1.3 MB per region.
        std::vector<F32> mDepth;    // RGBA per texel, row-major, y outer
        std::vector<F32> mExpo;     // R per texel
        // [WAVES 2026-09-07] The painted wave zones (wolfwavezones.cpp fill) over the SAME
        // span as the exposure bake, one texel per 16 m cell: surf 1, open 0.55, calm 0.15,
        // off 0. Sampled by waterV.glsl as wolfZoneField; the CPU copy feeds zoneAt().
        U32 mZoneTex = 0;
        S32 mZoneW = 0;
        S32 mZoneH = 0;
        F32 mZoneX0 = -256.f;
        F32 mZoneY0 = -256.f;
        F32 mZoneSX = 768.f;
        F32 mZoneSY = 768.f;
        std::vector<F32> mZone;
    };

    static constexpr S32 RES = 256;
    static constexpr S32 ERES = 256;
    // <WolfViewer 2026-09-21> Deep water for the surf dispersion: the depth used where no
    // region covers a texel, and the cap on the depth fed to the wavenumber. waterV.glsl uses
    // the same 30 m beyond the depth field. A 36 m wave has k0 h = 5.2 there, where Guo's
    // shallow-water correction is under 0.1 % — anything past ~20 m is deep and the exact
    // value cannot matter.
    static constexpr F32 DEEP_REF_M = 30.f;
    static constexpr F32 CHECK_INTERVAL_SECS = 2.f;
    static constexpr F32 REBAKE_SECS = 20.f;     // neighbours stream in over a minute
    static constexpr F32 MIN_REBAKE_SECS = 8.f;  // [2026-09-10] never re-shape the sea faster than this per region
    // <WolfViewer 2026-09-20> A terrain change is acted on only after the stamp has been
    // unchanged for this long. On Wolf Nation (51,200 m) patches stream in for as long as you
    // fly, so the stamp never stopped changing and the fields re-baked every MIN_REBAKE_SECS
    // — the sea re-shaped itself every 8 s, the "water flickers when I move" report (and the
    // Dire Wolf freeze of 09-10, which disabling fields above 4096 m had hidden). Now the sea
    // settles once, when the streaming pauses; a window move still re-bakes at once.
    static constexpr F32 STAMP_SETTLE_SECS = 10.f;
    static constexpr F32 STAMP_MARGIN_M = 256.f;   // patches this far outside the window still count (the 3x exposure span reads them)
    // <WolfViewer 2026-09-20> Fields bake over the whole region up to this many metres an
    // edge (8 m depth texels), else over a camera-following window this wide. Replaces the
    // 09-10 rule "no field above 4096 m" (RES texels would be > 16 m): Wolf Nation (51,200 m)
    // had no depth, no exposure and NO ZONE FIELD at all, so every stretch of water there
    // carried full open-sea swell and painted zones were ignored — Paul: "the ocean and
    // rivers don't look right on the 200x200 ... I draw on the water and it doesn't work".
    // WolfStorm terrain_manager.js WATER_FIELD_WINDOW_M same.
    static constexpr F32 WINDOW_M = 2048.f;
    /**
     * Where a region's field should sit right now: the whole region below WINDOW_M, else a
     * WINDOW_M window snapped to an eighth of itself and held inside the region, centred on
     * the camera — except that an existing field keeps its window while the camera is still
     * in the inner half of it, so the sea never re-shapes itself under a boat for a step.
     */
    static void fieldWindow(const LLViewerRegion* regionp, const Field* current, F32& x0, F32& y0, F32& sx, F32& sy);

    /** Every frame from LLAppViewer::idle(); bakes at most one region per check. */
    void idle();
    /** Drop every field (shutdown, GL reset). */
    void reset();
    /** [WAVES 2026-09-07] How many bakes have run, and the surf texel count of the agent region's last one (editor confirmation). */
    U32 bakeCount() const { return mBakes; }
    U32 lastSurfTexels() const { return mLastSurfTexels; }
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
    /** [WAVES 2026-09-07] Zone energy at region-relative (rx, ry): nearest texel, 0.55 outside. */
    static F32 zoneAt(const Field& f, F32 rx, F32 ry);
    /** [SURF 2026-09-07] Distance to land (exposure bake G), bilinear; 4000 outside the span. */
    static F32 distanceAt(const Field& f, F32 rx, F32 ry);
    /** <WolfViewer 2026-09-20/> Distance from the open sea (exposure bake B), bilinear; 4000 outside the span. */
    static F32 openDistanceAt(const Field& f, F32 rx, F32 ry);
    /**
     * <WolfViewer 2026-09-21/> The surf train's OPTICAL path from the open sea (exposure bake
     * A), bilinear; 40000 outside the span. Multiply by Field::mSurfK0 for the wave phase.
     */
    static F32 openPathAt(const Field& f, F32 rx, F32 ry);
    /**
     * <WolfViewer 2026-09-21/> The surf train's DEEP-WATER wavenumber: 2*pi over
     * max(surfLength, 12 * surfHeight), the same wavelength waterV.glsl builds.
     *
     * It takes no region because the surf parameters are not per region here: lldrawpoolwater
     * .cpp reads WolfWaveZones::current() — the AGENT's region — and sends those uniforms to
     * every water plane in the scene, neighbours included. The bake has to integrate the
     * optical path against the same wavelength the shader will draw with, so it asks the same
     * question. 0 = no region record, i.e. no surf train at all.
     */
    static F32 surfK0();
    /**
     * <WolfViewer 2026-09-21/> The local wavenumber of a wave of deep-water wavenumber k0 in
     * depth h. Guo (2002) Coastal Engineering 45, 71-74, restated by Fenton (2006) eq. (3):
     * k = k0 * (1 - exp(-(k0 h)^(5/4)))^(-2/5). Exact in the deep and shallow limits, 0.7 %
     * between. KEEP BYTE-IDENTICAL with waterV.glsl wolfSurfK() and the WolfStorm mirrors.
     */
    static F32 surfWavenumber(F32 k0, F32 h);
    /**
     * <WolfViewer 2026-09-21/> Green's-law shoaling gain Ks = sqrt(cg0 / cg), the factor a
     * wave's height grows by as it shoals from deep water into depth h, from conservation of
     * energy flux with the frequency held constant. KEEP BYTE-IDENTICAL with the shaders.
     */
    static F32 surfShoalGain(F32 k0, F32 k, F32 h);
    /** [WAVES 2026-09-07] Bake every field again at the next check (a layout arrived / was saved / is previewed). */
    void invalidate();

private:
    void bake(LLViewerRegion* regionp, Field& f);
    static U64 terrainStamp(LLViewerRegion* regionp);
    /** <WolfViewer 2026-09-20/> terrainStamp over the patches inside region rect (x0, y0, sx, sy) only: a 51,200 m region has 10 million patches, a 2048 m window 16 thousand. */
    static U64 terrainStampIn(LLViewerRegion* regionp, F32 x0, F32 y0, F32 sx, F32 sy);
    /** Terrain height at region-relative (px, py) metres, answering from this region or
     *  any live neighbour; false = no region covers the point (open water). */
    static bool heightAt(LLViewerRegion* regionp, F32 px, F32 py, F32& out);
    static void upload(U32& tex, S32 w, S32 h, U32 internal_format, U32 format, const F32* data);
    static void releaseField(Field& f);

    std::map<U64, Field> mFields;
    F64 mNextCheck = 0.0;
    // Scratch, reused across bakes.
    std::vector<F32> mH, mTmp, mSm, mDist, mOpen, mDepthData, mExpoData, mZoneData;   // <WolfViewer 2026-09-20/> mOpen
    std::vector<F32> mEDepth, mPath;   // <WolfViewer 2026-09-21/> depth per exposure texel, and the eikonal path baked from it
    bool mRebakeAll = false;   // [WAVES 2026-09-07] set by invalidate()
    U32 mBakes = 0;
    U32 mLastSurfTexels = 0;
};

#endif // WOLF_WATERFIELD_H
