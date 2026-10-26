/**
 * @file wolfcloudclearance.h
 * @brief WolfViewer: the build clearance field for the native clouds - clouds never cover anyone's builds.
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

#ifndef WOLF_CLOUD_CLEARANCE_H
#define WOLF_CLOUD_CLEARANCE_H

#include "llsingleton.h"
#include "v2math.h"
#include "v3dmath.h"
#include <vector>

/**
 * [CLOUDS CLEAR OF BUILDS 2026-10-26] Paul, after testing the native sky: low cloud covered the tops of some buildings.
 * His rule (approved 10-26): CLOUDS NEVER COVER ANYONE'S BUILDS - the clouds stay at their weather heights (the lifted
 * condensation level etc., wolfcloudgenus.cpp), but their density FADES to zero in and around builds: towers, castles,
 * skyboxes and sky platforms (up to 4,000 m and more) are never inside cloud. Ground fog (the Fog genus) is weather you
 * walk through and is not faded (wolfcloudgenus.cpp toCloudLayers, cloudsCommon.glsl wolfLayerBuildFade).
 *
 * The field. Three camera-centred levels of N x N texels (TEXEL_M 4 / 16 / 64 m: windows of 1, 4 and 16 km; the
 * coarse ones only matter for builds far away, which a few tens of metres of extra clearance does not show), held in
 * global space and recentred like WolfWaterClearance (wolfwaterfield.h, the waves' camera-centred clearance field: the
 * same blocks, footprints and once-a-second rebuild). Every prim of a build (not avatars, not attachments, not linksets
 * moving faster than MOVING_MPS - vehicles under way, as the waves field) marks the texels its world box covers, in one
 * of two groups:
 *   ground  its bottom within GROUND_ATTACH_M of the terrain under it: the cloud is cleared from the ground up to its top;
 *   sky     anything higher (skyboxes, platforms): cleared only between its bottom and top, so the cloud deck under and
 *           over a skybox stays (a cushion of clear air round it, not a shaft down to the ground).
 * Channels (RGBA16F, ROWS = LEVELS x N rows):
 *   R  D   distance to the nearest marked texel (two-pass chamfer, weights / CHAMFER so it never exceeds the Euclidean
 *          distance), minus sqrt(2) x the texel (the most a bilinear read inside a marked texel can see), floored at 0
 *   G  Tg  the highest ground-group top within the window radius WIN(k) of the texel (agent z, metres), + PAD_M
 *   B  Ts  the highest sky-group top within the window, + PAD_M
 *   A  Bs  the lowest sky-group bottom within the window, - PAD_M
 * (missing: Tg = Ts = -SENTINEL, Bs = +SENTINEL). The shader (cloudsCommon.glsl wolfBuildClear) and the CPU twin clearAt():
 *   dg = |(D, max(z - Tg, 0))|, ds = |(D, max(z - Ts, Bs - z, 0))|, clear = 1 - smoothstep(M0, M0 + W, min(dg, ds)),
 * and every cloud layer that fades multiplies its density by 1 - clear: a rounded cushion round each build's box, fully clear
 * within M0 of it and back to the weather's own cloud W further out - thinning, never a box-shaped hole.
 * THE INVARIANT: inside any build's box clear = 1, so the cloud density there is 0 - in the shader and the shadow pass alike.
 * Why it holds for the bilinear read: a point in a build lies in a marked texel; the four texels around it are that texel
 * and neighbours at most a diagonal away, whose chamfer D is at most sqrt(2) t, so all four store D = 0 after the
 * subtraction; the window radius is at least one texel, so all four hold Tg (or Ts / Bs) at least as wide as the build's
 * own top / bottom; PAD_M covers the half-float rounding (|z| < 16,384 m: half an ulp is at most 8 m). selfCheck() tests
 * it numerically every rebuild (the corners and centre of every source box through clearAt()) and logs any miss.
 * M0 = 20 m: the cloud's finest structure - takram's shape-detail noise (Worley, 2 to 16 cells per 1 / 0.006 m = 167 m,
 * cloudShapeDetailF.glsl, wolftkclouds.cpp shapeDetailRepeat) - is ~10-20 m, so a gap narrower than that would still let a
 * detail wisp touch a wall; 20 m is the smallest gap the cloud itself can show. W = 50 m: the cloud march's minimum step
 * (takram qualityPresets.ts minStepSize 50, the medium and high presets the viewer uses), so the fade is always resolved by
 * at least one sample: no ring, no aliased hole.
 * Why not one object scan with WolfWaterClearance: that scan keeps only prims within its waterline band (BAND_M) and runs
 * only where the Wolf sea shows; this one needs every prim's full box and runs wherever the native clouds draw. Sharing
 * would have meant changing the waves' scan; it is left byte-for-byte as it was (one walk of the object list a second each).
 */
