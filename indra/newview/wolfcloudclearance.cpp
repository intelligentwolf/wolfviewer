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

#include <cmath>

const F32 WolfCloudClearance::TEXEL_M[WolfCloudClearance::LEVELS] = { 4.f, 16.f, 64.f };
F32 WolfCloudClearance::M0 = 20.f;   // [2026-10-26] the cloud table's build_clear_m0_m (setFade)
F32 WolfCloudClearance::W = 50.f;    // [2026-10-26] the cloud table's build_clear_w_m

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
    bool moved = false;
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
            moved = true;
        }
    }
    if (!moved && mReady && now < mNext) return;
    mNext = now + INTERVAL_SECS;
    const LLVector3d agent_origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    raster(agent_origin);
    for (S32 k = 0; k < LEVELS; ++k) finishLevel(k);
    upload();
    mReady = mTex != 0;
    selfCheck(agent_origin);
}

void WolfCloudClearance::raster(const LLVector3d& agent_origin)
{
    const size_t NN = (size_t)N * N;
    for (S32 k = 0; k < LEVELS; ++k)
    {
        mTg[k].assign(NN, -SENTINEL);
        mTs[k].assign(NN, -SENTINEL);
        mBs[k].assign(NN, SENTINEL);
        mMark[k].assign(NN, 0);
    }
    mBoxes.clear();
    mTopZ = -SENTINEL;
    // the same prims as WolfWaterClearance::rasterObjects (wolfwaterfield.cpp): volumes, no avatars, nothing worn or sat on,
    // no linkset moving faster than MOVING_MPS
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME || o->isAvatar()
            || o->isAttachment() || o->isHUDAttachment() || !o->getRegion())
        {
            continue;
        }
        LLViewerObject* root = o->getRootEdit();
        if (!root || root->isAvatar() || root->isAttachment() || root->isHUDAttachment())
        {
            continue;
        }
        if (root->getVelocity().lengthSquared() > MOVING_MPS * MOVING_MPS)
        {
            continue;
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
        const bool groundBuild = lo.mV[VZ] <= ground + GROUND_ATTACH_M;
        mTopZ = llmax(mTopZ, hi.mV[VZ]);
        Box b;
        b.lo.setVec(lo.mV[VX] + agent_origin.mdV[VX], lo.mV[VY] + agent_origin.mdV[VY], lo.mV[VZ]);
        b.hi.setVec(hi.mV[VX] + agent_origin.mdV[VX], hi.mV[VY] + agent_origin.mdV[VY], hi.mV[VZ]);
        mBoxes.push_back(b);
        for (S32 k = 0; k < LEVELS; ++k)
        {
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
                        mTs[k][idx] = llmax(mTs[k][idx], hi.mV[VZ]);
                        mBs[k][idx] = llmin(mBs[k][idx], lo.mV[VZ]);
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
    windowed(mTs[k], true);
    windowed(mBs[k], false);
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
        if (mTs[k][i] > -SENTINEL * 0.5f) mTs[k][i] += PAD_M;
        if (mBs[k][i] < SENTINEL * 0.5f) mBs[k][i] -= PAD_M;
    }
}

// ROWS x N RGBA16F (R D, G Tg, B Ts, A Bs; level k in rows k N ..), allocated once, then replaced whole each rebuild.
void WolfCloudClearance::upload()
{
    mUpload.resize((size_t)N * ROWS * 4);
    for (S32 k = 0; k < LEVELS; ++k)
    {
        F32* dst = &mUpload[(size_t)k * N * N * 4];
        for (size_t i = 0; i < (size_t)N * N; ++i)
        {
            dst[i * 4] = mD[k][i];
            dst[i * 4 + 1] = mTg[k][i];
            dst[i * 4 + 2] = mTs[k][i];
            dst[i * 4 + 3] = mBs[k][i];
        }
    }
    if (!mTex)
    {
        LLImageGL::generateTextures(1, &mTex);
        mTexAllocated = false;
    }
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTex);
    if (!mTexAllocated)
    {
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RGBA16F,
                                  N, ROWS, GL_RGBA, GL_FLOAT, mUpload.data(), false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        mTexAllocated = true;
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, N, ROWS, GL_RGBA, GL_FLOAT, mUpload.data());
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// GL's bilinear read of level k at agent (ax, ay) (texel centres at (i + 0.5) t, clamped), or false outside the usable part
// (two texels in from each edge, so a read never reaches the next level's rows).
bool WolfCloudClearance::sampleLevel(S32 k, F32 ax, F32 ay, F32 out[4]) const
{
    const LLVector2 o = levelOriginAgent(k);
    const F32 t = TEXEL_M[k];
    const F32 tx = (ax - o.mV[VX]) / t, ty = (ay - o.mV[VY]) / t;
    if (tx < 2.f || ty < 2.f || tx > N - 2.f || ty > N - 2.f) return false;
    const F32 sx = tx - 0.5f, sy = ty - 0.5f;
    const S32 i0 = llclamp((S32)floorf(sx), 0, N - 1), j0 = llclamp((S32)floorf(sy), 0, N - 1);
    const S32 i1 = llmin(i0 + 1, N - 1), j1 = llmin(j0 + 1, N - 1);
    const F32 fx = sx - floorf(sx), fy = sy - floorf(sy);
    const std::vector<F32>* ch[4] = { &mD[k], &mTg[k], &mTs[k], &mBs[k] };
    for (S32 c = 0; c < 4; ++c)
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
    auto clearOf = [&](const F32 s[4])
    {
        const F32 dvg = llmax(az - s[1], 0.f);
        const F32 dvs = llmax(llmax(az - s[2], s[3] - az), 0.f);
        const F32 d = llmin(sqrtf(s[0] * s[0] + dvg * dvg), sqrtf(s[0] * s[0] + dvs * dvs));
        return 1.f - smooth01(M0, M0 + W, d);
    };
    for (S32 k = 0; k < LEVELS; ++k)
    {
        F32 s[4];
        if (!sampleLevel(k, ax, ay, s)) continue;
        const LLVector2 o = levelOriginAgent(k);
        const F32 t = TEXEL_M[k];
        const F32 tx = (ax - o.mV[VX]) / t, ty = (ay - o.mV[VY]) / t;
        const F32 edge = llmin(llmin(tx - 2.f, (N - 2.f) - tx), llmin(ty - 2.f, (N - 2.f) - ty));
        const F32 blend = 1.f - smooth01(0.f, N * 0.1f, edge);
        F32 c = clearOf(s);
        if (blend > 0.f)
        {
            F32 s2[4];
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
