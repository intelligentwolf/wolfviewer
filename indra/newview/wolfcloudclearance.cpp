/**
 * @file wolfcloudclearance.cpp
 * @brief WolfViewer: the build clearance field for the native clouds (see wolfcloudclearance.h).
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

#include "llviewerprecompiledheaders.h"

#include "wolfcloudclearance.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewercontrol.h"   // [t1003 2026-11-22] gSavedSettings (WolfEnvironmentTrace)

#include <cmath>
#include <cstring>   // [W114 FRAME RATE 2026-11-26/] memcpy (the field hash)
#include <utility>   // [FAR DRAW PERF 2026-11-14/] std::swap (swapPendingAll)
#include <algorithm>   // [t1003 2026-11-22] std::sort (traceColumn)
#include <sstream>     // [t1003 2026-11-22] traceColumn

const F32 WolfCloudClearance::TEXEL_M[WolfCloudClearance::LEVELS] = { 4.f, 16.f, 64.f };
F32 WolfCloudClearance::M0 = 20.f;   // [2026-10-26] the cloud table's build_clear_m0_m (setFade)
F32 WolfCloudClearance::W = 50.f;    // [2026-10-26] the cloud table's build_clear_w_m
// [t1003 2026-11-22] the sky bands' edges (wolfcloudclearance.h THE SKY BANDS): doubling heights, so two sky builds windowed into
// one band are at most a factor of two apart in height; 4,000 m is the top of the highest cloud base the genus table uses (Ns)
const F32 WolfCloudClearance::SKY_BAND_TOP[WolfCloudClearance::SKY_BANDS - 1] = { 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f };

namespace
{
    // GLSL smoothstep (t clamped, t^2 (3 - 2t)), as cloudsCommon.glsl wolfBuildClear uses it
    F32 smooth01(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }
    // the texels of the window (in texels) round a texel that the T / B channels take their max / min over: at least the
    // fade's reach M0 + W plus the bilinear allowance sqrt(2) t, plus one texel - and never less than one texel (wolfcloudclearance.h)
    S32 windowTexels(F32 t)
    {
        return (S32)ceilf((WolfCloudClearance::M0 + WolfCloudClearance::W + F_SQRT2 * t) / t) + 1;
    }
}

WolfCloudClearance::WolfCloudClearance()
{
}

WolfCloudClearance::~WolfCloudClearance()
{
    reset();
}

void WolfCloudClearance::reset()
{
    if (mTex)
    {
        LLImageGL::deleteTextures(1, &mTex);
        mTex = 0;
    }
    mTexAllocated = false;
    mReady = false;
    for (S32 k = 0; k < LEVELS; ++k) mHaveOrigin[k] = false;
    mTopZ = -SENTINEL;
    mNext = 0.0;
    // [FAR DRAW PERF 2026-11-14] the builds read and being read, and a rebuild under way, go with the rest
    if (WolfObjectSweep::instanceExists())
    {
        WolfObjectSweep::instance().cancel(this);
    }
    mPrims.clear();
    mPassPrims.clear();
    mPrimOrigins.clear();
    mPassOrigins.clear();
    mPrimsFresh = false;
    mHavePass = false;
    mPassRegion = 0;
    mPrimsRegion = 0;
    mStage = -1;
}

LLVector2 WolfCloudClearance::levelOriginAgent(S32 k) const
{
    const LLVector3d agent_origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    return LLVector2((F32)(mOrigin[k].mdV[VX] - agent_origin.mdV[VX]), (F32)(mOrigin[k].mdV[VY] - agent_origin.mdV[VY]));
}

void WolfCloudClearance::idle()
{
    LL_PROFILE_ZONE_SCOPED;
    if (!gAgent.getRegion()) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    U32 moved = 0;   // [W114 2026-11-26] a bit per level whose window was recentred (was one flag for all)
    for (S32 k = 0; k < LEVELS; ++k)
    {
        // recentred as WolfWaterClearance::idle: when the camera is an eighth of the window from its centre, the corner
        // snapped to the texel grid
        const F64 t = TEXEL_M[k];
        const F64 half = N * t * 0.5;
        if (!mHaveOrigin[k]
            || fabs(cam.mdV[VX] - (mOrigin[k].mdV[VX] + half)) > half * 0.25
            || fabs(cam.mdV[VY] - (mOrigin[k].mdV[VY] + half)) > half * 0.25)
        {
            mOrigin[k].mdV[VX] = floor((cam.mdV[VX] - half) / t) * t;
            mOrigin[k].mdV[VY] = floor((cam.mdV[VY] - half) / t) * t;
            mOrigin[k].mdV[VZ] = 0.0;
            mHaveOrigin[k] = true;
            moved |= 1u << k;
        }
    }
    // [FAR DRAW PERF 2026-11-14] The builds come from the shared object pass (INTERVAL_SECS in wolfcloudclearance.h). A recentre
    // (or no field yet) rebuilds at once from the last finished pass's builds, as before (a staged rebuild under way was made
    // for the old window and is dropped); a newly finished pass is rebuilt over the next frames (advanceStage).
    // [review 2] no pass to trust round the camera yet (wolfcloudclearance.h INTERVAL_SECS): one whole walk now, so no cloud
    // ever stands over a build while a pass reads them
    const U64 region = gAgent.getRegion()->getHandle();
    const bool walk = !mHavePass || region != mPrimsRegion;
    if (walk)
    {
        WolfObjectSweep::instance().walkNow(this);
    }
    // [W114 FRAME RATE 2026-11-26] (the Aurora review, 26 Nov: the whole field - 3 levels of 256^2 texels, 16 channels - was
    // rebuilt and uploaded every second, and in one frame at a recentre, even when nothing had changed.) A recentre of some
    // levels over the builds the published field already shows (no fresh pass waiting, no staged rebuild under way, no
    // walk) rebuilds and uploads only those levels: the others would come out the same. A finished pass whose builds
    // and windows make the same field key as the published field is dropped: its rebuild would give the same field.
    if (moved && mReady && !walk && mStage < 0 && !mPrimsFresh)
    {
        rebuildNow(moved);
    }
    else if (moved || !mReady || walk)
    {
        rebuildNow();
    }
    else if (mStage >= 0)
    {
        advanceStage();
    }
    else if (mPrimsFresh)
    {
        mPrimsFresh = false;
        const U64 key = fieldKey();
        if (key != mFieldKey)
        {
            mPendingKey = key;
            swapPendingAll();
            raster();
            swapPendingAll();
            mStage = 0;
        }
    }
    // [t1003 2026-11-22] the column over the camera, for a trace log (wolfcloudclearance.h traceColumn)
    static LLCachedControl<bool> trace(gSavedSettings, "WolfEnvironmentTrace", false);
    static F64 s_nextColumn = 0.0;
    if (trace && mReady && now >= s_nextColumn)
    {
        s_nextColumn = now + 10.0;
        traceColumn();
    }
    if (now >= mNext && !WolfObjectSweep::instance().busy(this))
    {
        mNext = now + INTERVAL_SECS;
        WolfObjectSweep::instance().request(this);
    }
}

// [FAR DRAW PERF 2026-11-14]
void WolfCloudClearance::rebuildNow(U32 levels)
{
    mStage = -1;
    mPrimsFresh = false;
    const LLVector3d agent_origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    raster(levels);
    for (S32 k = 0; k < LEVELS; ++k)
    {
        if (levels & (1u << k)) finishLevel(k);
    }
    upload(levels);
    mReady = mTex != 0;
    mFieldKey = fieldKey();
    selfCheck(agent_origin);
}

// [W114 FRAME RATE 2026-11-26] FNV-1a over 64-bit words, then a final mix (splitmix64's): the window corners, then each prim's
// box as raster() places it in global metres (lo, hi: the F32 agent box plus its pass origin, in F64) and its ground flag
U64 WolfCloudClearance::fieldKey() const
{
    U64 h = 1469598103934665603ULL;
    auto mixWord = [&h](U64 w) { h ^= w; h *= 1099511628211ULL; };
    auto mixF64 = [&mixWord](F64 v) { U64 w; memcpy(&w, &v, sizeof(w)); mixWord(w); };
    for (S32 k = 0; k < LEVELS; ++k)
    {
        mixF64(mOrigin[k].mdV[VX]);
        mixF64(mOrigin[k].mdV[VY]);
    }
    mixWord((U64)mPrims.size());
    for (const PrimBox& pb : mPrims)
    {
        const LLVector3d& ao = mPrimOrigins[pb.mOrigin];
        mixF64(pb.mLo.mV[VX] + ao.mdV[VX]);
        mixF64(pb.mLo.mV[VY] + ao.mdV[VY]);
        mixF64(pb.mLo.mV[VZ]);
        mixF64(pb.mHi.mV[VX] + ao.mdV[VX]);
        mixF64(pb.mHi.mV[VY] + ao.mdV[VY]);
        mixF64(pb.mHi.mV[VZ]);
        mixWord(pb.mGround ? 1u : 0u);
    }
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

void WolfCloudClearance::swapPendingLevel(S32 k)
{
    mTg[k].swap(mTgP[k]);
    for (S32 b = 0; b < SKY_BANDS; ++b)
    {
        mTs[k][b].swap(mTsP[k][b]);
        mBs[k][b].swap(mBsP[k][b]);
    }
    mMark[k].swap(mMarkP[k]);
    mD[k].swap(mDP[k]);
}

void WolfCloudClearance::swapPendingAll()
{
    for (S32 k = 0; k < LEVELS; ++k) swapPendingLevel(k);
    mBoxes.swap(mBoxesP);
    std::swap(mTopZ, mTopZP);
}

// [FAR DRAW PERF 2026-11-14] The staged rebuild after a finished pass: raster() ran into the pending arrays in the frame it
// began, then one level is finished a frame (finishLevel reads and writes only level k and the scratch mTmp), then the
// pending field is published whole, uploaded and checked. Until then the shader and clearAt read the previous field.
void WolfCloudClearance::advanceStage()
{
    if (mStage < LEVELS)
    {
        swapPendingLevel(mStage);
        finishLevel(mStage);
        swapPendingLevel(mStage);
        ++mStage;
        return;
    }
    swapPendingAll();
    upload();
    mReady = mTex != 0;
    mFieldKey = mPendingKey;
    selfCheck(gAgent.getPosGlobalFromAgent(LLVector3::zero));
    mStage = -1;
}

// [FAR DRAW PERF 2026-11-14] The builds are read in the shared object pass: visit() is the old raster loop's filter, box and
// ground test (the shared pass already leaves out the dead, what is not LL_PCODE_VOLUME - avatars among them -, attachments
// and HUD attachments), kept as a PrimBox with the agent origin of the frame it was read in.
void WolfCloudClearance::beginPass()
{
    mPassPrims.clear();
    mPassOrigins.clear();
    mPassRegion = gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0;
}

void WolfCloudClearance::beginSlice()
{
    mSliceOrigin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    if (mPassOrigins.empty() || mPassOrigins.back() != mSliceOrigin)
    {
        mPassOrigins.push_back(mSliceOrigin);
    }
}

void WolfCloudClearance::visit(LLViewerObject* o)
{
    if (!o->getRegion())
    {
        return;
    }
    LLViewerObject* root = o->getRootEdit();
    if (!root || root->isAvatar() || root->isAttachment() || root->isHUDAttachment())
    {
        return;
    }
    if (root->getVelocity().lengthSquared() > MOVING_MPS * MOVING_MPS)
    {
        return;
    }
    // the prim's world box (agent space; the box of its rotated scale, a superset of the prim)
    const LLVector3 c = o->getPositionAgent();
    const LLVector3 half = o->getScale() * 0.5f;
    const LLQuaternion rot = o->getRotationRegion();
    LLVector3 lo(SENTINEL, SENTINEL, SENTINEL), hi(-SENTINEL, -SENTINEL, -SENTINEL);
    for (S32 n = 0; n < 8; ++n)
    {
        const LLVector3 corner((n & 1) ? half.mV[VX] : -half.mV[VX],
                               (n & 2) ? half.mV[VY] : -half.mV[VY],
                               (n & 4) ? half.mV[VZ] : -half.mV[VZ]);
        const LLVector3 p = c + corner * rot;
        lo.set(llmin(lo.mV[VX], p.mV[VX]), llmin(lo.mV[VY], p.mV[VY]), llmin(lo.mV[VZ], p.mV[VZ]));
        hi.set(llmax(hi.mV[VX], p.mV[VX]), llmax(hi.mV[VY], p.mV[VY]), llmax(hi.mV[VZ], p.mV[VZ]));
    }
    LLViewerRegion* region = o->getRegion();
    const F32 ground = region->getLandHeightRegion(region->getPosRegionFromAgent(LLVector3(c.mV[VX], c.mV[VY], lo.mV[VZ])));
    PrimBox pb;
    pb.mLo = lo;
    pb.mHi = hi;
    pb.mGround = lo.mV[VZ] <= ground + GROUND_ATTACH_M;
    pb.mOrigin = (U32)(mPassOrigins.size() - 1);   // beginSlice pushed this frame's
    mPassPrims.push_back(pb);
}

void WolfCloudClearance::endPass()
{
    mPrims.swap(mPassPrims);
    mPrimOrigins.swap(mPassOrigins);
    mPassPrims.clear();
    mPassOrigins.clear();
    mPrimsFresh = true;
    mHavePass = true;
    mPrimsRegion = mPassRegion;
}

void WolfCloudClearance::raster(U32 levels)
{
    const size_t NN = (size_t)N * N;
    for (S32 k = 0; k < LEVELS; ++k)
    {
        if (!(levels & (1u << k))) continue;   // [W114 2026-11-26/] left as it is
        mTg[k].assign(NN, -SENTINEL);
        for (S32 b = 0; b < SKY_BANDS; ++b)
        {
            mTs[k][b].assign(NN, -SENTINEL);
            mBs[k][b].assign(NN, SENTINEL);
        }
        mMark[k].assign(NN, 0);
    }
    mBoxes.clear();
    mTopZ = -SENTINEL;
    // the same prims as WolfWaterClearance::rasterObjects (wolfwaterfield.cpp): volumes, no avatars, nothing worn or sat on,
    // no linkset moving faster than MOVING_MPS - [FAR DRAW PERF 2026-11-14] chosen in visit(), in the shared object pass; each
    // is placed with the agent origin of the frame it was read in, the one the old loop read it with
    for (const PrimBox& pb : mPrims)
    {
        const LLVector3d& agent_origin = mPrimOrigins[pb.mOrigin];
        const LLVector3& lo = pb.mLo;
        const LLVector3& hi = pb.mHi;
        const bool groundBuild = pb.mGround;
        mTopZ = llmax(mTopZ, hi.mV[VZ]);
        Box b;
        b.lo.setVec(lo.mV[VX] + agent_origin.mdV[VX], lo.mV[VY] + agent_origin.mdV[VY], lo.mV[VZ]);
        b.hi.setVec(hi.mV[VX] + agent_origin.mdV[VX], hi.mV[VY] + agent_origin.mdV[VY], hi.mV[VZ]);
        mBoxes.push_back(b);
        for (S32 k = 0; k < LEVELS; ++k)
        {
            if (!(levels & (1u << k))) continue;   // [W114 2026-11-26/]
            const F32 t = TEXEL_M[k];
            const F64 ox = mOrigin[k].mdV[VX] - agent_origin.mdV[VX], oy = mOrigin[k].mdV[VY] - agent_origin.mdV[VY];
            const S32 ti0 = llmax(0, (S32)floor((lo.mV[VX] - ox) / t)), ti1 = llmin(N - 1, (S32)floor((hi.mV[VX] - ox) / t));
            const S32 tj0 = llmax(0, (S32)floor((lo.mV[VY] - oy) / t)), tj1 = llmin(N - 1, (S32)floor((hi.mV[VY] - oy) / t));
            for (S32 tj = tj0; tj <= tj1; ++tj)
            {
                for (S32 ti = ti0; ti <= ti1; ++ti)
                {
                    const size_t idx = (size_t)tj * N + ti;
                    mMark[k][idx] = 1;
                    if (groundBuild)
                    {
                        mTg[k][idx] = llmax(mTg[k][idx], hi.mV[VZ]);
                    }
                    else
                    {
                        // [t1003 2026-11-22] the box's part in each sky band it reaches (the parts' union is the box)
                        for (S32 b = 0; b < SKY_BANDS; ++b)
                        {
                            const F32 bandLo = b > 0 ? SKY_BAND_TOP[b - 1] : -SENTINEL;
                            const F32 bandHi = b < SKY_BANDS - 1 ? SKY_BAND_TOP[b] : SENTINEL;
                            if (hi.mV[VZ] < bandLo || lo.mV[VZ] > bandHi) continue;
                            mTs[k][b][idx] = llmax(mTs[k][b][idx], llmin(hi.mV[VZ], bandHi));
                            mBs[k][b][idx] = llmin(mBs[k][b][idx], llmax(lo.mV[VZ], bandLo));
                        }
                    }
                }
            }
        }
    }
}

// The window max / min of the heights (separable square windows: a superset of the round reach, so conservative) and the
// chamfer distance of the marks, then the margins folded in (wolfcloudclearance.h).
void WolfCloudClearance::finishLevel(S32 k)
{
    const F32 t = TEXEL_M[k];
    const S32 r = windowTexels(t);
    const size_t NN = (size_t)N * N;
    // separable, stamping from the texels that hold a value (a few per cent of the window in a town): the cost follows the
    // builds, not the window (a full 256^2 x 41-tap pass a second was ~10 ms on the main thread)
    auto windowed = [&](std::vector<F32>& v, bool takeMax)
    {
        const F32 none = takeMax ? -SENTINEL : SENTINEL;
        mTmp.assign(NN, none);
        for (S32 y = 0; y < N; ++y)
        {
            for (S32 x = 0; x < N; ++x)
            {
                const F32 w = v[(size_t)y * N + x];
                if (w == none) continue;
                for (S32 dx = llmax(0, x - r); dx <= llmin(N - 1, x + r); ++dx)
                {
                    F32& m = mTmp[(size_t)y * N + dx];
                    m = takeMax ? llmax(m, w) : llmin(m, w);
                }
            }
        }
        v.assign(NN, none);
        for (S32 y = 0; y < N; ++y)
        {
            for (S32 x = 0; x < N; ++x)
            {
                const F32 w = mTmp[(size_t)y * N + x];
                if (w == none) continue;
                for (S32 dy = llmax(0, y - r); dy <= llmin(N - 1, y + r); ++dy)
                {
                    F32& m = v[(size_t)dy * N + x];
                    m = takeMax ? llmax(m, w) : llmin(m, w);
                }
            }
        }
    };
    windowed(mTg[k], true);
    for (S32 b = 0; b < SKY_BANDS; ++b)
    {
        windowed(mTs[k][b], true);
        windowed(mBs[k][b], false);
    }
    // chamfer distance of the marked texels: WolfWaterClearance::solve's D (wolfwaterfield.cpp), the same weights
    std::vector<F32>& D = mD[k];
    D.resize(NN);
    for (size_t i = 0; i < NN; ++i) D[i] = mMark[k][i] ? 0.f : SENTINEL;
    const F32 a = t / CHAMFER, b = t * F_SQRT2 / CHAMFER;
    auto relax = [&](size_t i, size_t j, F32 w) { const F32 d = D[j] + w; if (d < D[i]) D[i] = d; };
    for (S32 y = 0; y < N; ++y)
    {
        for (S32 x = 0; x < N; ++x)
        {
            const size_t i = (size_t)y * N + x;
            if (x > 0) relax(i, i - 1, a);
            if (y > 0)
            {
                relax(i, i - N, a);
                if (x > 0) relax(i, i - N - 1, b);
                if (x < N - 1) relax(i, i - N + 1, b);
            }
        }
    }
    for (S32 y = N - 1; y >= 0; --y)
    {
        for (S32 x = N - 1; x >= 0; --x)
        {
            const size_t i = (size_t)y * N + x;
            if (x < N - 1) relax(i, i + 1, a);
            if (y < N - 1)
            {
                relax(i, i + N, a);
                if (x < N - 1) relax(i, i + N + 1, b);
                if (x > 0) relax(i, i + N - 1, b);
            }
        }
    }
    // the margins: D a lower bound for the bilinear read (minus sqrt(2) t), the heights widened by the half-float rounding
    for (size_t i = 0; i < NN; ++i)
    {
        D[i] = D[i] < SENTINEL * 0.5f ? llmax(D[i] - F_SQRT2 * t, 0.f) : SENTINEL;
        if (mTg[k][i] > -SENTINEL * 0.5f) mTg[k][i] += PAD_M;
        for (S32 b = 0; b < SKY_BANDS; ++b)
        {
            if (mTs[k][b][i] > -SENTINEL * 0.5f) mTs[k][b][i] += PAD_M;
            if (mBs[k][b][i] < SENTINEL * 0.5f) mBs[k][b][i] -= PAD_M;
        }
    }
}

// ROWS x (PAGES x N) RGBA16F (level k in rows k N ..; page 0 R D, G Tg, B Ts0, A Bs0, page p the bands 2p - 1 and 2p: R Ts,
// G Bs, B Ts, A Bs - wolfcloudclearance.h), allocated once, then replaced whole each rebuild.
void WolfCloudClearance::upload(U32 levels)
{
    const size_t W_TEX = (size_t)PAGES * N;
    mUpload.resize(W_TEX * ROWS * 4);
    if (!mTex || !mTexAllocated) levels = ALL_LEVELS;   // [W114 2026-11-26/] the first upload is the whole texture
    for (S32 k = 0; k < LEVELS; ++k)
    {
        if (!(levels & (1u << k))) continue;   // [W114 2026-11-26/] its rows on the texture stand
        for (S32 y = 0; y < N; ++y)
        {
            F32* row = &mUpload[((size_t)k * N + y) * W_TEX * 4];
            for (S32 x = 0; x < N; ++x)
            {
                const size_t i = (size_t)y * N + x;
                F32 ch[CHANNELS];
                ch[0] = mD[k][i];
                ch[1] = mTg[k][i];
                for (S32 b = 0; b < SKY_BANDS; ++b)
                {
                    ch[2 + 2 * b] = mTs[k][b][i];
                    ch[3 + 2 * b] = mBs[k][b][i];
                }
                for (S32 p = 0; p < PAGES; ++p)
                {
                    F32* dst = &row[((size_t)p * N + x) * 4];
                    for (S32 c = 0; c < 4; ++c) dst[c] = ch[p * 4 + c];
                }
            }
        }
    }
    if (!mTex) mTexAllocated = false;
    if (!mTexAllocated)
    {
        // [VK S6 2026-11-26] in Vulkan-owned memory when the Vulkan renderer runs (LLImageGL::wolfVkManualImage2D makes
        // the name when there is none and leaves the texture bound on unit 0), else as before
        LLImageGL::wolfVkManualImage2D(mTex, GL_RGBA16F, PAGES * N, ROWS, GL_RGBA, GL_FLOAT, mUpload.data());
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        mTexAllocated = true;
    }
    else if (levels == ALL_LEVELS)
    {
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTex);
        if (gGLManager.mIsNativeVulkan)
        {
            if (LLImageGL::sWolfVkNativeStore)
            {
                LLImageGL::sWolfVkNativeStore->uploadSubImage2D(mTex, 0, 0, 0, PAGES * N, ROWS, PAGES * N, GL_RGBA16F, GL_RGBA, GL_FLOAT, mUpload.data(), false);
            }
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PAGES * N, ROWS, GL_RGBA, GL_FLOAT, mUpload.data());
        }
    }
    else
    {
        // [W114 2026-11-26] level k is rows k N .. k N + N - 1, whole width (the layout above)
        for (S32 k = 0; k < LEVELS; ++k)
        {
            if (!(levels & (1u << k))) continue;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, k * N, PAGES * N, N, GL_RGBA, GL_FLOAT, &mUpload[(size_t)k * N * W_TEX * 4]);
        }
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// GL's bilinear read of level k at agent (ax, ay) (texel centres at (i + 0.5) t, clamped), or false outside the usable part
// (two texels in from each edge, so a read never reaches the next level's rows).
bool WolfCloudClearance::sampleLevel(S32 k, F32 ax, F32 ay, F32 out[CHANNELS]) const
{
    const LLVector2 o = levelOriginAgent(k);
    const F32 t = TEXEL_M[k];
    const F32 tx = (ax - o.mV[VX]) / t, ty = (ay - o.mV[VY]) / t;
    if (tx < 2.f || ty < 2.f || tx > N - 2.f || ty > N - 2.f) return false;
    const F32 sx = tx - 0.5f, sy = ty - 0.5f;
    const S32 i0 = llclamp((S32)floorf(sx), 0, N - 1), j0 = llclamp((S32)floorf(sy), 0, N - 1);
    const S32 i1 = llmin(i0 + 1, N - 1), j1 = llmin(j0 + 1, N - 1);
    const F32 fx = sx - floorf(sx), fy = sy - floorf(sy);
    const std::vector<F32>* ch[CHANNELS];
    ch[0] = &mD[k];
    ch[1] = &mTg[k];
    for (S32 b = 0; b < SKY_BANDS; ++b)
    {
        ch[2 + 2 * b] = &mTs[k][b];
        ch[3 + 2 * b] = &mBs[k][b];
    }
    for (S32 c = 0; c < CHANNELS; ++c)
    {
        const std::vector<F32>& v = *ch[c];
        const F32 a = v[(size_t)j0 * N + i0] * (1.f - fx) + v[(size_t)j0 * N + i1] * fx;
        const F32 b = v[(size_t)j1 * N + i0] * (1.f - fx) + v[(size_t)j1 * N + i1] * fx;
        out[c] = a * (1.f - fy) + b * fy;
    }
    return true;
}

// The CPU twin of cloudsCommon.glsl wolfBuildClear (keep in step): the finest level that holds the point, blended into the
// next over the last tenth of its half-window, the outermost faded out over its own last tenth.
F32 WolfCloudClearance::clearAt(F32 ax, F32 ay, F32 az) const
{
    if (!mReady) return 0.f;
    auto clearOf = [&](const F32 s[CHANNELS])
    {
        const F32 dvg = llmax(az - s[1], 0.f);
        F32 d = sqrtf(s[0] * s[0] + dvg * dvg);
        for (S32 b = 0; b < SKY_BANDS; ++b)   // [t1003 2026-11-22] each sky band's slab on its own
        {
            const F32 dvs = llmax(llmax(az - s[2 + 2 * b], s[3 + 2 * b] - az), 0.f);
            d = llmin(d, sqrtf(s[0] * s[0] + dvs * dvs));
        }
        return 1.f - smooth01(M0, M0 + W, d);
    };
    for (S32 k = 0; k < LEVELS; ++k)
    {
        F32 s[CHANNELS];
        if (!sampleLevel(k, ax, ay, s)) continue;
        const LLVector2 o = levelOriginAgent(k);
        const F32 t = TEXEL_M[k];
        const F32 tx = (ax - o.mV[VX]) / t, ty = (ay - o.mV[VY]) / t;
        const F32 edge = llmin(llmin(tx - 2.f, (N - 2.f) - tx), llmin(ty - 2.f, (N - 2.f) - ty));
        const F32 blend = 1.f - smooth01(0.f, N * 0.1f, edge);
        F32 c = clearOf(s);
        if (blend > 0.f)
        {
            F32 s2[CHANNELS];
            const F32 next = (k + 1 < LEVELS && sampleLevel(k + 1, ax, ay, s2)) ? clearOf(s2) : 0.f;
            c = c + (next - c) * blend;
        }
        return c;
    }
    return 0.f;
}

// THE INVARIANT, checked numerically every rebuild: the corners and centre of every source box (agent space) that lies
// where the field is not blending out (the outermost level's last tenth) must read clear = 1 through the CPU twin.
void WolfCloudClearance::selfCheck(const LLVector3d& agent_origin)
{
    // [W114 FRAME RATE 2026-11-26] a diagnostic only (nothing drawn reads it): the first check of the session, then only with
    // WolfEnvironmentTrace on (up to 4,096 boxes x 9 clearAt reads at each rebuild otherwise)
    static LLCachedControl<bool> trace(gSavedSettings, "WolfEnvironmentTrace", false);
    if (mChecks > 0 && !trace)
    {
        return;
    }
    U32 points = 0, misses = 0;
    const LLVector2 o = levelOriginAgent(LEVELS - 1);
    const F32 t = TEXEL_M[LEVELS - 1];
    // at most CHECK_BOXES boxes a rebuild (a rotating stride over the list), so a crowded sim costs a bounded few ms a second
    const size_t CHECK_BOXES = 4096;
    const size_t stride = llmax((size_t)1, mBoxes.size() / CHECK_BOXES);
    for (size_t bi = mChecks % stride; bi < mBoxes.size(); bi += stride)
    {
        const Box& b = mBoxes[bi];
        const F32 lx = (F32)(b.lo.mdV[VX] - agent_origin.mdV[VX]), ly = (F32)(b.lo.mdV[VY] - agent_origin.mdV[VY]);
        const F32 hx = (F32)(b.hi.mdV[VX] - agent_origin.mdV[VX]), hy = (F32)(b.hi.mdV[VY] - agent_origin.mdV[VY]);
        const F32 lz = (F32)b.lo.mdV[VZ], hz = (F32)b.hi.mdV[VZ];
        for (S32 n = 0; n < 9; ++n)
        {
            const F32 x = n == 8 ? 0.5f * (lx + hx) : ((n & 1) ? hx : lx);
            const F32 y = n == 8 ? 0.5f * (ly + hy) : ((n & 2) ? hy : ly);
            const F32 z = n == 8 ? 0.5f * (lz + hz) : ((n & 4) ? hz : lz);
            const F32 tx = (x - o.mV[VX]) / t, ty = (y - o.mV[VY]) / t;
            const F32 edge = llmin(llmin(tx - 2.f, (N - 2.f) - tx), llmin(ty - 2.f, (N - 2.f) - ty));
            if (edge < N * 0.1f) continue;   // outside the field's full reach (wolfcloudclearance.h)
            ++points;
            if (clearAt(x, y, z) < 0.999f) ++misses;
        }
    }
    ++mChecks;
    mMisses += misses;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (misses > 0 && now >= mNextMissLog)
    {
        mNextMissLog = now + 60.0;
        LL_WARNS("WolfEnv") << "cloud build clearance: " << misses << " of " << points << " build points not clear of cloud ("
                            << mBoxes.size() << " prims)" << LL_ENDL;
    }
    if (mChecks == 1)
    {
        LL_INFOS("WolfEnv") << "cloud build clearance: first check " << points << " points of " << mBoxes.size()
                            << " prims, " << misses << " not clear" << LL_ENDL;
    }
}

// [t1003 2026-11-22] WolfEnvironmentTrace: the field over the camera. For heights up the column, clearAt and, from the finest level
// holding the point, which term is nearest (ground, or sky band b) with its slab; then the builds within 150 m of the column whose
// boxes rise highest (agent z), with their group. Ticket 1003 (TROMSO): the whole low deck was gone over the town, and the log
// had no way to say whether a build's cushion took it.
void WolfCloudClearance::traceColumn() const
{
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    std::ostringstream o;
    o << "cloud build clearance over the camera (" << cam.mV[VX] << ", " << cam.mV[VY] << "), top " << mTopZ << ":";
    static const F32 HEIGHTS[] = { 100.f, 150.f, 200.f, 300.f, 400.f, 500.f, 700.f, 1000.f, 1500.f, 2000.f, 3000.f, 4000.f };
    for (F32 z : HEIGHTS)
    {
        o << " z" << z << " " << clearAt(cam.mV[VX], cam.mV[VY], z);
        F32 s[CHANNELS];
        for (S32 k = 0; k < LEVELS; ++k)
        {
            if (!sampleLevel(k, cam.mV[VX], cam.mV[VY], s)) continue;
            F32 best = sqrtf(s[0] * s[0] + llmax(z - s[1], 0.f) * llmax(z - s[1], 0.f));
            S32 which = -1;
            for (S32 b = 0; b < SKY_BANDS; ++b)
            {
                const F32 dv = llmax(llmax(z - s[2 + 2 * b], s[3 + 2 * b] - z), 0.f);
                const F32 d = sqrtf(s[0] * s[0] + dv * dv);
                if (d < best) { best = d; which = b; }
            }
            o << " (L" << k << " D " << s[0] << " d " << best << " by ";
            if (which < 0) o << "ground Tg " << s[1] << ")";
            else o << "sky band " << which << " " << s[3 + 2 * which] << ".." << s[2 + 2 * which] << ")";
            break;
        }
    }
    LL_INFOS("WolfEnvTrace") << o.str() << LL_ENDL;
    // the highest builds near the column (the last finished pass), at most 8
    const LLVector3d agent_origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    std::vector<std::pair<F32, size_t>> nearby;
    for (size_t i = 0; i < mPrims.size(); ++i)
    {
        const PrimBox& pb = mPrims[i];
        const LLVector3d& po = mPrimOrigins[pb.mOrigin];
        const F32 dx = (F32)(po.mdV[VX] - agent_origin.mdV[VX]), dy = (F32)(po.mdV[VY] - agent_origin.mdV[VY]);
        const F32 lx = pb.mLo.mV[VX] + dx, hx = pb.mHi.mV[VX] + dx, ly = pb.mLo.mV[VY] + dy, hy = pb.mHi.mV[VY] + dy;
        const F32 ex = llmax(llmax(lx - cam.mV[VX], cam.mV[VX] - hx), 0.f), ey = llmax(llmax(ly - cam.mV[VY], cam.mV[VY] - hy), 0.f);
        if (ex * ex + ey * ey > 150.f * 150.f) continue;
        nearby.emplace_back(pb.mHi.mV[VZ], i);
    }
    std::sort(nearby.begin(), nearby.end(), [](const std::pair<F32, size_t>& a, const std::pair<F32, size_t>& b) { return a.first > b.first; });
    std::ostringstream p;
    p << "cloud build clearance: " << nearby.size() << " builds within 150 m of the column; highest:";
    for (size_t j = 0; j < nearby.size() && j < 8; ++j)
    {
        const PrimBox& pb = mPrims[nearby[j].second];
        const LLVector3d& po = mPrimOrigins[pb.mOrigin];
        const F32 dx = (F32)(po.mdV[VX] - agent_origin.mdV[VX]), dy = (F32)(po.mdV[VY] - agent_origin.mdV[VY]);
        p << " [" << (pb.mGround ? "ground" : "sky") << " x " << pb.mLo.mV[VX] + dx << ".." << pb.mHi.mV[VX] + dx
          << " y " << pb.mLo.mV[VY] + dy << ".." << pb.mHi.mV[VY] + dy << " z " << pb.mLo.mV[VZ] << ".." << pb.mHi.mV[VZ] << "]";
    }
    LL_INFOS("WolfEnvTrace") << p.str() << LL_ENDL;
}