class WolfCloudClearance : public LLSingleton<WolfCloudClearance>
{
    LLSINGLETON(WolfCloudClearance);
    ~WolfCloudClearance();

public:
    static constexpr S32 LEVELS = 3;
    static constexpr S32 N = 256;
    static constexpr S32 ROWS = LEVELS * N;
    static const F32 TEXEL_M[LEVELS];                 // 4, 16, 64 m
    // [2026-10-26] M0 and W are read from the cloud table (app_settings/wolf_clouds.xml constants.build_clear_m0_m / _w_m; 20 / 50 m
    // shipped, the reasons above) and set by WolfEnvironmentRender::loadShaders through setFade; the shader takes them as the
    // uniform wolfBuildFade (cloudsCommon.glsl), so the CPU twin (clearAt, windowTexels) and the shader always agree
    static F32 M0;                                    // fully clear within this of a build (see above)
    static F32 W;                                     // then back to the weather's cloud over this
    static void setFade(F32 m0, F32 w) { M0 = llmax(m0, 0.f); W = llmax(w, 1.f); }
    static constexpr F32 GROUND_ATTACH_M = 50.f;      // a prim whose bottom is within this of the terrain is a ground build
    static constexpr F32 PAD_M = 8.f;                 // half-float rounding (half an ulp below 16,384 m)
    static constexpr F32 SENTINEL = 60000.f;          // "none" (inside half-float range)
    static constexpr F32 CHAMFER = 1.0824f;           // as WolfWaterClearance::CHAMFER
    static constexpr F32 MOVING_MPS = 0.5f;           // as WolfWaterClearance::MOVING_MPS
    static constexpr F64 INTERVAL_SECS = 1.0;         // the builds are re-rastered this often

    /** Once a frame from the clouds' frame (wolfenvironmentrender.cpp cloudsFrame); rebuilds once a second. */
    void idle();
    void reset();
    bool isReady() const { return mReady; }
    U32 texture() const { return mTex; }
    /** Level k's texel-grid corner in agent space this frame. */
    LLVector2 levelOriginAgent(S32 k) const;
    /** The highest cleared height (agent z): above this the shader skips the field. -SENTINEL = no builds. */
    F32 topAgentZ() const { return mTopZ; }
    /** CPU twin of cloudsCommon.glsl wolfBuildClear at agent (x, y, z): 1 = no cloud. */
    F32 clearAt(F32 ax, F32 ay, F32 az) const;

private:
    struct Box { LLVector3d lo, hi; };   // global
    void raster(const LLVector3d& agent_origin);
    void finishLevel(S32 k);
    void upload();
    void selfCheck(const LLVector3d& agent_origin);
    bool sampleLevel(S32 k, F32 ax, F32 ay, F32 out[4]) const;

    LLVector3d mOrigin[LEVELS];
    bool mHaveOrigin[LEVELS] = { false, false, false };
    std::vector<F32> mTg[LEVELS], mTs[LEVELS], mBs[LEVELS];   // raw per texel, then windowed
    std::vector<U8> mMark[LEVELS];
    std::vector<F32> mD[LEVELS];
    std::vector<F32> mTmp, mUpload;
    std::vector<Box> mBoxes;   // this rebuild's sources (selfCheck)
    U32 mTex = 0;
    bool mTexAllocated = false;
    bool mReady = false;
    F32 mTopZ = -SENTINEL;
    F64 mNext = 0.0;
    U32 mChecks = 0, mMisses = 0;
    F64 mNextMissLog = 0.0;
};

#endif // WOLF_CLOUD_CLEARANCE_H
